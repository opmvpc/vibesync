# run-real-vm.ps1 -- seance reelle a deux clients C sur UNE machine Windows.
#
# Equivalent Windows de scripts/run-real-macos.sh. Deux instances de
# vibesync.exe, chacune avec son %APPDATA% (donc son vibesync.ini, son jeton de
# session et son journal), chacune avec son VRAI VLC, sur un vrai serveur. Le
# but est celui du harnais mac : verifier de bout en bout ce qu'aucun test
# unitaire ne peut prouver -- que deux VLC reels restent synchronises -- plus ce
# que VS-029 exige en propre :
#
#   * l'attache tient malgre un vlcrc facon Syncplay (extraintf/luaintf,
#     one-instance, mot de passe http fige) ET malgre un VLC deja ouvert ;
#   * les actions faites DANS VLC (pause, play, seek envoyes a l'interface HTTP
#     locale par CE script, donc hors du client) sont detectees et propagees.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run-real-vm.ps1 `
#       -Url wss://vibesync.choboai.com/ws
#
# Variables d'environnement :
#   VIBESYNC_PASSWORD  mot de passe du serveur (jamais en argument : argv est
#                      lisible par tous les processus de la machine)
#   VIBESYNC_ROOM      salle a utiliser (defaut : vibesync-vm-<aleatoire>)
#   VIBESYNC_VLC       chemin de vlc.exe (defaut : emplacements standards)
#
# Les deux instances tournent en mode auto (ui/win32/src/auto.c) : elles se
# connectent seules, publient leur etat dans un fichier JSON une-ligne et
# executent les commandes qu'on ajoute a leur fichier de commandes.
#
# EN SSH : un processus lance depuis une session sshd n'a pas de bureau ; VLC ne
# demarrera pas. Passer par une tache planifiee interactive :
#   schtasks /create /tn vs_real /tr "powershell -ExecutionPolicy Bypass -File
#            C:\...\scripts\run-real-vm.ps1" /sc once /st 00:00 /f
#   schtasks /run /tn vs_real
#
# Sortie : un PASS/FAIL par point de controle, code retour non nul si l'un
# echoue, journaux dans le dossier de travail affiche en fin de course.

param(
    [string]$Media = "",
    [string]$Url = "ws://127.0.0.1:8080/ws",
    [int]$MediaSeconds = 600,
    # Ne pas installer le vlcrc facon Syncplay (seance "environnement propre").
    [switch]$PlainVlcrc,
    # Ne pas ouvrir de VLC AVANT la seance (le piege one-instance).
    [switch]$NoPreexistingVlc,
    [switch]$Keep
)

$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"

$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "ui\win32\build\vibesync.exe"

$password = $env:VIBESYNC_PASSWORD
if (-not $password) { $password = "" }
$room = $env:VIBESYNC_ROOM
if (-not $room) { $room = "vibesync-vm-" + (Get-Random -Maximum 99999) }

# Bornes du scenario (secondes).
$CONNECT_TIMEOUT = 30
$FILE_TIMEOUT = 45
$STEP_TIMEOUT = 25
$HOLD_SEC = 5
$SEEK_TARGET = 120
$VLC_SEEK_TARGET = 300
$DRIFT_MAX = 0.5

$script:passed = 0
$script:failed = 0

function Say($m)  { Write-Host $m }
function Step($m) { Write-Host ""; Write-Host "== $m" }
function Pass($m) { $script:passed++; Write-Host "  PASS  $m" }
function Fail($m) { $script:failed++; Write-Host "  FAIL  $m" }
function Die($m)  { Write-Host "erreur : $m"; exit 2 }

# --- lecture de l'etat publie par les instances -----------------------------

# Read-State : l'application ecrit par fichier temporaire puis renommage, donc
# jamais de ligne tronquee -- mais le renommage lui-meme peut faire echouer une
# ouverture concurrente. On reessaie brievement.
function Read-State($path) {
    for ($i = 0; $i -lt 6; $i++) {
        try {
            $raw = [IO.File]::ReadAllText($path)
            if ($raw.Trim().Length -gt 0) { return ($raw | ConvertFrom-Json) }
        } catch {
            Start-Sleep -Milliseconds 60
        }
    }
    return $null
}

function S1 { Read-State $script:st1 }
function S2 { Read-State $script:st2 }

function Pos($s) { if ($s) { [double]$s.positionSec } else { 0 } }
function Gap {
    $a = Pos (S1); $b = Pos (S2)
    [Math]::Abs($a - $b)
}

function Snapshot {
    foreach ($i in 1, 2) {
        $s = if ($i -eq 1) { S1 } else { S2 }
        if (-not $s) { Say ("  client{0} : (pas d'etat)" -f $i); continue }
        Say ("  client{0} : phase={1,-9} vlc={2,-7} pos={3,8:F2} salle={4,8:F2} drift={5,7:F3} paused={6,-5} ready={7,-5} users={8} buf={9}" -f `
            $i, $s.phase, $s.vlcState, $s.positionSec, $s.roomPositionSec, $s.driftSec, $s.paused, $s.ready, $s.users, $s.buffering)
    }
}

# Wait-For <secondes> <description> <condition> : rend $true si la condition
# devient vraie avant l'echeance.
function Wait-For([int]$timeout, [string]$desc, [scriptblock]$cond) {
    $end = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $end) {
        try { if (& $cond) { return $true } } catch { }
        Start-Sleep -Milliseconds 400
    }
    Say "  (delai depasse : $desc)"
    return $false
}

# Holds : la condition doit rester vraie tout du long.
function Holds([int]$secs, [scriptblock]$cond) {
    $end = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $end) {
        try { if (-not (& $cond)) { return $false } } catch { return $false }
        Start-Sleep -Milliseconds 400
    }
    return $true
}

function Send-Cmd([string]$file, [string]$line) {
    Add-Content -Path $file -Value $line -Encoding ASCII
    Say "  -> $([IO.Path]::GetFileName($file)) : $line"
}

# Invoke-Vlc parle DIRECTEMENT a l'interface HTTP locale de VLC : ces requetes
# ne passent pas par le client, c'est donc exactement ce que fait un
# utilisateur qui appuie sur Espace dans la fenetre de VLC.
function Invoke-Vlc($state, [string]$command, [string]$val) {
    if (-not $state -or -not $state.vlcPort -or [int]$state.vlcPort -eq 0) {
        Say "  (interface VLC inconnue : le client n'a pas publie vlcPort)"
        return $false
    }
    $uri = "http://127.0.0.1:$($state.vlcPort)/requests/status.json?command=$command"
    if ($val -ne "") { $uri += "&val=$val" }
    $auth = "Basic " + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes(":" + $state.vlcPassword))
    try {
        Invoke-WebRequest -Uri $uri -Headers @{ Authorization = $auth } -UseBasicParsing -TimeoutSec 5 | Out-Null
        Say "  -> VLC (port $($state.vlcPort)) : $command $val"
        return $true
    } catch {
        Say "  (requete VLC en echec : $($_.Exception.Message))"
        return $false
    }
}

# --- media ------------------------------------------------------------------

# New-SilentWav : WAV PCM 8 bits, 8 kHz, mono. Meme recette que le harnais mac
# et que writeSilentWAV du harnais Go : VLC l'ouvre, en donne la duree, il est
# seekable et ne coute rien a produire (4,8 Mo pour 10 minutes).
function New-SilentWav([string]$path, [int]$secs) {
    $rate = 8000
    $data = $rate * $secs
    $fs = [IO.File]::Create($path)
    $bw = New-Object IO.BinaryWriter($fs)
    $bw.Write([Text.Encoding]::ASCII.GetBytes("RIFF"))
    $bw.Write([int](36 + $data))
    $bw.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $bw.Write([int]16)
    $bw.Write([int16]1)      # PCM
    $bw.Write([int16]1)      # mono
    $bw.Write([int]$rate)
    $bw.Write([int]$rate)    # octets/seconde
    $bw.Write([int16]1)      # alignement de bloc
    $bw.Write([int16]8)      # bits par echantillon
    $bw.Write([Text.Encoding]::ASCII.GetBytes("data"))
    $bw.Write([int]$data)
    $chunk = New-Object byte[] $rate
    for ($i = 0; $i -lt $rate; $i++) { $chunk[$i] = 128 }   # silence PCM 8 bits
    for ($i = 0; $i -lt $secs; $i++) { $bw.Write($chunk) }
    $bw.Close()
    $fs.Close()
}

# --- vlcrc facon Syncplay ---------------------------------------------------

# Le vlcrc que VS-029 doit survivre : interface lua principale ET
# supplementaire, instance unique (les deux variantes), enqueue, aleatoire,
# boucle, et un mot de passe d'interface http fige. Chacune de ces lignes casse
# une hypothese du lancement ; la ligne de commande doit toutes les couvrir.
$VLCRC_SYNCPLAY = @"
[core]
extraintf=luaintf
one-instance=1
one-instance-when-started-from-file=1
playlist-enqueue=1
started-from-file=1
random=1
loop=1
repeat=1
play-and-exit=1
video-title-show=1
[lua]
lua-intf=syncplay
lua-config=syncplay={port="4123"}
http-password=motdepassefigeparsyncplay
http-host=127.0.0.1
http-port=4123
"@

# --- verifications ----------------------------------------------------------

Step "verifications"
if (-not (Test-Path $exe)) {
    Say "  binaire absent, construction..."
    & (Join-Path $repo "ui\win32\build.bat") | Out-Null
}
if (-not (Test-Path $exe)) { Die "vibesync.exe introuvable : $exe" }

$vlc = $env:VIBESYNC_VLC
if (-not $vlc -or -not (Test-Path $vlc)) {
    foreach ($c in @("C:\Program Files\VideoLAN\VLC\vlc.exe",
                     "C:\Program Files (x86)\VideoLAN\VLC\vlc.exe",
                     "$env:LOCALAPPDATA\Programs\VideoLAN\VLC\vlc.exe")) {
        if (Test-Path $c) { $vlc = $c; break }
    }
}
if (-not $vlc -or -not (Test-Path $vlc)) { Die "VLC introuvable (renseignez VIBESYNC_VLC)" }
Say "  VLC     : $vlc"
Say "  client  : $exe"
Say "  serveur : $Url"
Say "  salle   : $room"

$work = Join-Path $env:TEMP ("vibesync-real-" + (Get-Random -Maximum 999999))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$script:st1 = Join-Path $work "c1.json"
$script:st2 = Join-Path $work "c2.json"
$cmd1 = Join-Path $work "c1.cmds"
$cmd2 = Join-Path $work "c2.cmds"
$log1 = Join-Path $work "c1.log"
$log2 = Join-Path $work "c2.log"
New-Item -ItemType File -Force -Path $cmd1, $cmd2 | Out-Null
Say "  travail : $work"

# --- media ------------------------------------------------------------------

Step "media"
if ($Media -ne "") {
    if (-not (Test-Path $Media)) { Die "fichier introuvable : $Media" }
    $media1 = (Resolve-Path $Media).Path
    $media2 = $media1
    Say "  fichier fourni : $media1 (les deux clients ouvrent la meme copie)"
} else {
    # Chacun sa copie, comme dans la vraie vie -- mais le MEME nom de base et la
    # meme duree, sinon le serveur signalerait une divergence de fichier.
    New-Item -ItemType Directory -Force -Path (Join-Path $work "media1"), (Join-Path $work "media2") | Out-Null
    $media1 = Join-Path $work "media1\vibesync-test.wav"
    $media2 = Join-Path $work "media2\vibesync-test.wav"
    New-SilentWav $media1 $MediaSeconds
    Copy-Item $media1 $media2
    Say "  WAV silencieux genere ($MediaSeconds s, une copie par client)"
}

# --- vlcrc ------------------------------------------------------------------

Step "configuration de VLC"
$vlcDir = Join-Path $env:APPDATA "vlc"
$vlcrc = Join-Path $vlcDir "vlcrc"
$vlcrcBackup = ""
if ($PlainVlcrc) {
    Say "  -PlainVlcrc : le vlcrc de la machine est laisse tel quel"
} else {
    New-Item -ItemType Directory -Force -Path $vlcDir | Out-Null
    if (Test-Path $vlcrc) {
        $vlcrcBackup = Join-Path $work "vlcrc.backup"
        Copy-Item $vlcrc $vlcrcBackup -Force
        Say "  vlcrc existant sauvegarde dans $vlcrcBackup"
    }
    Set-Content -Path $vlcrc -Value $VLCRC_SYNCPLAY -Encoding ASCII
    Say "  vlcrc facon Syncplay installe ($vlcrc)"
}

# --- lancement --------------------------------------------------------------

$script:p1 = $null
$script:p2 = $null
$script:preVlc = $null

function Cleanup {
    # Depart volontaire par la commande quit (c'est LUI qui envoie la close 1000
    # et arrete VLC), puis on ne tue que ce qui a survecu.
    foreach ($pair in @(@($script:p1, $cmd1), @($script:p2, $cmd2))) {
        $p = $pair[0]
        if ($p -and -not $p.HasExited) { Add-Content -Path $pair[1] -Value "quit" -Encoding ASCII }
    }
    $end = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $end) {
        $alive = @($script:p1, $script:p2) | Where-Object { $_ -and -not $_.HasExited }
        if ($alive.Count -eq 0) { break }
        Start-Sleep -Milliseconds 400
    }
    foreach ($p in @($script:p1, $script:p2)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    if ($script:preVlc -and -not $script:preVlc.HasExited) {
        Stop-Process -Id $script:preVlc.Id -Force -ErrorAction SilentlyContinue
    }
    # Les VLC lances par le test ouvrent NOS fichiers : aucun ne doit survivre.
    Get-CimInstance Win32_Process -Filter "Name='vlc.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($work) } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if ($vlcrcBackup -ne "") {
        Copy-Item $vlcrcBackup $vlcrc -Force -ErrorAction SilentlyContinue
    } elseif (-not $PlainVlcrc -and (Test-Path $vlcrc)) {
        Remove-Item $vlcrc -Force -ErrorAction SilentlyContinue
    }
}

# Le piege one-instance ne se declenche que s'il y a DEJA un VLC ouvert : c'est
# la situation du retour terrain (VLC lance a la main, ou reste d'une seance).
if (-not $NoPreexistingVlc) {
    Step "VLC prealable (piege one-instance)"
    $script:preVlc = Start-Process -FilePath $vlc -PassThru -ArgumentList @("--no-video-title-show", "`"$media1`"")
    Start-Sleep -Seconds 6
    Say "  VLC deja ouvert (pid $($script:preVlc.Id)) sur $media1"
}

# Launch : une instance isolee. %APPDATA% distinct -- indispensable, sans quoi
# les deux instances presentent le MEME jeton de session au serveur (VS-028) et
# se battent pour le meme vibesync.ini.
function Launch([int]$idx, [string]$name, [string]$media, [string]$status, [string]$cmds, [string]$log) {
    $homeDir = Join-Path $work "home$idx"
    New-Item -ItemType Directory -Force -Path $homeDir | Out-Null
    if (-not $PlainVlcrc) {
        # VLC est un ENFANT du client : il herite du %APPDATA% isole. Selon la
        # facon dont il resout son dossier de configuration (variable
        # d'environnement ou dossier connu du shell), il lira l'un ou l'autre ;
        # le vlcrc hostile est donc pose des deux cotes, sans quoi la moitie du
        # test s'evaporerait en silence.
        $iso = Join-Path $homeDir "vlc"
        New-Item -ItemType Directory -Force -Path $iso | Out-Null
        Set-Content -Path (Join-Path $iso "vlcrc") -Value $VLCRC_SYNCPLAY -Encoding ASCII
    }
    Remove-Item $status -Force -ErrorAction SilentlyContinue
    $saved = $env:APPDATA
    $env:APPDATA = $homeDir
    $env:VIBESYNC_VLC = $vlc
    $env:VIBESYNC_AUTO_URL = $Url
    $env:VIBESYNC_AUTO_NAME = $name
    $env:VIBESYNC_AUTO_ROOM = $room
    $env:VIBESYNC_AUTO_PASSWORD = $password
    $env:VIBESYNC_AUTO_FILE = $media
    $env:VIBESYNC_AUTO_STATUS = $status
    $env:VIBESYNC_AUTO_CMDS = $cmds
    $env:VIBESYNC_AUTO_SCENARIO = "client$idx"
    $p = Start-Process -FilePath $exe -PassThru -RedirectStandardError $log
    $env:APPDATA = $saved
    foreach ($v in "VIBESYNC_AUTO_URL", "VIBESYNC_AUTO_NAME", "VIBESYNC_AUTO_ROOM",
                   "VIBESYNC_AUTO_PASSWORD", "VIBESYNC_AUTO_FILE", "VIBESYNC_AUTO_STATUS",
                   "VIBESYNC_AUTO_CMDS", "VIBESYNC_AUTO_SCENARIO") {
        Remove-Item "env:$v" -ErrorAction SilentlyContinue
    }
    return $p
}

Step "lancement des deux clients"
$script:p1 = Launch 1 "alice" $media1 $script:st1 $cmd1 $log1
$script:p2 = Launch 2 "bob" $media2 $script:st2 $cmd2 $log2
Say "  pid client1=$($script:p1.Id)  client2=$($script:p2.Id)"

# --- points de controle -----------------------------------------------------

Step "(a) les deux clients sont connectes"
$ok = Wait-For $CONNECT_TIMEOUT "connexion des deux clients" {
    $a = S1; $b = S2
    ($a -and $b -and $a.connected -and $b.connected)
}
if ($ok) {
    Pass "connectes a $Url (salle $room)"
} else {
    $a = S1
    Fail "connexion impossible : $($a.connection) $($a.error)"
    Snapshot
    Cleanup
    Say ""
    Say "journaux : $work"
    exit 1
}
if (Wait-For $STEP_TIMEOUT "les deux membres se voient" { $a = S1; $b = S2; ($a.users -eq 2 -and $b.users -eq 2) }) {
    Pass "les deux membres se voient dans la salle"
} else {
    Fail "les membres ne se voient pas ($((S1).users) / $((S2).users))"
}

Step "(b) les deux VLC sont attaches et le fichier declare"
if (Wait-For $FILE_TIMEOUT "fichier declare des deux cotes" { $a = S1; $b = S2; ($a.fileDeclared -and $b.fileDeclared) }) {
    if ($PlainVlcrc) {
        Pass "VLC lance et fichier declare des deux cotes"
    } else {
        Pass "VLC lance et fichier declare des deux cotes MALGRE le vlcrc facon Syncplay"
    }
} else {
    Fail "fichier non declare : $((S1).media) / $((S2).media) -- $((S1).lastError) / $((S2).lastError)"
}
Snapshot

Step "(c) lecture lancee depuis l'UI du client 1"
Send-Cmd $cmd1 "ready"
Send-Cmd $cmd2 "ready"
if (Wait-For $STEP_TIMEOUT "les deux se declarent prets" { $a = S1; $b = S2; ($a.ready -and $b.ready) }) {
    Say "  ready-gate leve"
} else {
    Say "  (ready non confirme, on tente quand meme)"
}
Send-Cmd $cmd1 "play"
if (Wait-For $STEP_TIMEOUT "les deux VLC jouent" { $a = S1; $b = S2; ($a.vlcState -eq "playing" -and $b.vlcState -eq "playing") }) {
    if (Wait-For $STEP_TIMEOUT "la position avance des deux cotes" { (Pos (S1)) -ge 1 -and (Pos (S2)) -ge 1 }) {
        Pass "play depuis l'UI du client 1 : la position avance chez les deux"
    } else {
        Fail "la position n'avance pas (pos1=$(Pos (S1)) pos2=$(Pos (S2)))"
    }
} else {
    Fail "les deux VLC ne jouent pas (vlc1=$((S1).vlcState) vlc2=$((S2).vlcState))"
}
Snapshot

Say "  maintien de la synchronisation pendant $HOLD_SEC s..."
if (Holds $HOLD_SEC { (S1).vlcState -eq "playing" -and (Gap) -lt 2 }) {
    Pass ("synchronisation stable en lecture (ecart {0:F3} s)" -f (Gap))
} else {
    Fail ("synchronisation instable en lecture (ecart {0:F3} s)" -f (Gap))
}

Step "(d) pause demandee depuis l'UI du client 2"
Send-Cmd $cmd2 "pause"
if (Wait-For $STEP_TIMEOUT "les deux VLC en pause" { $a = S1; $b = S2; ($a.vlcState -eq "paused" -and $b.vlcState -eq "paused") }) {
    if (Holds 3 { (S1).vlcState -eq "paused" -and (S2).vlcState -eq "paused" }) {
        Pass "pause depuis l'UI du client 2 : les deux sont en pause et y restent"
    } else {
        Fail "la pause n'a pas tenu"
    }
} else {
    Fail "pause non propagee (vlc1=$((S1).vlcState) vlc2=$((S2).vlcState))"
}
Snapshot

Step "(e) seek demande depuis l'UI du client 1"
Send-Cmd $cmd1 "seek $SEEK_TARGET"
if (Wait-For $STEP_TIMEOUT "les deux VLC pres de la cible" {
        [Math]::Abs((Pos (S1)) - $SEEK_TARGET) -lt 3 -and [Math]::Abs((Pos (S2)) - $SEEK_TARGET) -lt 3 }) {
    Pass ("seek a $SEEK_TARGET s depuis l'UI : les deux positions y sont (ecart {0:F3} s)" -f (Gap))
} else {
    Fail "seek non propage (pos1=$(Pos (S1)) pos2=$(Pos (S2)), cible $SEEK_TARGET)"
}
Snapshot

# --- l'autre sens : l'utilisateur agit DANS VLC ------------------------------
#
# Ces trois points sont le trou historique de VS-029 : la detection d'action
# utilisateur du client C n'avait jamais ete prouvee en reel. Les requetes
# partent de CE script vers l'interface HTTP locale de VLC : le client ne les a
# pas emises, il ne peut que les CONSTATER.

Step "(f) play fait DANS VLC du client 1"
if (Invoke-Vlc (S1) "pl_forceresume" "") {
    if (Wait-For $STEP_TIMEOUT "le client 2 repart" { $a = S1; $b = S2; ($a.vlcState -eq "playing" -and $b.vlcState -eq "playing") }) {
        Pass "play fait dans VLC (client 1) : detecte et propage au client 2"
    } else {
        Fail "play dans VLC non propage (vlc1=$((S1).vlcState) vlc2=$((S2).vlcState))"
    }
} else {
    Fail "requete play vers VLC impossible"
}
Snapshot

Step "(g) pause faite DANS VLC du client 2"
Start-Sleep -Seconds 3   # laisser retomber le hold de 2 s de l'action precedente
if (Invoke-Vlc (S2) "pl_forcepause" "") {
    if (Wait-For $STEP_TIMEOUT "le client 1 se met en pause" { $a = S1; $b = S2; ($a.vlcState -eq "paused" -and $b.vlcState -eq "paused") }) {
        Pass "pause faite dans VLC (client 2) : detectee et propagee au client 1"
    } else {
        Fail "pause dans VLC non propagee (vlc1=$((S1).vlcState) vlc2=$((S2).vlcState))"
    }
} else {
    Fail "requete pause vers VLC impossible"
}
Snapshot

Step "(h) seek fait DANS VLC du client 2"
Start-Sleep -Seconds 3
if (Invoke-Vlc (S2) "seek" "$VLC_SEEK_TARGET") {
    if (Wait-For $STEP_TIMEOUT "le client 1 suit la nouvelle position" {
            [Math]::Abs((Pos (S1)) - $VLC_SEEK_TARGET) -lt 5 -and [Math]::Abs((Pos (S2)) - $VLC_SEEK_TARGET) -lt 5 }) {
        Pass ("seek a $VLC_SEEK_TARGET s fait dans VLC (client 2) : detecte et propage (ecart {0:F3} s)" -f (Gap))
    } else {
        Fail "seek dans VLC non propage (pos1=$(Pos (S1)) pos2=$(Pos (S2)), cible $VLC_SEEK_TARGET)"
    }
} else {
    Fail "requete seek vers VLC impossible"
}
Snapshot

Step "(i) drift final"
Send-Cmd $cmd2 "play"
if (Wait-For $STEP_TIMEOUT "reprise de la lecture" { $a = S1; $b = S2; ($a.vlcState -eq "playing" -and $b.vlcState -eq "playing") }) {
    Say "  la lecture est repartie (relancee par le client 2)"
}
Wait-For $STEP_TIMEOUT "convergence sous $DRIFT_MAX s" { (Gap) -lt $DRIFT_MAX } | Out-Null
$finalGap = Gap
# Deux VLC arretes ont un ecart de 0 s : sans cette garde, le point le plus
# important du harnais passerait au vert alors que rien n'a jamais joue.
$reallyPlaying = ((S1).vlcState -eq "playing" -and (S2).vlcState -eq "playing" -and (Pos (S1)) -gt 0)
if (-not $reallyPlaying) {
    Fail "aucune lecture en cours a la mesure du drift (vlc1=$((S1).vlcState) vlc2=$((S2).vlcState) pos1=$(Pos (S1)))"
} elseif ($finalGap -lt $DRIFT_MAX) {
    Pass ("ecart entre les deux VLC = {0:F3} s (< $DRIFT_MAX s) -- drift salle {1:F3}/{2:F3}" -f $finalGap, (S1).driftSec, (S2).driftSec)
} else {
    Fail ("ecart final {0:F3} s >= $DRIFT_MAX s -- drift salle {1:F3}/{2:F3}" -f $finalGap, (S1).driftSec, (S2).driftSec)
}
Snapshot

Step "(j) fermeture propre"
Send-Cmd $cmd1 "quit"
Send-Cmd $cmd2 "quit"
if (Wait-For 20 "arret des deux clients" { $script:p1.HasExited -and $script:p2.HasExited }) {
    Pass "les deux clients se sont arretes d'eux-memes (close 1000 envoyee)"
} else {
    Fail "un client ne s'est pas arrete sur la commande quit"
}
Start-Sleep -Seconds 2
$orphans = @(Get-CimInstance Win32_Process -Filter "Name='vlc.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine.Contains($work) -and $_.CommandLine.Contains("http-port") })
if ($orphans.Count -eq 0) {
    Pass "aucun VLC orphelin lance par les clients"
} else {
    Fail "$($orphans.Count) VLC orphelin(s)"
}

# --- bilan ------------------------------------------------------------------

Cleanup
Step "bilan"
Say "  $script:passed point(s) OK, $script:failed echec(s)"
Say "  journaux et etats : $work"
if ($script:failed -ne 0) {
    Say ""
    foreach ($pair in @(@("client1", (Join-Path $work "home1\vibesync.log")), @("client2", (Join-Path $work "home2\vibesync.log")))) {
        Say "--- fin du journal $($pair[0]) ---"
        if (Test-Path $pair[1]) { Get-Content $pair[1] -Tail 20 }
    }
    exit 1
}
if (-not $Keep) { Say "  (-Keep pour conserver le dossier)" }
Say ""
Say "RESULTAT : PASS"
exit 0

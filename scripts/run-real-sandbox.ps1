# Exécute le test réel double-VLC dans une Windows Sandbox jetable (VS-006).
# Tout tourne dans la sandbox : serveur + 2 moteurs + 2 vrais VLC (mappé du host,
# lecture seule). Réseau désactivé (loopback uniquement). Résultat récupéré via un
# dossier mappé en écriture, la sandbox s'éteint elle-même à la fin.
param(
    [int]$TimeoutMinutes = 12
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$go = "C:\Program Files\Go\bin\go.exe"
$vlcHost = "C:\Program Files\VideoLAN\VLC"
$work = Join-Path $repo "dist\sandbox"
$out = Join-Path $work "out"

if (-not (Test-Path "$env:windir\System32\WindowsSandbox.exe")) { throw "Windows Sandbox indisponible" }
if (-not (Test-Path $vlcHost)) { throw "VLC introuvable sur le host" }

New-Item -ItemType Directory -Force $work, $out | Out-Null
Remove-Item "$out\*" -Force -ErrorAction SilentlyContinue

# 1. Compile le binaire de test (pure Go, exécutable sans Go dans la sandbox).
& $go test -c -o (Join-Path $work "real.test.exe") "$repo\test\real"
if ($LASTEXITCODE -ne 0) { throw "compilation du test échouée" }

# 2. Script exécuté au logon de la sandbox.
$desktop = "C:\Users\WDAGUtilityAccount\Desktop"
@"
@echo off
set VIBESYNC_REAL=1
set VIBESYNC_VLC=$desktop\vlc\vlc.exe
echo [sandbox] demarrage du test reel > $desktop\sandbox\out\result.txt
$desktop\sandbox\real.test.exe -test.v -test.timeout 10m >> $desktop\sandbox\out\result.txt 2>&1
echo EXITCODE=%ERRORLEVEL% >> $desktop\sandbox\out\result.txt
echo fini > $desktop\sandbox\out\done.txt
shutdown /s /t 5
"@ | Out-File -Encoding ascii (Join-Path $work "run.cmd")

# 3. Configuration .wsb.
@"
<Configuration>
  <Networking>Disable</Networking>
  <VGpu>Disable</VGpu>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <MappedFolders>
    <MappedFolder><HostFolder>$vlcHost</HostFolder><SandboxFolder>$desktop\vlc</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$work</HostFolder><SandboxFolder>$desktop\sandbox</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>cmd.exe /c $desktop\sandbox\run.cmd</Command></LogonCommand>
</Configuration>
"@ | Out-File -Encoding ascii (Join-Path $work "vibesync-real.wsb")

# 4. Lance la sandbox (minimisée) et attend le verdict.
$proc = Start-Process "$env:windir\System32\WindowsSandbox.exe" -ArgumentList "`"$work\vibesync-real.wsb`"" -WindowStyle Minimized -PassThru
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while ((Get-Date) -lt $deadline -and -not (Test-Path "$out\done.txt")) { Start-Sleep -Seconds 10 }

if (-not (Test-Path "$out\done.txt")) {
    Write-Warning "délai dépassé — fermeture forcée de la sandbox"
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 5
Get-Content "$out\result.txt"

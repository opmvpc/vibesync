<#
.SYNOPSIS
    Provisionne une VM Windows 11 ARM64 pour builder et tester vibesync.

.DESCRIPTION
    Installe, sans droits admin et sans installeur, la toolchain complete :
      - Git      : MinGit ARM64 (zip portable)            -> <Tools>\git
      - Go       : archive officielle go.dev windows-arm64 -> <Tools>\go
      - Clang    : llvm-mingw ucrt aarch64 (multi-cibles)  -> <Tools>\llvm-mingw
      - VLC      : zip officiel videolan winarm64          -> <Tools>\vlc
    puis pousse git\cmd, go\bin, llvm-mingw\bin et %USERPROFILE%\go\bin dans le
    PATH *utilisateur* persistant (registre HKCU), et clone le depot.

    Le script est REJOUABLE : chaque etape verifie d'abord la version deja
    installee et ne retelecharge rien si elle correspond. Les archives sont
    mises en cache dans <Tools>\.cache et re-verifiees par taille.

    Aucun redemarrage n'est necessaire : le PATH utilisateur est relu a chaque
    nouvelle session SSH (le service sshd cree l'environnement depuis le
    registre a l'ouverture de session).

.NOTES
    Windows 11 ARM64 execute les binaires x86_64 par emulation : la variante
    ucrt-aarch64 de llvm-mingw tourne en natif ET sait cross-compiler vers
    x86_64-w64-mingw32, qui est la cible de ui\win32\build.bat.

    ui\win32\build.bat cherche clang d'abord dans C:\Users\thibs\tools\llvm-mingw
    puis, si absent, retombe sur le PATH (lignes 11-12) : mettre llvm-mingw\bin
    dans le PATH suffit, build.bat n'a pas a etre modifie.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\OPMVPC\provision-vm.ps1

.NOTES
    CE FICHIER DOIT RESTER EN ASCII PUR. PowerShell 5.1 lit un .ps1 sans BOM en
    ANSI (cp1252) : un tiret cadratin UTF-8 (E2 80 94) y devient <<a-euro-">>,
    et ce 0x94 est le guillemet fermant typographique, que l'analyseur traite
    comme un vrai " -- toutes les chaines suivantes se decalent et le script ne
    parse plus. Verification : grep -n '[^ -~\t]' scripts/provision-vm.ps1
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [string] $ToolsRoot = (Join-Path $env:USERPROFILE 'tools'),
    [string] $RepoDir   = (Join-Path $env:USERPROFILE 'vibesync'),
    [string] $RepoUrl   = 'https://github.com/opmvpc/vibesync.git',

    # Versions epinglees. Les remonter d'un cran suffit a rejouer une mise a jour.
    [string] $GitVersion      = '2.55.0.3',
    [string] $GitTag          = 'v2.55.0.windows.3',
    [string] $GoVersion       = 'go1.26.5',
    [string] $LlvmMingwTag    = '20260616',
    [string] $VlcVersion      = '3.0.23',

    [switch] $SkipVlc,
    [switch] $SkipClone,

    # Vide <Tools>\.cache en fin de course (~345 Mo d'archives). A ne PAS
    # utiliser si l'on compte rejouer le script hors ligne ou changer de version
    # souvent : le cache est ce qui rend un second passage instantane.
    [switch] $PurgeCache
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # sinon Invoke-WebRequest est ~10x plus lent
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$CacheDir = Join-Path $ToolsRoot '.cache'

function Write-Step { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Skip { param([string]$Message) Write-Host "    (deja fait) $Message" -ForegroundColor DarkGray }

function New-Dir {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { [void](New-Item -ItemType Directory -Force -Path $Path) }
}

# Telechargement avec cache : si le fichier existe deja et que sa taille
# correspond a l'attendu (ou qu'aucune taille n'est fournie), on ne refait rien.
function Get-CachedFile {
    param(
        [Parameter(Mandatory)] [string] $Url,
        [Parameter(Mandatory)] [string] $FileName,
        [long] $ExpectedSize = 0
    )
    New-Dir $CacheDir
    $dest = Join-Path $CacheDir $FileName
    if (Test-Path -LiteralPath $dest) {
        $len = (Get-Item -LiteralPath $dest).Length
        if ($ExpectedSize -eq 0 -or $len -eq $ExpectedSize) {
            Write-Skip "archive en cache : $FileName ($len octets)"
            return $dest
        }
        Write-Host "    archive incomplete ($len octets), retelechargement" -ForegroundColor Yellow
        Remove-Item -LiteralPath $dest -Force
    }
    Write-Host "    telechargement $Url"
    $tmp = "$dest.part"
    Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
    $len = (Get-Item -LiteralPath $tmp).Length
    if ($ExpectedSize -ne 0 -and $len -ne $ExpectedSize) {
        Remove-Item -LiteralPath $tmp -Force
        throw "taille inattendue pour $FileName : $len octets au lieu de $ExpectedSize"
    }
    Move-Item -LiteralPath $tmp -Destination $dest -Force
    Write-Host "    recu $len octets"
    return $dest
}

# tar.exe (bsdtar, present depuis Win10 1803) degzippe un .zip bien plus vite
# que Expand-Archive de PowerShell 5.1 ; on garde Expand-Archive en secours.
function Expand-Zip {
    param(
        [Parameter(Mandatory)] [string] $Archive,
        [Parameter(Mandatory)] [string] $Destination
    )
    New-Dir $Destination
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path -LiteralPath $tar) {
        & $tar -xf $Archive -C $Destination
        if ($LASTEXITCODE -eq 0) { return }
        Write-Host "    tar a echoue ($LASTEXITCODE), repli sur Expand-Archive" -ForegroundColor Yellow
    }
    Expand-Archive -LiteralPath $Archive -DestinationPath $Destination -Force
}

# Installe une archive dont le contenu utile est soit a la racine du zip
# (StripRoot absent) soit dans un unique repertoire de tete (StripRoot present).
function Install-Zip {
    param(
        [Parameter(Mandatory)] [string] $Archive,
        [Parameter(Mandatory)] [string] $Target,   # repertoire final, ex. <Tools>\go
        [switch] $StripRoot
    )
    $staging = "$Target.staging"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    Expand-Zip -Archive $Archive -Destination $staging

    $source = $staging
    if ($StripRoot) {
        $tops = @(Get-ChildItem -LiteralPath $staging)
        if ($tops.Count -ne 1 -or -not $tops[0].PSIsContainer) {
            throw "archive $Archive : un unique repertoire de tete etait attendu, trouve $($tops.Count) entree(s)"
        }
        $source = $tops[0].FullName
    }

    if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target -Recurse -Force }
    Move-Item -LiteralPath $source -Destination $Target
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}

# Ajout idempotent au PATH utilisateur persistant (HKCU\Environment).
# On evite setx : il tronque a 1024 caracteres.
function Add-UserPath {
    param([Parameter(Mandatory)] [string[]] $Directories)
    $current = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($null -eq $current) { $current = '' }
    $entries = @($current -split ';' | Where-Object { $_ -ne '' })
    $added   = @()
    foreach ($dir in $Directories) {
        $normalized = $dir.TrimEnd('\')
        $already = $entries | Where-Object { $_.TrimEnd('\') -ieq $normalized }
        if (-not $already) { $entries += $normalized; $added += $normalized }
    }
    if ($added.Count -gt 0) {
        [Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'User')
        foreach ($d in $added) { Write-Host "    PATH utilisateur += $d" }
    } else {
        Write-Skip 'PATH utilisateur deja complet'
    }
    # Rend les outils utilisables dans CE processus (le PATH utilisateur n'est
    # relu qu'a la prochaine session).
    foreach ($dir in $Directories) {
        if (($env:Path -split ';') -notcontains $dir) { $env:Path = "$dir;$env:Path" }
    }
}

# Execute un binaire et renvoie sa premiere ligne de sortie, ou $null s'il
# n'existe pas / echoue. Sert aux tests d'idempotence.
function Get-ToolVersion {
    param([string] $Exe, [string[]] $Arguments = @('--version'))
    if (-not (Test-Path -LiteralPath $Exe)) { return $null }
    try {
        $out = & $Exe @Arguments 2>&1 | Select-Object -First 1
        return [string]$out
    } catch { return $null }
}

Write-Host ''
Write-Host "vibesync - provisionnement VM Windows ARM64" -ForegroundColor Green
Write-Host "  outils : $ToolsRoot"
Write-Host "  depot  : $RepoDir"
Write-Host ''
New-Dir $ToolsRoot

# ---------------------------------------------------------------- 1. Git ----
$GitDir = Join-Path $ToolsRoot 'git'
$GitExe = Join-Path $GitDir 'cmd\git.exe'
Write-Step "Git $GitVersion (MinGit ARM64)"
$v = Get-ToolVersion $GitExe
if ($v -and $v -match [regex]::Escape(($GitVersion -replace '\.(\d+)$', '.windows.$1'))) {
    Write-Skip $v
} elseif ($v -and $v -match ($GitVersion -replace '\.\d+$', '')) {
    Write-Skip $v
} else {
    $zip = Get-CachedFile `
        -Url "https://github.com/git-for-windows/git/releases/download/$GitTag/MinGit-$GitVersion-arm64.zip" `
        -FileName "MinGit-$GitVersion-arm64.zip" -ExpectedSize 37446324
    Install-Zip -Archive $zip -Target $GitDir       # MinGit : contenu a la racine du zip
    Write-Host "    $(Get-ToolVersion $GitExe)"
}

# ----------------------------------------------------------------- 2. Go ----
$GoDir = Join-Path $ToolsRoot 'go'
$GoExe = Join-Path $GoDir 'bin\go.exe'
Write-Step "Go $GoVersion (windows-arm64)"
$v = Get-ToolVersion $GoExe @('version')
if ($v -and $v -match [regex]::Escape($GoVersion)) {
    Write-Skip $v
} else {
    $zip = Get-CachedFile `
        -Url "https://go.dev/dl/$GoVersion.windows-arm64.zip" `
        -FileName "$GoVersion.windows-arm64.zip" -ExpectedSize 71440103
    Install-Zip -Archive $zip -Target $GoDir -StripRoot   # le zip contient go\
    Write-Host "    $(Get-ToolVersion $GoExe @('version'))"
}

# --------------------------------------------------------- 3. llvm-mingw ----
$LlvmDir  = Join-Path $ToolsRoot 'llvm-mingw'
$ClangExe = Join-Path $LlvmDir 'bin\x86_64-w64-mingw32-clang.exe'
Write-Step "llvm-mingw $LlvmMingwTag (ucrt, host aarch64, multi-cibles)"
$v = Get-ToolVersion $ClangExe
if ($v -and (Test-Path -LiteralPath (Join-Path $LlvmDir 'bin\x86_64-w64-mingw32-windres.exe'))) {
    Write-Skip $v
} else {
    $zip = Get-CachedFile `
        -Url "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmMingwTag/llvm-mingw-$LlvmMingwTag-ucrt-aarch64.zip" `
        -FileName "llvm-mingw-$LlvmMingwTag-ucrt-aarch64.zip" -ExpectedSize 181877750
    Install-Zip -Archive $zip -Target $LlvmDir -StripRoot
    Write-Host "    $(Get-ToolVersion $ClangExe)"
}

# ---------------------------------------------------------------- 4. VLC ----
$VlcDir = Join-Path $ToolsRoot 'vlc'
$VlcExe = Join-Path $VlcDir 'vlc.exe'
if ($SkipVlc) {
    Write-Step 'VLC - ignore (-SkipVlc)'
} else {
    Write-Step "VLC $VlcVersion (winarm64, zip portable)"
    if (Test-Path -LiteralPath $VlcExe) {
        Write-Skip $VlcExe
    } else {
        $zip = Get-CachedFile `
            -Url "https://download.videolan.org/pub/videolan/vlc/$VlcVersion/winarm64/vlc-$VlcVersion-winarm64.zip" `
            -FileName "vlc-$VlcVersion-winarm64.zip"
        Install-Zip -Archive $zip -Target $VlcDir -StripRoot   # le zip contient vlc-3.0.x\
        Write-Host "    $VlcExe"
    }
}

# --------------------------------------------------------------- 5. PATH ----
Write-Step 'PATH utilisateur persistant'
$pathDirs = @(
    (Join-Path $GitDir  'cmd'),
    (Join-Path $GoDir   'bin'),
    (Join-Path $LlvmDir 'bin'),
    (Join-Path $env:USERPROFILE 'go\bin')    # GOPATH par defaut = %USERPROFILE%\go
)
Add-UserPath -Directories $pathDirs

# -------------------------------------------------------------- 6. clone ----
if ($SkipClone) {
    Write-Step 'clone - ignore (-SkipClone)'
} else {
    Write-Step "depot $RepoUrl"
    if (Test-Path -LiteralPath (Join-Path $RepoDir '.git')) {
        Write-Skip "$RepoDir existe deja, fetch"
        & $GitExe -C $RepoDir fetch --all --prune
    } else {
        & $GitExe clone $RepoUrl $RepoDir
    }
    if ($LASTEXITCODE -ne 0) { throw "git a echoue ($LASTEXITCODE)" }
    Write-Host "    $(& $GitExe -C $RepoDir log -1 --oneline)"
}

# ------------------------------------------------------------- 7. cache -----
if ($PurgeCache -and (Test-Path -LiteralPath $CacheDir)) {
    Write-Step 'purge du cache d''archives'
    Remove-Item -LiteralPath $CacheDir -Recurse -Force
}

# ------------------------------------------------------------- 8. bilan -----
Write-Host ''
Write-Host 'Bilan' -ForegroundColor Green
Write-Host "  git    : $GitExe   -> $(Get-ToolVersion $GitExe)"
Write-Host "  go     : $GoExe    -> $(Get-ToolVersion $GoExe @('version'))"
Write-Host "  clang  : $ClangExe -> $(Get-ToolVersion $ClangExe)"
if (Test-Path -LiteralPath $VlcExe) { Write-Host "  vlc    : $VlcExe" }
Write-Host ''
Write-Host 'Prochaine etape (nouvelle session, pour que le PATH soit relu) :'
Write-Host "  cd /d $RepoDir"
Write-Host '  ui\win32\build.bat test'
Write-Host ''

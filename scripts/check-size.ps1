# Garde-fou ADR-007 : chaque client (UI + core) doit peser < 10 Mo.
# Usage : .\scripts\check-size.ps1 -Files dist\vibesync-core.exe, dist\VibeSync.exe
param(
    [Parameter(Mandatory = $true)][string[]]$Files,
    [int]$BudgetMB = 10
)

$total = 0
foreach ($f in $Files) {
    if (-not (Test-Path $f)) { Write-Error "Introuvable : $f"; exit 1 }
    $size = (Get-Item $f).Length
    $total += $size
    "{0,-45} {1,8:N2} Mo" -f $f, ($size / 1MB)
}
"{0,-45} {1,8:N2} Mo (budget {2} Mo)" -f "TOTAL", ($total / 1MB), $BudgetMB
if ($total -gt $BudgetMB * 1MB) {
    Write-Error "Budget taille dépassé (ADR-007) !"
    exit 1
}
"OK : dans le budget."

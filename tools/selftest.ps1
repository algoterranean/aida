<#
.SYNOPSIS
  Run the AIDA packaged-game self-test harness (docs/SELFTEST.md).

.DESCRIPTION
  Launches Satisfactory with -AIDASelfTest=<scenarios>. Once a save is loaded (load the TEST SAVE
  manually unless your game version honors an auto-load argument via -ExtraArgs), the mod drives
  the scenarios against the live world, writes a results JSON, and quits (quitOnDone).
  This script waits for the results file and prints the outcomes.

.EXAMPLE
  ./tools/selftest.ps1                                   # smoke scenarios
  ./tools/selftest.ps1 -Scenarios tools/selftest/my.json -TimeoutMinutes 20
#>
[CmdletBinding()]
param(
	[string]$Scenarios = "$PSScriptRoot\selftest\scenarios-smoke.json",
	[string]$GameDir   = 'C:\Program Files (x86)\Steam\steamapps\common\Satisfactory',
	[string]$ResultsPath = "$PSScriptRoot\..\.debug\selftest-results.json",
	[int]$TimeoutMinutes = 20,
	[string]$ExtraArgs = ''
)

$ErrorActionPreference = 'Stop'
$Scenarios = (Resolve-Path $Scenarios).Path
$ResultsDir = Split-Path $ResultsPath -Parent
New-Item -ItemType Directory -Force $ResultsDir | Out-Null
$ResultsPath = Join-Path (Resolve-Path $ResultsDir).Path (Split-Path $ResultsPath -Leaf)
if (Test-Path $ResultsPath) { Remove-Item $ResultsPath -Force }

$exe = Join-Path $GameDir 'FactoryGameSteam.exe'
if (-not (Test-Path $exe)) { throw "Game exe not found at $exe -- is -GameDir correct?" }

Write-Host "Scenarios: $Scenarios" -ForegroundColor Cyan
Write-Host "Results:   $ResultsPath" -ForegroundColor Cyan
Write-Host 'Launching Satisfactory (Steam must be running). LOAD THE TEST SAVE when the menu appears' -ForegroundColor Yellow
Write-Host 'unless an auto-load argument was passed via -ExtraArgs. The run starts once a player exists.' -ForegroundColor Yellow

$argList = @("-AIDASelfTest=$Scenarios", "-AIDASelfTestOut=$ResultsPath")
if ($ExtraArgs) { $argList += $ExtraArgs.Split(' ') }
Start-Process -FilePath $exe -ArgumentList $argList | Out-Null

$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while (-not (Test-Path $ResultsPath)) {
	if ((Get-Date) -gt $deadline) {
		Write-Host "No results after $TimeoutMinutes min. Check the game log:" -ForegroundColor Red
		Write-Host "  $env:LOCALAPPDATA\FactoryGame\Saved\Logs\FactoryGame.log  (grep [selftest])"
		exit 1
	}
	Start-Sleep -Seconds 5
}
Start-Sleep -Seconds 1 # let the writer finish

$results = Get-Content $ResultsPath -Raw | ConvertFrom-Json
Write-Host ''
foreach ($o in $results.outcomes) {
	if ($o.passed) {
		Write-Host ("PASS  {0}/{1}  {2}" -f $o.scenario, $o.step, $o.label) -ForegroundColor Green
	} else {
		Write-Host ("FAIL  {0}/{1}  {2}: {3}" -f $o.scenario, $o.step, $o.label, $o.reason) -ForegroundColor Red
	}
}
Write-Host ''
Write-Host ("{0} passed, {1} failed" -f $results.passed, $results.failed) -ForegroundColor $(if ($results.failed -eq 0) { 'Green' } else { 'Red' })
exit $(if ($results.failed -eq 0) { 0 } else { 1 })

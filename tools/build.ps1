<#
.SYNOPSIS
  Build a Satisfactory/AIDA target from the command line (C++ hot-reload in-editor is unreliable).

.EXAMPLE
  ./tools/build.ps1                        # FactoryEditor Win64 Development (default)
  ./tools/build.ps1 -Target FactoryServer -Platform Linux
  ./tools/build.ps1 -GenerateProjectFiles  # regenerate the .sln first

.NOTES
  Defaults match this dev machine. Override with params or the AIDA_ENGINE / AIDA_PROJECT env vars.
  NOTE: the editor target is 'FactoryEditor' (NOT 'FactoryGameEditor' as some docs say).
#>
[CmdletBinding()]
param(
	[string]$Target   = 'FactoryEditor',
	[ValidateSet('Win64', 'Linux')]
	[string]$Platform = 'Win64',
	[ValidateSet('Development', 'Shipping', 'DebugGame')]
	[string]$Config   = 'Development',
	[string]$Engine   = $(if ($env:AIDA_ENGINE)  { $env:AIDA_ENGINE }  else { 'C:\UE_CSS' }),
	[string]$Project  = $(if ($env:AIDA_PROJECT) { $env:AIDA_PROJECT } else { 'C:\Users\Dad\Desktop\SatisfactoryModLoader\FactoryGame.uproject' }),
	[switch]$GenerateProjectFiles
)

$ErrorActionPreference = 'Stop'
$build = Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat'
$ubt   = Join-Path $Engine 'Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe'

if (-not (Test-Path $build))   { throw "Build.bat not found at $build -- is -Engine correct?" }
if (-not (Test-Path $Project)) { throw "Project not found at $Project -- is -Project correct?" }

if ($GenerateProjectFiles) {
	Write-Host "Generating project files for $Project ..." -ForegroundColor Cyan
	& $ubt -projectfiles -project="$Project" -game -engine -progress
	if ($LASTEXITCODE -ne 0) { throw "Project file generation failed ($LASTEXITCODE)" }
}

if ($Platform -eq 'Linux' -and -not $env:LINUX_MULTIARCH_ROOT) {
	Write-Warning "LINUX_MULTIARCH_ROOT is not set -- the Linux cross-compile toolchain (v25_clang-18.1.0-rockylinux8) may be missing."
}

Write-Host "Building $Target | $Platform | $Config" -ForegroundColor Cyan
& $build $Target $Platform $Config -project="$Project" -waitmutex
exit $LASTEXITCODE

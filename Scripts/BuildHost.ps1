# Builds UXWebviewHost and stages the exe with its CEF runtime next to the plugin,
# where the plugin looks for it: Binaries/ThirdParty/UXWebviewHost/<Platform>/.
# Usage: ./Scripts/BuildHost.ps1 [-Config Release] [-StageDir <dir>] [-SkipFetchCef]

[CmdletBinding()]
param(
	[string]$Config = 'Release',
	[string]$StageDir,
	[switch]$SkipFetchCef
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceDir = Join-Path $repoRoot 'native/host'
$buildDir = Join-Path $repoRoot 'native/build'

if (-not $SkipFetchCef -and -not (Test-Path (Join-Path $repoRoot 'native/cef')))
{
	Write-Host 'CEF not found locally, running FetchCef.ps1'
	& (Join-Path $PSScriptRoot 'FetchCef.ps1') | Out-Null
	if ($LASTEXITCODE -ne 0)
	{
		exit $LASTEXITCODE
	}
}

if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt')))
{
	Write-Host 'Configuring CMake'
	cmake -S $sourceDir -B $buildDir
	if ($LASTEXITCODE -ne 0)
	{
		exit $LASTEXITCODE
	}
}

# --parallel 2 keeps memory bounded on machines that build UE in parallel.
cmake --build $buildDir --config $Config --target UXWebviewHost --parallel 2
if ($LASTEXITCODE -ne 0)
{
	exit $LASTEXITCODE
}

$outputDir = Join-Path $buildDir $Config
if (-not (Test-Path $outputDir))
{
	Write-Error "Build output not found: $outputDir"
	exit 1
}

if (-not $StageDir)
{
	# UXWebviewCEF.Build.cs reads this folder to fill its RuntimeDependencies, so
	# the host has to be staged inside the plugin rather than next to the repo.
	$StageDir = Join-Path $repoRoot 'Plugins/UXWebview/Binaries/ThirdParty/UXWebviewHost/Win64'
}
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null

# CEF resolves libcef.dll, *.pak and locales relative to the executable, so the
# whole Release folder is staged flat.
Copy-Item -Path (Join-Path $outputDir '*') -Destination $StageDir -Recurse -Force
Remove-Item -Path (Join-Path $StageDir 'debug.log') -Force -ErrorAction SilentlyContinue

$exePath = Join-Path $StageDir 'UXWebviewHost.exe'
Write-Host "Staged $exePath"

$missing = @('libcef.dll', 'chrome_elf.dll', 'icudtl.dat', 'resources.pak', 'locales')
foreach ($item in $missing)
{
	if (-not (Test-Path (Join-Path $StageDir $item)))
	{
		Write-Error "staged CEF runtime is incomplete, '$item' is missing"
		exit 1
	}
}

Write-Output $exePath

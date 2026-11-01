# Cooks, stages and archives the sample project with the CEF host bundled.
# Usage: ./Scripts/Package.ps1 -EngineRoot 'E:\UnrealEngine\UE_5.8' [-Config Shipping]
#        [-ArchiveDir <dir>] [-BuildEditor] [-SkipHost]

[CmdletBinding()]
param(
	[string]$EngineRoot = $env:UE_ROOT,
	[string]$Config = 'Shipping',
	[string]$ArchiveDir,
	[switch]$BuildEditor,
	[switch]$SkipHost
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot 'Samples/UXWebviewSample/UXWebviewSample.uproject'

if (-not $EngineRoot)
{
	Write-Error 'Pass -EngineRoot or set the UE_ROOT environment variable to the engine install directory.'
	exit 1
}

$runUat = Join-Path $EngineRoot 'Engine/Build/BatchFiles/RunUAT.bat'
if (-not (Test-Path $runUat))
{
	Write-Error "RunUAT.bat not found under $EngineRoot"
	exit 1
}

if (-not $ArchiveDir)
{
	$ArchiveDir = Join-Path $repoRoot 'Samples/UXWebviewSample/Saved/Package/Windows'
}

# The CEF runtime is staged next to the plugin by BuildHost.ps1 and then picked
# up by the module's RuntimeDependencies, so it always ships with the package.
if (-not $SkipHost)
{
	& (Join-Path $PSScriptRoot 'BuildHost.ps1')
	if ($LASTEXITCODE -ne 0)
	{
		exit $LASTEXITCODE
	}
}

New-Item -ItemType Directory -Path $ArchiveDir -Force | Out-Null

$uatArgs = @(
	'BuildCookRun',
	"-project=$project",
	'-noP4',
	'-platform=Win64',
	"-clientconfig=$Config",
	'-cook',
	'-stage',
	'-pak',
	'-archive',
	"-archivedirectory=$ArchiveDir"
)

if (-not $BuildEditor)
{
	# The plugin and sample are expected to be compiled already.
	$uatArgs += '-nobuild'
}

& $runUat @uatArgs
if ($LASTEXITCODE -ne 0)
{
	exit $LASTEXITCODE
}

$stagedHost = Join-Path $ArchiveDir 'Windows/UXWebviewSample/Binaries/Win64/UXWebviewHost/UXWebviewHost.exe'
if (-not (Test-Path $stagedHost))
{
	Write-Error "packaged host missing: $stagedHost"
	exit 1
}

foreach ($file in @('libcef.dll', 'resources.pak', 'locales/zh-CN.pak'))
{
	$path = Join-Path (Split-Path -Parent $stagedHost) $file
	if (-not (Test-Path $path))
	{
		Write-Error "packaged CEF runtime is incomplete, '$file' is missing"
		exit 1
	}
}

Write-Output "Packaged $ArchiveDir"

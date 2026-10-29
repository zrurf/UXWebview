# Fetches and verifies the CEF binary distribution pinned in native/cef.lock.json.
# Usage: ./Scripts/FetchCef.ps1 [-Platform windows64] [-CefRoot <dir>] [-Force]
# Prints the resolved CEF root directory as the final output line.

[CmdletBinding()]
param(
	[string]$Platform,
	[string]$CefRoot,
	[switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Fail([string]$Message)
{
	Write-Error "FetchCef: $Message"
	exit 1
}

function Resolve-DefaultPlatform
{
	$arch = $env:PROCESSOR_ARCHITECTURE
	if (-not $arch)
	{
		$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
	}

	if ($IsMacOS -or $IsLinux)
	{
		$arch = (uname -m)
	}

	switch ($arch.ToUpperInvariant())
	{
		'AMD64' { return 'windows64' }
		'X86_64' { if ($IsLinux) { return 'linux64' } return 'macosx64' }
		'ARM64' { if ($IsMacOS) { return 'macosarm64' } return 'windowsarm64' }
		default { Fail "unsupported host architecture '$arch'; pass -Platform explicitly" }
	}
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $repoRoot 'native/cef.lock.json'
if (-not (Test-Path $lockPath))
{
	Fail "lock file not found: $lockPath"
}

$lock = Get-Content -Path $lockPath -Raw | ConvertFrom-Json
$targetPlatform = if ($Platform) { $Platform } else { Resolve-DefaultPlatform }
$entry = $lock.files.$targetPlatform
if (-not $entry)
{
	Fail "platform '$targetPlatform' is not pinned in $lockPath"
}

$version = $lock.version
if (-not $CefRoot)
{
	$CefRoot = Join-Path $repoRoot 'native/cef'
}
$targetDir = Join-Path $CefRoot "$version/$targetPlatform"
$markerPath = Join-Path $targetDir '.cef-sha1'
$expectedMarker = "$($entry.sha1)  $($entry.name)"

if (-not $Force -and (Test-Path $markerPath))
{
	$currentMarker = (Get-Content -Path $markerPath -Raw).Trim()
	if ($currentMarker -eq $expectedMarker -and (Test-Path (Join-Path $targetDir 'include')))
	{
		Write-Host "CEF $version ($targetPlatform) already present, skipping download."
		Write-Output $targetDir
		exit 0
	}
}

$downloadUrl = "$($lock.baseUrl)$($entry.name)"
$cacheDir = Join-Path $env:TEMP 'uxwebview-cef'
$archivePath = Join-Path $cacheDir $entry.name
New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null

$archiveValid = $false
if ((Test-Path $archivePath) -and -not $Force)
{
	$cachedHash = (Get-FileHash -Path $archivePath -Algorithm SHA1).Hash.ToLowerInvariant()
	$archiveValid = ($cachedHash -eq $entry.sha1)
	if (-not $archiveValid)
	{
		Write-Host "Cached archive hash mismatch, discarding it."
		Remove-Item -Path $archivePath -Force
	}
}

if (-not $archiveValid)
{
	Write-Host "Downloading $downloadUrl"
	try
	{
		# curl.exe and WebClient both stream to disk; Invoke-WebRequest buffers
		# the whole archive in memory on Windows PowerShell 5.1.
		$curl = Get-Command curl.exe -ErrorAction SilentlyContinue
		if ($curl)
		{
			& $curl.Source -fL --retry 3 -o $archivePath $downloadUrl
			if ($LASTEXITCODE -ne 0)
			{
				throw "curl exited with code $LASTEXITCODE"
			}
		}
		else
		{
			$client = New-Object System.Net.WebClient
			$client.DownloadFile($downloadUrl, $archivePath)
			$client.Dispose()
		}
	}
	catch
	{
		Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue
		Fail "download failed: $($_.Exception.Message)"
	}
}

$actualHash = (Get-FileHash -Path $archivePath -Algorithm SHA1).Hash.ToLowerInvariant()
if ($actualHash -ne $entry.sha1)
{
	Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue
	Fail "SHA1 mismatch for $($entry.name): expected $($entry.sha1), got $actualHash"
}

if (-not (Get-Command tar -ErrorAction SilentlyContinue))
{
	Fail 'tar.exe is required to extract the CEF archive (available on Windows 10 1803+)'
}

$stagingDir = Join-Path $cacheDir "extract-$targetPlatform"
if (Test-Path $stagingDir)
{
	Remove-Item -Path $stagingDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null

Write-Host "Extracting $($entry.name)"
& tar -xjf $archivePath --strip-components=1 -C $stagingDir
if ($LASTEXITCODE -ne 0)
{
	Remove-Item -Path $stagingDir -Recurse -Force -ErrorAction SilentlyContinue
	Fail "extraction failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path (Join-Path $stagingDir 'include')))
{
	Remove-Item -Path $stagingDir -Recurse -Force -ErrorAction SilentlyContinue
	Fail 'unexpected archive layout: include/ is missing after extraction'
}

if (Test-Path $targetDir)
{
	Remove-Item -Path $targetDir -Recurse -Force
}
New-Item -ItemType Directory -Path (Split-Path -Parent $targetDir) -Force | Out-Null
Move-Item -Path $stagingDir -Destination $targetDir
Set-Content -Path $markerPath -Value $expectedMarker -NoNewline -Encoding ascii

Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue

Write-Host "CEF $version ($targetPlatform) ready at $targetDir"
Write-Output $targetDir

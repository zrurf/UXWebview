# Fetches the WebView2 SDK pin from native/webview2.lock.json and installs the
# headers plus the x64 loader under native/third_party/webview2.
# The SDK only provides headers and the loader; the WebView2 runtime itself is
# installed with Windows/Edge and must exist on the target machine.
# Usage: ./Scripts/FetchWebView2.ps1 [-Force]
# Prints the installed SDK root as the final output line.

[CmdletBinding()]
param(
	[switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Fail([string]$Message)
{
	Write-Error "FetchWebView2: $Message"
	exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $repoRoot 'native/webview2.lock.json'
if (-not (Test-Path $lockPath))
{
	Fail "lock file not found: $lockPath"
}

$lock = Get-Content -Path $lockPath -Raw | ConvertFrom-Json
$targetDir = Join-Path $repoRoot 'native/third_party/webview2'
$markerPath = Join-Path $targetDir '.version'
$expectedMarker = "$($lock.package) $($lock.version)"

if (-not $Force -and (Test-Path $markerPath))
{
	$currentMarker = (Get-Content -Path $markerPath -Raw).Trim()
	if ($currentMarker -eq $expectedMarker -and (Test-Path (Join-Path $targetDir 'include/WebView2.h')))
	{
		Write-Host "WebView2 SDK $($lock.version) already present, skipping download."
		Write-Output $targetDir
		exit 0
	}
}

$cacheDir = Join-Path $env:TEMP 'uxwebview-webview2'
$archivePath = Join-Path $cacheDir "$($lock.package).$($lock.version).zip"
New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null

$archiveValid = $false
if ((Test-Path $archivePath) -and -not $Force)
{
	$cachedHash = (Get-FileHash -Path $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
	$archiveValid = ($cachedHash -eq $lock.sha256)
	if (-not $archiveValid)
	{
		Write-Host "Cached archive hash mismatch, discarding it."
		Remove-Item -Path $archivePath -Force
	}
}

if (-not $archiveValid)
{
	Write-Host "Downloading $($lock.url)"
	try
	{
		$curl = Get-Command curl.exe -ErrorAction SilentlyContinue
		if ($curl)
		{
			& $curl.Source -fL --retry 3 -o $archivePath $lock.url
			if ($LASTEXITCODE -ne 0)
			{
				throw "curl exited with code $LASTEXITCODE"
			}
		}
		else
		{
			$client = New-Object System.Net.WebClient
			$client.DownloadFile($lock.url, $archivePath)
			$client.Dispose()
		}
	}
	catch
	{
		Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue
		Fail "download failed: $($_.Exception.Message)"
	}
}

$actualHash = (Get-FileHash -Path $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $lock.sha256)
{
	Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue
	Fail "SHA256 mismatch: expected $($lock.sha256), got $actualHash"
}

$stagingDir = Join-Path $cacheDir 'extract'
if (Test-Path $stagingDir)
{
	Remove-Item -Path $stagingDir -Recurse -Force
}
Expand-Archive -Path $archivePath -DestinationPath $stagingDir -Force

$includeSource = Join-Path $stagingDir 'build/native/include'
$x64Source = Join-Path $stagingDir 'build/native/x64'
if (-not (Test-Path (Join-Path $includeSource 'WebView2.h')))
{
	Fail 'unexpected package layout: build/native/include/WebView2.h is missing'
}
if (-not (Test-Path (Join-Path $x64Source 'WebView2Loader.dll.lib')))
{
	Fail 'unexpected package layout: x64 loader import library is missing'
}

if (Test-Path $targetDir)
{
	Remove-Item -Path $targetDir -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $targetDir 'include') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $targetDir 'x64') -Force | Out-Null

Copy-Item -Path (Join-Path $includeSource '*') -Destination (Join-Path $targetDir 'include') -Force
Copy-Item -Path (Join-Path $x64Source 'WebView2Loader.dll') -Destination (Join-Path $targetDir 'x64') -Force
# UBT expects a conventional .lib suffix; the package names it WebView2Loader.dll.lib.
Copy-Item -Path (Join-Path $x64Source 'WebView2Loader.dll.lib') `
	-Destination (Join-Path $targetDir 'x64/WebView2Loader.lib') -Force
Copy-Item -Path (Join-Path $stagingDir 'LICENSE.txt') -Destination (Join-Path $targetDir 'LICENSE.txt') -Force
Set-Content -Path $markerPath -Value $expectedMarker -NoNewline -Encoding ascii

Remove-Item -Path $stagingDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "WebView2 SDK $($lock.version) ready at $targetDir"
Write-Output $targetDir

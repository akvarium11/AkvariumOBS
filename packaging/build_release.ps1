# Full release build script for AkvariumOBS 1.1
$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot | Split-Path -Parent
Set-Location $repoRoot

$version = "1.1"
$releaseDir = Join-Path $repoRoot "release"
$stageDir = Join-Path $releaseDir "stage\AkvariumOBS"
$portableZip = Join-Path $releaseDir "AkvariumOBS-$version-Windows-x64-Portable.zip"
$installerExe = Join-Path $releaseDir "AkvariumOBS-$version-Windows-x64-Installer.exe"

# 1. Run Staging
Write-Host "[1/3] Staging release files..."
& (Join-Path $PSScriptRoot "stage_release.ps1")

# 2. Build Portable ZIP
Write-Host "[2/3] Building Portable ZIP ($portableZip)..."
if (Test-Path $portableZip) { Remove-Item -Force $portableZip }
Compress-Archive -Path $stageDir -DestinationPath $portableZip -CompressionLevel Optimal

# 3. Build NSIS Installer
Write-Host "[3/3] Building NSIS Installer ($installerExe)..."
$nsisExe = "C:\Program Files (x86)\NSIS\makensis.exe"
if (-not (Test-Path $nsisExe)) {
    throw "NSIS compiler not found at $nsisExe"
}

$nsiScript = Join-Path $PSScriptRoot "installer.nsi"
& $nsisExe /V2 "/DSTAGE=$stageDir" "/DVERSION=$version" $nsiScript
if ($LASTEXITCODE -ne 0) {
    throw "makensis failed with exit code $LASTEXITCODE"
}

# NSIS outputs to repo root or packaging depending on cwd
$generatedInstaller = Join-Path $repoRoot "AkvariumOBS-$version-Windows-x64-Installer.exe"
if (-not (Test-Path $generatedInstaller)) {
    $generatedInstaller = Join-Path $PSScriptRoot "AkvariumOBS-$version-Windows-x64-Installer.exe"
}

if (Test-Path $generatedInstaller) {
    Move-Item -Force $generatedInstaller $installerExe
}

Write-Host "================ RELEASE READY ================"
Get-Item $portableZip, $installerExe | Select-Object Name, Length, LastWriteTime

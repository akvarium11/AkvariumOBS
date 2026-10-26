# Staging script for AkvariumOBS release
$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot | Split-Path -Parent
Set-Location $repoRoot

$stageDir = Join-Path $repoRoot "release\stage\AkvariumOBS"
$binDir = Join-Path $stageDir "bin\64bit"
$dataVoiceDir = Join-Path $stageDir "data\voice"

Write-Host "Cleaning up old release/stage..."
if (Test-Path (Join-Path $repoRoot "release\stage")) {
    Remove-Item -Recurse -Force (Join-Path $repoRoot "release\stage")
}
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

Write-Host "Copying bin, data, obs-plugins from rundir..."
Copy-Item -Recurse -Force (Join-Path $repoRoot "build_x64\rundir\RelWithDebInfo\bin") (Join-Path $stageDir "bin")
Copy-Item -Recurse -Force (Join-Path $repoRoot "build_x64\rundir\RelWithDebInfo\data") (Join-Path $stageDir "data")
Copy-Item -Recurse -Force (Join-Path $repoRoot "build_x64\rundir\RelWithDebInfo\obs-plugins") (Join-Path $stageDir "obs-plugins")

Write-Host "Stripping PDB files..."
Get-ChildItem -Path (Join-Path $repoRoot "release\stage") -Recurse -Filter "*.pdb" | Remove-Item -Force

Write-Host "Removing test binaries..."
Get-ChildItem -Path $binDir -Filter "akvarium-*-test.exe" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -Path $binDir -Filter "akvarium-*-selftest.exe" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -Path $binDir -Filter "phrase-match-test.exe" -ErrorAction SilentlyContinue | Remove-Item -Force

Write-Host "Copying voice recognition models and runtime..."
New-Item -ItemType Directory -Force -Path $dataVoiceDir | Out-Null
$models = @("vosk-model-small-ru-0.22", "vosk-model-small-en-us-0.15")
foreach ($m in $models) {
    $src = Join-Path $repoRoot "data\voice\$m"
    if (Test-Path $src) {
        Copy-Item -Recurse -Force $src (Join-Path $dataVoiceDir $m)
    }
}
$voskDlls = @("libvosk.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")
foreach ($dll in $voskDlls) {
    $src = Join-Path $repoRoot "deps\vosk\bin\$dll"
    if (Test-Path $src) {
        Copy-Item -Force $src (Join-Path $binDir $dll)
    }
}

Write-Host "Copying CRT and OpenMP runtime DLLs..."
$crtDir = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
$ompDir = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.OpenMP"

$crtDlls = @("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll")
foreach ($dll in $crtDlls) {
    Copy-Item -Force (Join-Path $crtDir $dll) $binDir
}
Copy-Item -Force (Join-Path $ompDir "vcomp140.dll") $binDir
Copy-Item -Force "C:\Windows\System32\vulkan-1.dll" $binDir

Write-Host "Copying COPYING and README.txt..."
Copy-Item -Force (Join-Path $repoRoot "COPYING") $stageDir
Copy-Item -Force (Join-Path $repoRoot "packaging\README.txt") $stageDir

Write-Host "Staging complete at $stageDir"

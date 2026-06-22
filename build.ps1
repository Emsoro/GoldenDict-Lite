# Build script for GoldenDict-Lite
param(
    [switch]$Clean,
    [switch]$SetupDeps
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

if ($SetupDeps) {
    Write-Host "Setting up dependencies..." -ForegroundColor Cyan
    Push-Location "TauriCPP"
    & .\build.ps1 -SetupDeps
    Pop-Location
    
    # Install zlib via vcpkg
    Write-Host "Installing zlib..." -ForegroundColor Cyan
    vcpkg install zlib:x64-windows --triplet x64-windows
    
    Write-Host "Dependencies setup complete!" -ForegroundColor Green
    exit 0
}

$buildDir = "build"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Set-Location $buildDir

# Find vcpkg toolchain
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
    $vcpkgRoot = "C:/vcpkg"
}

$toolchainFile = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"

Write-Host "Configuring with CMake..." -ForegroundColor Cyan
cmake .. -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE="$toolchainFile" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DCMAKE_BUILD_TYPE=Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configuration failed!" -ForegroundColor Red
    exit 1
}

Write-Host "Building..." -ForegroundColor Cyan
cmake --build . --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host "Build successful!" -ForegroundColor Green
Write-Host "Output: build/GoldenDictLite.exe" -ForegroundColor Green

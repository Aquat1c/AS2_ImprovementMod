# AS2 Improvement Mod - Dependency Setup Script
# Run this script to download and set up all required libraries

param(
    [switch]$Force = $false
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$LibDir = Join-Path $PSScriptRoot "lib"

Write-Host "=== AS2 Improvement Mod - Dependency Setup ===" -ForegroundColor Cyan
Write-Host ""

# Create lib directory
if (-not (Test-Path $LibDir)) {
    New-Item -ItemType Directory -Path $LibDir | Out-Null
    Write-Host "Created lib directory" -ForegroundColor Green
}

# ============================================================================
# MinHook
# ============================================================================

$MinHookDir = Join-Path $LibDir "minhook"
$MinHookVersion = "1.3.3"
$MinHookUrl = "https://github.com/TsudaKageyu/minhook/releases/download/v$MinHookVersion/MinHook_133_bin.zip"

Write-Host "[1/3] MinHook..." -ForegroundColor Yellow

if ((Test-Path "$MinHookDir\include\MinHook.h") -and -not $Force) {
    Write-Host "  Already installed, skipping" -ForegroundColor Gray
} else {
    Write-Host "  Downloading MinHook v$MinHookVersion..."
    $MinHookZip = Join-Path $env:TEMP "minhook.zip"
    
    try {
        Invoke-WebRequest -Uri $MinHookUrl -OutFile $MinHookZip -UseBasicParsing
        
        # Clean and create directory
        if (Test-Path $MinHookDir) { Remove-Item -Recurse -Force $MinHookDir }
        New-Item -ItemType Directory -Path $MinHookDir | Out-Null
        
        # Extract
        Expand-Archive -Path $MinHookZip -DestinationPath $MinHookDir -Force
        
        # Reorganize structure
        $includeDir = Join-Path $MinHookDir "include"
        $libDir32 = Join-Path $MinHookDir "lib\x86"
        
        New-Item -ItemType Directory -Path $includeDir -Force | Out-Null
        New-Item -ItemType Directory -Path $libDir32 -Force | Out-Null
        
        # Find and copy files
        Get-ChildItem -Path $MinHookDir -Filter "MinHook.h" -Recurse | 
            Select-Object -First 1 | 
            ForEach-Object { Copy-Item $_.FullName $includeDir }
        
        Get-ChildItem -Path $MinHookDir -Filter "MinHook.x86.lib" -Recurse | 
            Select-Object -First 1 | 
            ForEach-Object { Copy-Item $_.FullName $libDir32 }
        
        Remove-Item $MinHookZip -Force
        Write-Host "  Installed successfully" -ForegroundColor Green
    } catch {
        Write-Host "  Failed to download: $_" -ForegroundColor Red
        Write-Host "  Please download manually from: $MinHookUrl" -ForegroundColor Yellow
    }
}

# ============================================================================
# SDL3
# ============================================================================

$SDL3Dir = Join-Path $LibDir "SDL3"
$SDL3Version = "3.2.14"
$SDL3Url = "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3Version/SDL3-devel-$SDL3Version-VC.zip"

Write-Host "[2/3] SDL3..." -ForegroundColor Yellow

if ((Test-Path "$SDL3Dir\include\SDL3\SDL.h") -and -not $Force) {
    Write-Host "  Already installed, skipping" -ForegroundColor Gray
} else {
    Write-Host "  Downloading SDL3 v$SDL3Version..."
    $SDL3Zip = Join-Path $env:TEMP "sdl3.zip"
    
    try {
        Invoke-WebRequest -Uri $SDL3Url -OutFile $SDL3Zip -UseBasicParsing
        
        # Clean and extract
        if (Test-Path $SDL3Dir) { Remove-Item -Recurse -Force $SDL3Dir }
        Expand-Archive -Path $SDL3Zip -DestinationPath $LibDir -Force
        
        # Rename extracted folder
        $extracted = Get-ChildItem -Path $LibDir -Directory -Filter "SDL3-*" | Select-Object -First 1
        if ($extracted) {
            Rename-Item $extracted.FullName $SDL3Dir
        }
        
        # Reorganize for our structure
        $libDir32 = Join-Path $SDL3Dir "lib\x86"
        if (-not (Test-Path $libDir32)) {
            New-Item -ItemType Directory -Path $libDir32 -Force | Out-Null
            
            # Copy x86 files
            $sdlLib = Get-ChildItem -Path $SDL3Dir -Filter "SDL3.lib" -Recurse | 
                Where-Object { $_.Directory.Name -eq "x86" -or $_.FullName -match "lib\\x86" } |
                Select-Object -First 1
            
            $sdlDll = Get-ChildItem -Path $SDL3Dir -Filter "SDL3.dll" -Recurse | 
                Where-Object { $_.Directory.Name -eq "x86" -or $_.FullName -match "lib\\x86" } |
                Select-Object -First 1
            
            if ($sdlLib) { Copy-Item $sdlLib.FullName $libDir32 }
            if ($sdlDll) { Copy-Item $sdlDll.FullName $libDir32 }
        }
        
        Remove-Item $SDL3Zip -Force
        Write-Host "  Installed successfully" -ForegroundColor Green
    } catch {
        Write-Host "  Failed to download: $_" -ForegroundColor Red
        Write-Host "  Please download manually from: $SDL3Url" -ForegroundColor Yellow
    }
}

# ============================================================================
# ImGui
# ============================================================================

$ImGuiDir = Join-Path $LibDir "imgui"
$ImGuiVersion = "1.91.9"
$ImGuiUrl = "https://github.com/ocornut/imgui/archive/refs/tags/v$ImGuiVersion.zip"

Write-Host "[3/3] ImGui..." -ForegroundColor Yellow

if ((Test-Path "$ImGuiDir\imgui.h") -and -not $Force) {
    Write-Host "  Already installed, skipping" -ForegroundColor Gray
} else {
    Write-Host "  Downloading ImGui v$ImGuiVersion..."
    $ImGuiZip = Join-Path $env:TEMP "imgui.zip"
    
    try {
        Invoke-WebRequest -Uri $ImGuiUrl -OutFile $ImGuiZip -UseBasicParsing
        
        # Clean and extract
        if (Test-Path $ImGuiDir) { Remove-Item -Recurse -Force $ImGuiDir }
        Expand-Archive -Path $ImGuiZip -DestinationPath $LibDir -Force
        
        # Rename extracted folder
        $extracted = Get-ChildItem -Path $LibDir -Directory -Filter "imgui-*" | Select-Object -First 1
        if ($extracted) {
            Rename-Item $extracted.FullName $ImGuiDir
        }
        
        Remove-Item $ImGuiZip -Force
        Write-Host "  Installed successfully" -ForegroundColor Green
    } catch {
        Write-Host "  Failed to download: $_" -ForegroundColor Red
        Write-Host "  Please download manually from: $ImGuiUrl" -ForegroundColor Yellow
    }
}

# ============================================================================
# Verify Installation
# ============================================================================

Write-Host ""
Write-Host "=== Verification ===" -ForegroundColor Cyan

$allGood = $true

# Check MinHook
if (Test-Path "$MinHookDir\include\MinHook.h") {
    Write-Host "[OK] MinHook" -ForegroundColor Green
} else {
    Write-Host "[MISSING] MinHook - include/MinHook.h not found" -ForegroundColor Red
    $allGood = $false
}

# Check SDL3
if (Test-Path "$SDL3Dir\include\SDL3\SDL.h") {
    Write-Host "[OK] SDL3 headers" -ForegroundColor Green
} else {
    Write-Host "[MISSING] SDL3 - include/SDL3/SDL.h not found" -ForegroundColor Red
    $allGood = $false
}

if (Test-Path "$SDL3Dir\lib\x86\SDL3.lib") {
    Write-Host "[OK] SDL3 lib (x86)" -ForegroundColor Green
} else {
    Write-Host "[MISSING] SDL3 - lib/x86/SDL3.lib not found" -ForegroundColor Red
    $allGood = $false
}

# Check ImGui
if (Test-Path "$ImGuiDir\imgui.h") {
    Write-Host "[OK] ImGui" -ForegroundColor Green
} else {
    Write-Host "[MISSING] ImGui - imgui.h not found" -ForegroundColor Red
    $allGood = $false
}

if (Test-Path "$ImGuiDir\backends\imgui_impl_dx9.cpp") {
    Write-Host "[OK] ImGui DX9 backend" -ForegroundColor Green
} else {
    Write-Host "[MISSING] ImGui - backends/imgui_impl_dx9.cpp not found" -ForegroundColor Red
    $allGood = $false
}

Write-Host ""

if ($allGood) {
    Write-Host "All dependencies installed! Ready to build." -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Cyan
    Write-Host "  cd build"
    Write-Host "  cmake .. -A Win32"
    Write-Host "  cmake --build . --config Release"
} else {
    Write-Host "Some dependencies are missing. Please install them manually." -ForegroundColor Red
    Write-Host ""
    Write-Host "Download links:" -ForegroundColor Yellow
    Write-Host "  MinHook: https://github.com/TsudaKageyu/minhook/releases"
    Write-Host "  SDL3:    https://github.com/libsdl-org/SDL/releases"
    Write-Host "  ImGui:   https://github.com/ocornut/imgui/releases"
}

Write-Host ""

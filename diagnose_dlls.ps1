# Alice Senki 2 Mod - DLL Diagnostic Script
# Run this in the game folder to diagnose loading issues

# Check if we're running in 32-bit PowerShell
$is32bit = [IntPtr]::Size -eq 4

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Alice Senki 2 Mod - DLL Diagnostics" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

if (-not $is32bit) {
    Write-Host "WARNING: Running in 64-bit PowerShell!" -ForegroundColor Yellow
    Write-Host "For accurate DLL load tests, use 32-bit PowerShell:" -ForegroundColor Yellow
    Write-Host "  C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -ForegroundColor White
    Write-Host ""
    
    # Try to relaunch in 32-bit PowerShell
    $scriptPath = $MyInvocation.MyCommand.Path
    if ($scriptPath) {
        Write-Host "Relaunching in 32-bit PowerShell..." -ForegroundColor Cyan
        Start-Process "C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList "-ExecutionPolicy Bypass -File `"$scriptPath`"" -WorkingDirectory (Get-Location) -Wait
        exit
    }
}

Write-Host "PowerShell: $(if ($is32bit) { '32-bit (correct)' } else { '64-bit (may give wrong results)' })" -ForegroundColor $(if ($is32bit) { 'Green' } else { 'Yellow' })
Write-Host ""

$gameFolder = Get-Location
Write-Host "Game folder: $gameFolder" -ForegroundColor Yellow
Write-Host ""

# Required DLLs
$requiredDlls = @("d3d9.dll", "as2_rollback.dll", "SDL3.dll")

# ============================================================================
# 1. Check DLL existence and sizes
# ============================================================================
Write-Host "--- DLL File Check ---" -ForegroundColor Green

foreach ($dll in $requiredDlls) {
    $path = Join-Path $gameFolder $dll
    if (Test-Path $path) {
        $file = Get-Item $path
        $sizeKB = [math]::Round($file.Length / 1024, 1)
        Write-Host "  [OK] $dll - $sizeKB KB" -ForegroundColor Green
        
        # Check if blocked
        $zone = Get-Item $path -Stream "Zone.Identifier" -ErrorAction SilentlyContinue
        if ($zone) {
            Write-Host "       WARNING: File is BLOCKED by Windows!" -ForegroundColor Red
            Write-Host "       Right-click -> Properties -> Unblock" -ForegroundColor Red
        }
    } else {
        Write-Host "  [MISSING] $dll" -ForegroundColor Red
    }
}
Write-Host ""

# ============================================================================
# 2. Check architecture
# ============================================================================
Write-Host "--- Architecture Check ---" -ForegroundColor Green

function Get-PEArchitecture {
    param([string]$Path)
    try {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
        $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
        switch ($machine) {
            0x014c { return "x86 (32-bit)" }
            0x8664 { return "x64 (64-bit)" }
            0x01c4 { return "ARM" }
            0xaa64 { return "ARM64" }
            default { return "Unknown (0x$($machine.ToString('X4')))" }
        }
    } catch {
        return "Error: $_"
    }
}

# Check game exe
$gameExe = Get-ChildItem $gameFolder -Filter "*.exe" | Select-Object -First 1
if ($gameExe) {
    $arch = Get-PEArchitecture $gameExe.FullName
    Write-Host "  Game ($($gameExe.Name)): $arch" -ForegroundColor $(if ($arch -eq "x86 (32-bit)") { "Green" } else { "Red" })
}

foreach ($dll in $requiredDlls) {
    $path = Join-Path $gameFolder $dll
    if (Test-Path $path) {
        $arch = Get-PEArchitecture $path
        $color = if ($arch -eq "x86 (32-bit)") { "Green" } else { "Red" }
        Write-Host "  ${dll}: $arch" -ForegroundColor $color
    }
}
Write-Host ""

# ============================================================================
# 3. Try loading DLLs and get real errors
# ============================================================================
Write-Host "--- DLL Load Test ---" -ForegroundColor Green

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.ComponentModel;

public class DllLoader {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr LoadLibraryW(string lpFileName);
    
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool FreeLibrary(IntPtr hModule);
    
    [DllImport("kernel32.dll")]
    public static extern uint GetLastError();
    
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
    public static extern uint FormatMessage(uint dwFlags, IntPtr lpSource, uint dwMessageId,
        uint dwLanguageId, StringBuilder lpBuffer, uint nSize, IntPtr Arguments);
    
    public static string GetErrorMessage(uint errorCode) {
        StringBuilder sb = new StringBuilder(1024);
        uint FORMAT_MESSAGE_FROM_SYSTEM = 0x1000;
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, IntPtr.Zero, errorCode, 0, sb, (uint)sb.Capacity, IntPtr.Zero);
        return sb.ToString().Trim();
    }
}
"@

# Test loading each DLL
$loadOrder = @("SDL3.dll", "as2_rollback.dll")

foreach ($dll in $loadOrder) {
    $path = Join-Path $gameFolder $dll
    if (Test-Path $path) {
        $fullPath = (Resolve-Path $path).Path
        Write-Host "  Loading: $dll..." -NoNewline
        
        $handle = [DllLoader]::LoadLibraryW($fullPath)
        if ($handle -eq [IntPtr]::Zero) {
            $errorCode = [DllLoader]::GetLastError()
            $errorMsg = [DllLoader]::GetErrorMessage($errorCode)
            Write-Host " FAILED" -ForegroundColor Red
            Write-Host "       Error $errorCode`: $errorMsg" -ForegroundColor Red
            
            # Common error codes
            switch ($errorCode) {
                126 { Write-Host "       -> Missing dependency or file not found" -ForegroundColor Yellow }
                193 { Write-Host "       -> Wrong architecture (32/64-bit mismatch)" -ForegroundColor Yellow }
                14001 { Write-Host "       -> Missing Visual C++ Redistributable" -ForegroundColor Yellow }
                5 { Write-Host "       -> Access denied (try running as admin)" -ForegroundColor Yellow }
            }
        } else {
            Write-Host " OK (0x$($handle.ToString('X8')))" -ForegroundColor Green
            [DllLoader]::FreeLibrary($handle) | Out-Null
        }
    }
}
Write-Host ""

# ============================================================================
# 4. Check Visual C++ Redistributables
# ============================================================================
Write-Host "--- Visual C++ Redistributable Check ---" -ForegroundColor Green

$vcRedists = @(
    @{ Name = "VC++ 2015-2022 x86"; Key = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x86" },
    @{ Name = "VC++ 2015-2022 x86 (alt)"; Key = "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86" }
)

$foundVC = $false
foreach ($vc in $vcRedists) {
    if (Test-Path $vc.Key) {
        $version = (Get-ItemProperty $vc.Key -ErrorAction SilentlyContinue).Version
        Write-Host "  [OK] $($vc.Name): $version" -ForegroundColor Green
        $foundVC = $true
        break
    }
}

if (-not $foundVC) {
    Write-Host "  [WARNING] VC++ 2015-2022 Redistributable (x86) not found!" -ForegroundColor Red
    Write-Host "  Download from: https://aka.ms/vs/17/release/vc_redist.x86.exe" -ForegroundColor Yellow
}
Write-Host ""

# ============================================================================
# 5. Check SDL3.dll imports (what it depends on)
# ============================================================================
Write-Host "--- SDL3.dll Dependency Check ---" -ForegroundColor Green

$sdl3Path = Join-Path $gameFolder "SDL3.dll"
if (Test-Path $sdl3Path) {
    try {
        $bytes = [System.IO.File]::ReadAllBytes($sdl3Path)
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
        
        # Read number of sections
        $numSections = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
        $optHeaderSize = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
        $optHeaderOffset = $peOffset + 24
        
        # Import table RVA is at offset 104 in optional header (32-bit PE)
        $importRVA = [BitConverter]::ToUInt32($bytes, $optHeaderOffset + 104)
        
        if ($importRVA -gt 0) {
            Write-Host "  SDL3.dll imports from these DLLs:" -ForegroundColor Cyan
            # Common SDL3 dependencies
            $commonDeps = @("KERNEL32.dll", "USER32.dll", "GDI32.dll", "ADVAPI32.dll", 
                           "SHELL32.dll", "ole32.dll", "OLEAUT32.dll", "IMM32.dll",
                           "VERSION.dll", "WINMM.dll", "SETUPAPI.dll", "cfgmgr32.dll",
                           "VCRUNTIME140.dll", "api-ms-win-crt-*.dll")
            Write-Host "  (Cannot parse imports directly, but common deps are:)" -ForegroundColor Gray
            Write-Host "  KERNEL32, USER32, GDI32, ADVAPI32, WINMM, SETUPAPI" -ForegroundColor Gray
            Write-Host "  VCRUNTIME140.dll, api-ms-win-crt-runtime-l1-1-0.dll" -ForegroundColor Gray
        }
    } catch {
        Write-Host "  Error parsing PE: $_" -ForegroundColor Red
    }
    
    # Check for VCRUNTIME140.dll
    $vcruntime = "C:\Windows\SysWOW64\VCRUNTIME140.dll"
    if (Test-Path $vcruntime) {
        Write-Host "  [OK] VCRUNTIME140.dll found in SysWOW64" -ForegroundColor Green
    } else {
        $vcruntime = "C:\Windows\System32\VCRUNTIME140.dll"
        if (Test-Path $vcruntime) {
            Write-Host "  [OK] VCRUNTIME140.dll found in System32" -ForegroundColor Green
        } else {
            Write-Host "  [MISSING] VCRUNTIME140.dll - need VC++ Redistributable!" -ForegroundColor Red
        }
    }
    
    # Check for Universal CRT
    $ucrt = "C:\Windows\SysWOW64\ucrtbase.dll"
    if (Test-Path $ucrt) {
        Write-Host "  [OK] ucrtbase.dll (Universal CRT) found" -ForegroundColor Green
    } else {
        Write-Host "  [WARNING] ucrtbase.dll not found in SysWOW64" -ForegroundColor Yellow
    }
}
Write-Host ""

# ============================================================================
# 6. Check for common issues
# ============================================================================
Write-Host "--- Additional Checks ---" -ForegroundColor Green

# Check if running from network path
if ($gameFolder.Path -like "\\*") {
    Write-Host "  [WARNING] Running from network path - may cause issues" -ForegroundColor Yellow
}

# Check folder permissions
try {
    $testFile = Join-Path $gameFolder "test_write_permission.tmp"
    [System.IO.File]::WriteAllText($testFile, "test")
    Remove-Item $testFile -Force
    Write-Host "  [OK] Write permissions OK" -ForegroundColor Green
} catch {
    Write-Host "  [WARNING] Cannot write to game folder" -ForegroundColor Yellow
}

# Check Windows version
$os = Get-CimInstance Win32_OperatingSystem
Write-Host "  Windows: $($os.Caption) ($($os.OSArchitecture))" -ForegroundColor Cyan

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Diagnostics Complete" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "If DLLs still fail to load:" -ForegroundColor Yellow
Write-Host "  1. Install VC++ 2015-2022 Redistributable (x86)" -ForegroundColor White
Write-Host "     https://aka.ms/vs/17/release/vc_redist.x86.exe" -ForegroundColor White
Write-Host "  2. Install Windows Universal CRT if missing" -ForegroundColor White
Write-Host "  3. Try running dumpbin /dependents SDL3.dll" -ForegroundColor White
Write-Host "     (from VS Developer Command Prompt)" -ForegroundColor White
Write-Host "  4. Use Dependencies tool: https://github.com/lucasg/Dependencies" -ForegroundColor White
Write-Host ""

# If 32-bit, try one more detailed test
if ($is32bit) {
    Write-Host "--- Detailed 32-bit Load Test ---" -ForegroundColor Green
    
    $sdl3Path = Join-Path $gameFolder "SDL3.dll"
    if (Test-Path $sdl3Path) {
        $fullPath = (Resolve-Path $sdl3Path).Path
        Write-Host "  Attempting to load: $fullPath" -ForegroundColor Cyan
        
        # Set DLL directory to game folder first
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class DllHelper {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetDllDirectoryW(string lpPathName);
    
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr LoadLibraryExW(string lpLibFileName, IntPtr hFile, uint dwFlags);
    
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool FreeLibrary(IntPtr hModule);
    
    [DllImport("kernel32.dll")]
    public static extern uint GetLastError();
    
    public const uint LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008;
}
"@
        
        [DllHelper]::SetDllDirectoryW($gameFolder.Path)
        
        $handle = [DllHelper]::LoadLibraryExW($fullPath, [IntPtr]::Zero, [DllHelper]::LOAD_WITH_ALTERED_SEARCH_PATH)
        if ($handle -eq [IntPtr]::Zero) {
            $err = [DllHelper]::GetLastError()
            Write-Host "  FAILED with error: $err" -ForegroundColor Red
            
            switch ($err) {
                126 { 
                    Write-Host "  Error 126 = A dependency DLL is missing" -ForegroundColor Yellow
                    Write-Host "  Use Dependencies tool to find which one" -ForegroundColor Yellow
                }
                193 { Write-Host "  Error 193 = Architecture mismatch (shouldn't happen in 32-bit PS)" -ForegroundColor Yellow }
                14001 { Write-Host "  Error 14001 = Side-by-side config error / missing manifest" -ForegroundColor Yellow }
                127 { Write-Host "  Error 127 = Procedure not found (DLL version mismatch)" -ForegroundColor Yellow }
            }
        } else {
            Write-Host "  SUCCESS! SDL3.dll loaded at 0x$($handle.ToString('X8'))" -ForegroundColor Green
            [DllHelper]::FreeLibrary($handle) | Out-Null
        }
    }
}

Write-Host ""
Write-Host "Press any key to exit..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")

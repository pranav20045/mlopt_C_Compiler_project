# Puts the MLOpt-C toolchain on PATH for the current PowerShell session.
#
# Why this script exists:
#
# clang on Windows targets x86_64-pc-windows-msvc, which means it needs
# Microsoft's C++ standard library headers and the Windows SDK import libraries
# underneath it. clang does not reliably auto-detect a Build Tools-only install
# (no full Visual Studio), so we import the environment that vcvars64.bat sets
# up -- chiefly INCLUDE and LIB -- which clang then honours.
#
# Usage:   . ./env.ps1      (note the leading dot: it must run in YOUR session,
#                            not a child process, or the variables are lost)

$ErrorActionPreference = 'Stop'

# --- 1. LLVM, CMake and Ninja -------------------------------------------------
$toolPaths = @(
  "$env:ProgramFiles\LLVM\bin",
  "$env:ProgramFiles\CMake\bin",
  "$env:LOCALAPPDATA\Microsoft\WinGet\Links"
) | Where-Object { Test-Path $_ }

foreach ($p in $toolPaths) {
  if ($env:PATH -notlike "*$p*") { $env:PATH = "$p;$env:PATH" }
}

# --- 2. The MSVC standard library and Windows SDK -----------------------------
# vswhere is the supported way to locate an install; hardcoding the path breaks
# the moment someone installs to a different drive or a different edition.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
  throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}

$vsRoot = & $vswhere -products * -latest `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath

if (-not $vsRoot) {
  throw "No Visual Studio install with the MSVC C++ toolset was found."
}

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Run vcvars64.bat in cmd, dump the resulting environment, and copy it back into
# this PowerShell session. A batch file cannot modify its parent's environment
# directly, so round-tripping through `set` is the usual trick.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') {
    Set-Item -Path "env:$($matches[1])" -Value $matches[2]
  }
}

# --- 3. AddressSanitizer runtime ----------------------------------------------
# An ASan-instrumented executable links against clang_rt.asan_dynamic-x86_64.dll
# at RUNTIME. clang knows where that DLL lives at link time but does not put it
# on PATH, so without this the test binary dies at startup with
# 0xC0000139 (STATUS_ENTRYPOINT_NOT_FOUND) -- and the error message does not
# mention ASan at all, which makes it an unpleasant one to diagnose.
$asanDir = Get-ChildItem "$env:ProgramFiles\LLVM\lib\clang\*\lib\windows" -Directory -ErrorAction SilentlyContinue |
  Select-Object -Last 1
if ($asanDir -and $env:PATH -notlike "*$($asanDir.FullName)*") {
  $env:PATH = "$($asanDir.FullName);$env:PATH"
}

# --- 4. Prefer clang over MSVC's cl.exe ---------------------------------------
# vcvars64.bat puts cl.exe on PATH, and CMake would otherwise pick it. We want
# clang for three reasons: the warning flags in cmake/CompilerWarnings.cmake are
# tuned for it, it matches what CI runs on Linux, and the LLVM IR backend needs
# a clang that can compile .ll text files. CMake honours the CC/CXX environment
# variables, so this is all it takes.
$env:CC = 'clang'
$env:CXX = 'clang++'

# --- 5. Report ----------------------------------------------------------------
Write-Host "MLOpt-C environment ready" -ForegroundColor Green
Write-Host ("  clang   : " + (clang --version | Select-Object -First 1))
Write-Host ("  cmake   : " + (cmake --version | Select-Object -First 1))
Write-Host ("  ninja   : " + (ninja --version))
Write-Host ("  MSVC    : " + $vsRoot)
Write-Host ("  INCLUDE : " + ($env:INCLUDE -split ';').Count + " entries")

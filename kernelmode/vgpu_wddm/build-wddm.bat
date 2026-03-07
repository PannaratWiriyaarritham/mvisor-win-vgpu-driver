@echo off
setlocal

if not exist mvisor_wddm.sln (
  echo [ERROR] Run this script from kernelmode\vgpu_wddm
  exit /b 1
)

where msbuild >nul 2>nul
if errorlevel 1 (
  echo [ERROR] msbuild not found. Open a "Developer Command Prompt for VS" with WDK installed.
  exit /b 1
)

echo [INFO] Building mvisor_wddm (Release|x64)...
msbuild mvisor_wddm.sln /t:Build /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 (
  echo [ERROR] Build failed.
  exit /b 1
)

echo [OK] Build complete.
exit /b 0

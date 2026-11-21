@echo off
setlocal

rem Paths are discovered by build-mod.ps1 and passed through the environment.
rem The fallbacks below are only for invoking this .cmd directly.
if defined ESX_VCVARS64 (
  call "%ESX_VCVARS64%" >nul 2>&1
) else (
  call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)
if errorlevel 1 (
  echo vcvars64.bat failed
  exit /b 1
)

if defined ESX_WINDHAWK_LIB (
  set "WHLIB=%ESX_WINDHAWK_LIB%"
) else (
  set "WHLIB="
  for /d %%D in ("C:\Program Files\Windhawk\Engine\*") do (
    if exist "%%~D\64\windhawk.lib" set "WHLIB=%%~D\64\windhawk.lib"
  )
)
if not defined WHLIB (
  echo windhawk.lib not found under C:\Program Files\Windhawk\Engine
  exit /b 1
)
if not exist "%WHLIB%" (
  echo windhawk.lib not found: %WHLIB%
  exit /b 1
)

set "OUT=%~dp0..\dist\windhawk"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /LD /utf-8 /O2 /DUNICODE /D_UNICODE /DWH_MOD ^
   /DWH_MOD_ID=L\"local@everything-explorer-search\" /DWH_MOD_VERSION=L\"0.5.0\" ^
   /I"%~dp0generated" ^
   "%~dp0everything-explorer-search.wh.cpp" ^
   /link /OUT:"%OUT%\everything-explorer-search.dll" ^
   /EXPORT:_Z10Wh_ModInitv=Wh_ModInit /EXPORT:_Z12Wh_ModUninitv=Wh_ModUninit ^
   /EXPORT:_Z21Wh_ModSettingsChangedv=Wh_ModSettingsChanged ^
   /EXPORT:InternalWhModPtr,DATA ^
   /EXPORT:DllGetClassObject /EXPORT:DllCanUnloadNow ^
   "%WHLIB%" ^
   ole32.lib runtimeobject.lib oleaut32.lib CoreMessaging.lib UIAutomationCore.lib shell32.lib shlwapi.lib comctl32.lib version.lib user32.lib
exit /b %errorlevel%

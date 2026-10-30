@echo off
setlocal
title Tom Clancy's Splinter Cell: Double Agent - XBE Recomp - Build
cd /d "%~dp0"
echo.
echo   Tom Clancy's Splinter Cell: Double Agent - XBE Recomp
echo   =====================================================
echo.

set "PF86=%ProgramFiles(x86)%"
set "VSINST=%PF86%\Microsoft Visual Studio\Installer"
set "VSWHERE=%VSINST%\vswhere.exe"
rem Use "build.bat debug" for the release build plus F3 dump, level select, full logs
set "PRESET=release"
if /i "%~1"=="debug" set "PRESET=debug"
set "OUT=%CD%\out\build\%PRESET%"

if not exist assets\default.xbe goto :nogame
if not exist assets\sc4_adv.xbe goto :nogame

rem toolchain
echo [1/2] Toolchain
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo       Visual Studio with the C++ workload was not found.
    echo       Install Visual Studio Community and tick "Desktop development with C++".
    goto :fail
)
call :setuptools
rem CI is set on GitHub Actions: no prompts there.
if not defined TOOLS_OK if defined CI goto :byhand
if not defined TOOLS_OK (
    choice /c YN /n /m "      Let Visual Studio install the missing components? [Y/N] "
    if errorlevel 2 goto :byhand
    echo       Close Visual Studio if it is open, it will be shut down anyway.
    echo       Installing. Accept the admin prompt, then wait for the installer.
    powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait -FilePath '%VSINST%\setup.exe' -ArgumentList 'modify','--installPath','%VSDIR%','--add','Microsoft.VisualStudio.Component.VC.Llvm.Clang','--add','Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset','--add','Microsoft.VisualStudio.Component.VC.CMake.Project','--passive','--norestart','--force'"
    echo       Press a key once the Visual Studio Installer window has closed.
    pause >nul
    call :setuptools
    if not defined TOOLS_OK goto :byhand
)
for /f "tokens=*" %%v in ('clang++ --version ^| findstr /i version') do echo       %%v

rem game
echo [2/2] Game code
echo       the first build recompiles default.xbe and sc4_adv.xbe: 30 to 90 minutes on weaker systems ...
cmake --preset %PRESET% >nul || goto :fail
cmake --build "%OUT%" || goto :fail
rem the game's shadercache\ is a link to the one at the top (not in git): deleting out loses no shaders
if not exist shadercache mkdir shadercache
dir /AL /B "%OUT%" 2>nul | findstr /X /I shadercache >nul || (
    if exist "%OUT%\shadercache" (
        copy /y "%OUT%\shadercache\*" shadercache\ >nul
        rd /s /q "%OUT%\shadercache"
    )
    mklink /J "%OUT%\shadercache" "%CD%\shadercache" >nul
)

echo.
echo   Tom Clancy's Splinter Cell: Double Agent - XBE Recomp built.
echo   out\build\%PRESET%\SCDA.exe         solo and co-op
echo   out\build\%PRESET%\SCDA_Versus.exe  versus, started from the main menu
echo.
if defined CI exit /b 0
choice /c YN /n /m "  Play now? [Y/N] "
if not errorlevel 2 (start "" /d "%OUT%" "%OUT%\SCDA.exe") else (pause)
exit /b 0

rem helpers
:setuptools
set TOOLS_OK=
set MISSING=
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang++ >nul 2>nul || set MISSING=%MISSING% clang++
where cmake   >nul 2>nul || set MISSING=%MISSING% cmake
where ninja   >nul 2>nul || set MISSING=%MISSING% ninja
where git     >nul 2>nul || set MISSING=%MISSING% git
if defined MISSING (
    echo       missing:%MISSING%
    exit /b 0
)
set TOOLS_OK=1
exit /b 0

:byhand
echo.
echo       Install by hand, then run this again:
echo         clang++ / cmake / ninja  -  Visual Studio Installer, Modify, Individual
echo                                     components: "C++ Clang tools for Windows"
echo                                     and "C++ CMake tools for Windows"
echo         git                      -  https://git-scm.com/download/win
goto :fail

:nogame
echo   Put the full game files in the assets folder first:
echo     %CD%\assets
echo   It needs default.xbe and sc4_adv.xbe with the Default, Media and adv folders beside them.
goto :fail

:fail
echo.
echo   BUILD FAILED. Read the last message above.
echo.
if not defined CI pause
exit /b 1

@echo off
REM WeaponFX.dll - one command, MSVC x64, warnings at /W4 treated as errors.
REM   build.bat          -> build\WeaponFX.dll
REM Needs Visual Studio 2019+ or the Build Tools with the C++ workload; the
REM compiler is found through vswhere, so no developer prompt is required.
setlocal
set HERE=%~dp0
set OUT=%HERE%build
set OBJ=%OUT%\obj

REM Plain gotos, not a parenthesised block: inside one, %VAR% is expanded
REM before the block runs, and a variable set in it reads empty.
where cl >nul 2>nul
if not errorlevel 1 goto :have_cl
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" goto :no_vs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR goto :no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>nul
where cl >nul 2>nul
if errorlevel 1 goto :no_vs
:have_cl

if not exist "%OBJ%" mkdir "%OBJ%"

REM /PDBALTPATH: the DLL names its PDB by file name only - the full path
REM would carry the builder's user folder into every DLL handed out.
REM /W4 /WX: clean or nothing. /Zi: a PDB next to the DLL, for crash dumps.
set CFLAGS=/nologo /std:c++17 /EHsc /O2 /W4 /WX /utf-8 /Zi /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX
set SRC="%HERE%core\weapon_fx.cpp" "%HERE%host\log.cpp" "%HERE%host\patch.cpp" "%HERE%host\d3d_hooks.cpp" "%HERE%host\menu.cpp" "%HERE%host\ini.cpp" "%HERE%host\config.cpp" "%HERE%host\material.cpp" "%HERE%host\engine.cpp" "%HERE%host\dllmain.cpp"

REM Dear ImGui is third-party code: its own warning level, not /WX.
set IMGUI=%HERE%third_party\imgui
set IMGUI_SRC="%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\imgui_impl_dx11.cpp" "%IMGUI%\imgui_impl_win32.cpp"

pushd "%OBJ%"
cl %CFLAGS% /c %SRC%
if errorlevel 1 goto :fail
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /Zi /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /c %IMGUI_SRC%
if errorlevel 1 goto :fail
link /nologo /DLL /DEBUG /PDBALTPATH:%%_PDB%% /OPT:REF /OPT:ICF /OUT:"%OUT%\WeaponFX.dll" weapon_fx.obj log.obj patch.obj d3d_hooks.obj menu.obj ini.obj config.obj material.obj engine.obj dllmain.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_impl_dx11.obj imgui_impl_win32.obj d3d11.lib dxgi.lib dxguid.lib windowscodecs.lib ole32.lib user32.lib d3dcompiler.lib
if errorlevel 1 goto :fail
popd
echo === Build OK: %OUT%\WeaponFX.dll ===
exit /b 0

:fail
popd
echo === BUILD FAILED ===
exit /b 1

:no_vs
echo [ERR] MSVC not found - install Visual Studio Build Tools with the C++ workload
exit /b 1

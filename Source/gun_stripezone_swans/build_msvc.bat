@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build_msvc mkdir build_msvc
for /r src %%F in (*.cpp) do (
  cl /nologo /utf-8 /std:c++14 /EHsc /O2 /I src /c "%%F" /Fo"build_msvc\%%~nF.obj"
  if errorlevel 1 exit /b 1
)
link /nologo build_msvc\*.obj /OUT:build_msvc\MQSim_gun_stripezone_swans.exe
if errorlevel 1 exit /b 1
endlocal

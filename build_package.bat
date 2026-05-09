@echo off
setlocal

cmake --preset windows-x64-release
if errorlevel 1 exit /b %errorlevel%

cmake --build --preset windows-x64-release-package
if errorlevel 1 exit /b %errorlevel%

echo Package written to out\build\windows-x64-release\HedgehogEngineAnimationTools.zip

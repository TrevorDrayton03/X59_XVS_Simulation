@echo off
cd /d "%~dp0"

REM Use the same shader compiler and output directory as the application build.
cmake --build build --config Debug --target XVS_Shaders
exit /b %errorlevel%

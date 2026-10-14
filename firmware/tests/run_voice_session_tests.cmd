@echo off
where cl >nul 2>nul
if errorlevel 1 (
    echo Run from a Visual Studio Developer Command Prompt.
    exit /b 1
)
cl /nologo /EHsc /W4 /WX /std:c++17 tests\voice_session_test.cpp /Fe:build\voice_session_test.exe /Fo:build\voice_session_test.obj
if errorlevel 1 exit /b 1
build\voice_session_test.exe

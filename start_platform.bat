@echo off
echo =========================================================
echo             AETHERFLOW RUNTIME ORCHESTRATOR             
echo =========================================================
echo.

:: Check for compiler
where g++ >nul 2>nul
if %errorlevel% neq 0 (
    echo [Error] g++ compiler not found on PATH. Make sure MinGW is installed.
    pause
    exit /b 1
)

:: Compile server.exe if missing
if not exist server.exe (
    echo [Orchestrator] Compiling C++ Core Storage Node...
    g++ -std=c++11 -Wall -O2 src/server.cpp src/packet.cpp -o server.exe -lws2_32
    if %errorlevel% neq 0 (
        echo [Error] Compilation failed.
        pause
        exit /b 1
    )
    echo [Orchestrator] Core compiled successfully.
)

:: Clear lingering nodes to release port locks
taskkill /F /FI "WINDOWTITLE eq AetherFlow Core Node" >nul 2>nul
taskkill /F /FI "WINDOWTITLE eq AetherFlow Web Gateway" >nul 2>nul
taskkill /F /IM server.exe >nul 2>nul

echo [Orchestrator] Launching C++ Core Daemon...
start "AetherFlow Core Node" server.exe

echo [Orchestrator] Launching Python API Gateway...
start "AetherFlow Web Gateway" python gateway/api_gateway.py

:: Wait for socket binding
timeout /t 2 /nobreak >nul

echo [Orchestrator] Opening Web Dashboard...
start http://127.0.0.1:5000

echo.
echo =========================================================
echo AetherFlow platform is active!
echo Web Dashboard is loaded at: http://127.0.0.1:5000
echo.
echo Press any key in this window to stop and clean up servers.
echo =========================================================
echo.

pause >nul

echo [Orchestrator] Shutting down nodes...
taskkill /F /FI "WINDOWTITLE eq AetherFlow Core Node" >nul 2>nul
taskkill /F /FI "WINDOWTITLE eq AetherFlow Web Gateway" >nul 2>nul
taskkill /F /IM server.exe >nul 2>nul

echo [Orchestrator] Nodes shut down cleanly.
timeout /t 2 /nobreak >nul

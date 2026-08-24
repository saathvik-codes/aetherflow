#!/bin/bash
echo "========================================================="
echo "             AETHERFLOW RUNTIME ORCHESTRATOR             "
echo "========================================================="
echo ""

# Compile if needed
if [ ! -f server ]; then
    echo "[Orchestrator] Compiling C++ Core Storage Node..."
    g++ -std=c++11 -Wall -O2 src/server.cpp src/packet.cpp -o server -lpthread
    if [ $? -ne 0 ]; then
        echo "[Error] Compilation failed."
        exit 1
    fi
fi

# Clean up existing ports
killall server 2>/dev/null

echo "[Orchestrator] Launching C++ Core Daemon in background..."
./server &
SERVER_PID=$!

echo "[Orchestrator] Launching Python API Gateway..."
python3 gateway/api_gateway.py &
GATEWAY_PID=$!

sleep 2

# Open browser based on OS
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    xdg-open http://127.0.0.1:5000
elif [[ "$OSTYPE" == "darwin"* ]]; then
    open http://127.0.0.1:5000
fi

echo ""
echo "========================================================="
echo "AetherFlow is active! Press ENTER to terminate all nodes."
echo "========================================================="
echo ""

read

kill $SERVER_PID
kill $GATEWAY_PID
echo "[Orchestrator] Nodes shut down cleanly."

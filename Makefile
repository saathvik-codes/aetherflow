CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2

# Cross-platform settings
ifeq ($(OS),Windows_NT)
    LIBS = -lws2_32
    SERVER_EXE = server.exe
    CLIENT_EXE = client.exe
    RM = del /f /q
    PYTHON = python
else
    LIBS = -lpthread
    SERVER_EXE = server
    CLIENT_EXE = client
    RM = rm -f
    PYTHON = python3
endif

all: $(SERVER_EXE) $(CLIENT_EXE)

$(SERVER_EXE): src/server.cpp src/packet.cpp src/packet.h
	$(CXX) $(CXXFLAGS) src/server.cpp src/packet.cpp -o $(SERVER_EXE) $(LIBS)

$(CLIENT_EXE): src/client.cpp src/packet.cpp src/packet.h
	$(CXX) $(CXXFLAGS) src/client.cpp src/packet.cpp -o $(CLIENT_EXE) $(LIBS)

test: all
	$(PYTHON) tests/e2e_test.py

clean:
	$(RM) $(SERVER_EXE) $(CLIENT_EXE)

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <direct.h>
  typedef int socklen_t;
  #define make_dir(path) _mkdir(path)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <sys/stat.h>
  #include <signal.h>
  #include <thread>
  #include <mutex>
  typedef int SOCKET;
  const SOCKET INVALID_SOCKET = -1;
  const int SOCKET_ERROR = -1;
  #define closesocket(s) close(s)
  #define make_dir(path) mkdir(path, 0777)
#endif

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include <ctime>
#include <cstdio>
#include "packet.h"

// Client Session Context for File Ingestion
struct FileSession {
    std::string filename;
    uint32_t total_size;
    uint32_t bytes_written;
    std::ofstream file_stream;
    bool is_active;
};

struct ClientInfo {
    SOCKET socket;
    std::string ip;
    int port;
};

// Global socket handle for clean signal shutdown
SOCKET volatile listen_socket = INVALID_SOCKET;
bool volatile server_running = true;

// Thread-safe console & file logging
#ifdef _WIN32
  CRITICAL_SECTION log_cs;
  void init_log_mutex() { InitializeCriticalSection(&log_cs); }
  void lock_log() { EnterCriticalSection(&log_cs); }
  void unlock_log() { LeaveCriticalSection(&log_cs); }
#else
  std::mutex log_mutex;
  void init_log_mutex() {}
  void lock_log() { log_mutex.lock(); }
  void unlock_log() { log_mutex.unlock(); }
#endif

void log_message(const std::string& msg) {
    lock_log();
    
    char time_str[64];
#ifdef _WIN32
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             lt.wYear, lt.wMonth, lt.wDay,
             lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
#else
    std::time_t now = std::time(nullptr);
    struct tm time_info;
    localtime_r(&now, &time_info);
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_info);
#endif
    
    std::string log_entry = "[" + std::string(time_str) + "] [Server] " + msg;
    std::cout << log_entry << std::endl;
    
    std::ofstream log_file("server.log", std::ios::app);
    if (log_file.is_open()) {
        log_file << log_entry << "\n";
        log_file.close();
    }
    unlock_log();
}

// Client execution loop running in worker thread
void run_client_loop(SOCKET client_socket, std::string client_ip, int client_port) {
    log_message("Accepted client connection from " + client_ip + ":" + std::to_string(client_port));
    
    FileSession file_session;
    file_session.is_active = false;
    file_session.bytes_written = 0;
    
    std::vector<uint8_t> read_buffer;
    uint8_t temp_buffer[2048];
    
    while (server_running) {
        int bytes_received = recv(client_socket, reinterpret_cast<char*>(temp_buffer), sizeof(temp_buffer), 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                log_message("Client disconnected: " + client_ip);
            } else {
                log_message("Socket read error on client: " + client_ip);
            }
            break;
        }
        
        read_buffer.insert(read_buffer.end(), temp_buffer, temp_buffer + bytes_received);
        
        while (read_buffer.size() >= 8) {
            uint16_t length = (read_buffer[4] << 8) | read_buffer[5];
            size_t total_packet_size = 8 + length;
            
            if (read_buffer.size() < total_packet_size) {
                break; // Wait for complete packet payload
            }
            
            std::vector<uint8_t> packet_bytes(read_buffer.begin(), read_buffer.begin() + total_packet_size);
            read_buffer.erase(read_buffer.begin(), read_buffer.begin() + total_packet_size);
            
            Packet recv_packet;
            Packet response;
            response.magic1 = MAGIC_B1;
            response.magic2 = MAGIC_B2;
            response.version = 0x01;
            
            if (deserialize_packet(packet_bytes, recv_packet)) {
                switch (recv_packet.type) {
                    case PacketType::RBF_PING: {
                        response.type = PacketType::RBF_ACK;
                        std::string ack_msg = "PONG";
                        response.payload.assign(ack_msg.begin(), ack_msg.end());
                        log_message("Processed PING request from " + client_ip);
                        break;
                    }
                    case PacketType::RBF_DATA: {
                        std::string payload_str(recv_packet.payload.begin(), recv_packet.payload.end());
                        log_message("Processed DATA from " + client_ip + ": \"" + payload_str + "\"");
                        
                        response.type = PacketType::RBF_ACK;
                        std::string ack_msg = "ACK: Received " + std::to_string(recv_packet.length) + " bytes";
                        response.payload.assign(ack_msg.begin(), ack_msg.end());
                        break;
                    }
                    case PacketType::RBF_FILE_START: {
                        if (recv_packet.payload.size() < 5) {
                            response.type = PacketType::RBF_ERR;
                            std::string err = "Invalid FILE_START payload structure";
                            response.payload.assign(err.begin(), err.end());
                            break;
                        }
                        
                        // Parse Total File Size (Big-Endian uint32_t)
                        uint32_t total_size = (recv_packet.payload[0] << 24) | 
                                              (recv_packet.payload[1] << 16) | 
                                              (recv_packet.payload[2] << 8)  | 
                                              recv_packet.payload[3];
                        
                        // Parse Filename
                        std::string filename(recv_packet.payload.begin() + 4, recv_packet.payload.end());
                        
                        log_message("Initializing File Ingestion session: " + filename + " (" + std::to_string(total_size) + " bytes)");
                        
                        // Clean open file stream under uploads/
                        std::string target_path = "uploads/" + filename;
                        file_session.file_stream.open(target_path, std::ios::binary | std::ios::out | std::ios::trunc);
                        if (!file_session.file_stream.is_open()) {
                            log_message("FS Error: Failed to create target file: " + target_path);
                            response.type = PacketType::RBF_ERR;
                            std::string err = "Failed to create target file on server disk";
                            response.payload.assign(err.begin(), err.end());
                            break;
                        }
                        
                        file_session.filename = filename;
                        file_session.total_size = total_size;
                        file_session.bytes_written = 0;
                        file_session.is_active = true;
                        
                        response.type = PacketType::RBF_ACK;
                        std::string ack_msg = "FILE_START_OK";
                        response.payload.assign(ack_msg.begin(), ack_msg.end());
                        break;
                    }
                    case PacketType::RBF_FILE_CHUNK: {
                        if (!file_session.is_active) {
                            log_message("Protocol Error: Received FILE_CHUNK with no active session.");
                            response.type = PacketType::RBF_ERR;
                            std::string err = "No active upload session initialized";
                            response.payload.assign(err.begin(), err.end());
                            break;
                        }
                        
                        // Write block payload directly to file buffer
                        file_session.file_stream.write(reinterpret_cast<const char*>(recv_packet.payload.data()), recv_packet.payload.size());
                        file_session.bytes_written += recv_packet.payload.size();
                        
                        response.type = PacketType::RBF_ACK;
                        std::string ack_msg = "CHUNK_ACK:" + std::to_string(file_session.bytes_written);
                        response.payload.assign(ack_msg.begin(), ack_msg.end());
                        break;
                    }
                    case PacketType::RBF_FILE_END: {
                        if (!file_session.is_active) {
                            log_message("Protocol Error: Received FILE_END with no active session.");
                            response.type = PacketType::RBF_ERR;
                            std::string err = "No active upload session to close";
                            response.payload.assign(err.begin(), err.end());
                            break;
                        }
                        
                        file_session.file_stream.close();
                        log_message("Finished file ingestion. Wrote: " + std::to_string(file_session.bytes_written) + 
                                    "/" + std::to_string(file_session.total_size) + " bytes for file: " + file_session.filename);
                        
                        if (file_session.bytes_written != file_session.total_size) {
                            log_message("Verification Warning: Received bytes size mismatch!");
                            response.type = PacketType::RBF_ERR;
                            std::string err = "Transmitted byte size mismatch";
                            response.payload.assign(err.begin(), err.end());
                        } else {
                            response.type = PacketType::RBF_ACK;
                            std::string ack_msg = "FILE_COMPLETE:" + file_session.filename;
                            response.payload.assign(ack_msg.begin(), ack_msg.end());
                        }
                        file_session.is_active = false;
                        break;
                    }
                    default:
                        response.type = PacketType::RBF_ERR;
                        std::string err = "Unsupported packet type ID";
                        response.payload.assign(err.begin(), err.end());
                        break;
                }
                
                response.length = static_cast<uint16_t>(response.payload.size());
                response.checksum = calculate_checksum(response.payload.data(), response.length);
                
                std::vector<uint8_t> response_bytes = serialize_packet(response);
                send(client_socket, reinterpret_cast<const char*>(response_bytes.data()), response_bytes.size(), 0);
                
                // Graceful loop exit triggers
                if (recv_packet.type == PacketType::RBF_DATA && 
                    std::string(recv_packet.payload.begin(), recv_packet.payload.end()) == "exit") {
                    log_message("Ingested exit command from client. Terminating worker thread loop.");
                    closesocket(client_socket);
                    return;
                }
            } else {
                log_message("Bad packet structure or checksum error. Sending Error response.");
                Packet err_packet;
                err_packet.magic1 = MAGIC_B1;
                err_packet.magic2 = MAGIC_B2;
                err_packet.version = 0x01;
                err_packet.type = PacketType::RBF_ERR;
                std::string err_msg = "Bad Checksum or Header";
                err_packet.payload.assign(err_msg.begin(), err_msg.end());
                err_packet.length = static_cast<uint16_t>(err_packet.payload.size());
                err_packet.checksum = calculate_checksum(err_packet.payload.data(), err_packet.length);
                
                std::vector<uint8_t> err_bytes = serialize_packet(err_packet);
                send(client_socket, reinterpret_cast<const char*>(err_bytes.data()), err_bytes.size(), 0);
            }
        }
    }
    
    if (file_session.file_stream.is_open()) {
        file_session.file_stream.close();
    }
    closesocket(client_socket);
    log_message("Connection closed with " + client_ip);
}

#ifdef _WIN32
DWORD WINAPI handle_client_win(LPVOID lpParam) {
    ClientInfo* info = reinterpret_cast<ClientInfo*>(lpParam);
    run_client_loop(info->socket, info->ip, info->port);
    delete info;
    return 0;
}

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        log_message("Shutdown signal received. Closing listen socket and active streams...");
        server_running = false;
        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
        }
        return TRUE;
    }
    return FALSE;
}
#endif

int main() {
    init_log_mutex();
    
    // Auto-create uploads directory
    make_dir("uploads");
    log_message("Initializing system socket library...");
    
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[Error] WSAStartup failed." << std::endl;
        return 1;
    }
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        std::cerr << "[Error] Failed to create listen socket." << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    int optval = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[Error] Socket bind failed." << std::endl;
        closesocket(listen_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[Error] Socket listen failed." << std::endl;
        closesocket(listen_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    log_message("RBFTP Storage Server listening on port 8080...");

    while (server_running) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        SOCKET client_socket = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_socket == INVALID_SOCKET) {
            if (server_running) {
                log_message("Failed to accept incoming client socket connection.");
            }
            break;
        }

        std::string ip_str(inet_ntoa(client_addr.sin_addr));
        int client_port = ntohs(client_addr.sin_port);

#ifdef _WIN32
        ClientInfo* info = new ClientInfo{ client_socket, ip_str, client_port };
        HANDLE hThread = CreateThread(NULL, 0, handle_client_win, reinterpret_cast<LPVOID>(info), 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            log_message("Error: Failed to create Windows thread.");
            delete info;
            closesocket(client_socket);
        }
#else
        std::thread client_thread(run_client_loop, client_socket, ip_str, client_port);
        client_thread.detach();
#endif
    }

    log_message("Exiting main server thread. cleaning socket connections.");
    if (listen_socket != INVALID_SOCKET) {
        closesocket(listen_socket);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

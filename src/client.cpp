#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #define sleep_ms(ms) Sleep(ms)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <arpa/inet.h>
  #define sleep_ms(ms) usleep((ms) * 1000)
  typedef int SOCKET;
  const SOCKET INVALID_SOCKET = -1;
  const int SOCKET_ERROR = -1;
  #define closesocket(s) close(s)
#endif

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include <chrono>
#include "packet.h"

// Helper function to send packet and read synchronous ACK response
bool send_and_receive(SOCKET socket, Packet& packet, Packet& out_response) {
    if (packet.type != PacketType::RBF_FILE_CHUNK && packet.length > 0) {
        packet.checksum = calculate_checksum(packet.payload.data(), packet.length);
    }
    
    std::vector<uint8_t> buffer = serialize_packet(packet);
    
    int sent_bytes = send(socket, reinterpret_cast<const char*>(buffer.data()), buffer.size(), 0);
    if (sent_bytes == SOCKET_ERROR) {
        std::cerr << "\n[Client Error] Transmission send failed." << std::endl;
        return false;
    }
    
    std::vector<uint8_t> response_header(8);
    int received_bytes = recv(socket, reinterpret_cast<char*>(response_header.data()), 8, 0);
    if (received_bytes <= 0) {
        std::cerr << "\n[Client Error] Server closed connection or response header timeout." << std::endl;
        return false;
    }
    
    uint16_t length = (response_header[4] << 8) | response_header[5];
    
    std::vector<uint8_t> response_buffer = response_header;
    if (length > 0) {
        response_buffer.resize(8 + length);
        int payload_received = recv(socket, reinterpret_cast<char*>(response_buffer.data() + 8), length, 0);
        if (payload_received <= 0) {
            std::cerr << "\n[Client Error] Server closed connection during response payload transfer." << std::endl;
            return false;
        }
    }
    
    if (deserialize_packet(response_buffer, out_response)) {
        return true;
    } else {
        std::cerr << "\n[Client Error] Invalid checksum or header returned from server." << std::endl;
        return false;
    }
}

void print_help() {
    std::cout << "\nAvailable Commands:\n"
              << "  ping            - Probe connection and measure server response latency\n"
              << "  send <filepath> - Upload a local file to the server with validation checks\n"
              << "  help            - Display available commands\n"
              << "  exit            - Cleanly terminate socket session and exit\n" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[Client Error] WSAStartup library load failed." << std::endl;
        return 1;
    }
#endif

    SOCKET conn_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (conn_socket == INVALID_SOCKET) {
        std::cerr << "[Client Error] Socket creation failed." << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    std::cout << "[Client] Connecting to RBFTP Server at 127.0.0.1:8080..." << std::endl;
    if (connect(conn_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[Client Error] Could not connect to RBFTP Server. Make sure the server is running." << std::endl;
        closesocket(conn_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "=========================================================\n"
              << "    RBFTP SYSTEMS CLIENT INTERACTIVE INTERFACE - v1.0   \n"
              << "=========================================================\n"
              << "Type 'help' to view available system commands.\n" << std::endl;

    std::string line;
    while (true) {
        std::cout << "RBFTP> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break; // EOF (e.g. piped tests input)
        }

        // Trim leading and trailing whitespace
        size_t start_pos = line.find_first_not_of(" \t\r\n");
        if (start_pos == std::string::npos) {
            continue; // Empty line
        }
        size_t end_pos = line.find_last_not_of(" \t\r\n");
        line = line.substr(start_pos, end_pos - start_pos + 1);

        // Simple space tokenization
        std::string cmd;
        std::string arg;
        size_t space_idx = line.find(' ');
        if (space_idx == std::string::npos) {
            cmd = line;
        } else {
            cmd = line.substr(0, space_idx);
            arg = line.substr(space_idx + 1);
            
            // Trim argument whitespace
            size_t arg_start = arg.find_first_not_of(" \t\r\n");
            if (arg_start != std::string::npos) {
                size_t arg_end = arg.find_last_not_of(" \t\r\n");
                arg = arg.substr(arg_start, arg_end - arg_start + 1);
            } else {
                arg = "";
            }
        }

        if (cmd == "help") {
            print_help();
        } 
        else if (cmd == "ping") {
            auto start_time = std::chrono::high_resolution_clock::now();
            
            Packet ping_pkt;
            ping_pkt.magic1 = MAGIC_B1;
            ping_pkt.magic2 = MAGIC_B2;
            ping_pkt.version = 0x01;
            ping_pkt.type = PacketType::RBF_PING;
            ping_pkt.length = 0;
            
            Packet response;
            if (send_and_receive(conn_socket, ping_pkt, response)) {
                auto end_time = std::chrono::high_resolution_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
                std::string res_msg(response.payload.begin(), response.payload.end());
                std::cout << "[Latency Probe] RTT: " << elapsed_ms << " ms. Response: \"" << res_msg << "\"" << std::endl;
            } else {
                std::cout << "[Latency Probe] Connection lost or request failed." << std::endl;
            }
        } 
        else if (cmd == "send") {
            if (arg.empty()) {
                std::cout << "[Usage Error] syntax: send <filepath>" << std::endl;
                continue;
            }

            std::ifstream file_in(arg, std::ios::binary | std::ios::ate);
            if (!file_in.is_open()) {
                std::cout << "[File Error] Failed to open local file path: " << arg << std::endl;
                continue;
            }

            uint32_t file_size = static_cast<uint32_t>(file_in.tellg());
            file_in.seekg(0, std::ios::beg);

            // Parse filename from path
            std::string filename = arg;
            size_t last_slash = arg.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                filename = arg.substr(last_slash + 1);
            }

            // 1. FILE_START Packet
            Packet start_pkt;
            start_pkt.magic1 = MAGIC_B1;
            start_pkt.magic2 = MAGIC_B2;
            start_pkt.version = 0x01;
            start_pkt.type = PacketType::RBF_FILE_START;
            
            // Payload: [uint32_t size Big-Endian] [Filename]
            start_pkt.payload.push_back((file_size >> 24) & 0xFF);
            start_pkt.payload.push_back((file_size >> 16) & 0xFF);
            start_pkt.payload.push_back((file_size >> 8) & 0xFF);
            start_pkt.payload.push_back(file_size & 0xFF);
            start_pkt.payload.insert(start_pkt.payload.end(), filename.begin(), filename.end());
            start_pkt.length = static_cast<uint16_t>(start_pkt.payload.size());

            Packet response;
            std::cout << "[RBFTP] Initializing upload stream for: " << filename << " (" << file_size << " bytes)" << std::endl;
            if (!send_and_receive(conn_socket, start_pkt, response) || response.type == PacketType::RBF_ERR) {
                std::string err_reason(response.payload.begin(), response.payload.end());
                std::cout << "[RBFTP Error] Initialization rejected: " << err_reason << std::endl;
                file_in.close();
                continue;
            }

            // 2. Stream FILE_CHUNK packets with Live Progress bar
            std::vector<char> block_buffer(1024); // Use 1KB block chunks for realistic networks
            uint32_t total_sent = 0;
            bool upload_ok = true;

            while (file_in) {
                file_in.read(block_buffer.data(), block_buffer.size());
                std::streamsize bytes_read = file_in.gcount();
                if (bytes_read <= 0) break;

                Packet chunk_pkt;
                chunk_pkt.magic1 = MAGIC_B1;
                chunk_pkt.magic2 = MAGIC_B2;
                chunk_pkt.version = 0x01;
                chunk_pkt.type = PacketType::RBF_FILE_CHUNK;
                chunk_pkt.length = static_cast<uint16_t>(bytes_read);
                chunk_pkt.payload.assign(block_buffer.begin(), block_buffer.begin() + bytes_read);
                chunk_pkt.checksum = calculate_checksum(chunk_pkt.payload.data(), chunk_pkt.length);

                if (send_and_receive(conn_socket, chunk_pkt, response)) {
                    if (response.type == PacketType::RBF_ERR) {
                        std::string err_reason(response.payload.begin(), response.payload.end());
                        std::cout << "\n[RBFTP Error] Chunk transmission rejected: " << err_reason << std::endl;
                        upload_ok = false;
                        break;
                    }
                    total_sent += bytes_read;
                    
                    // Render real-time console progress bar
                    float progress = (float)total_sent / file_size;
                    int barWidth = 30;
                    std::cout << "\rUploading: [";
                    int pos = barWidth * progress;
                    for (int i = 0; i < barWidth; ++i) {
                        if (i < pos) std::cout << "=";
                        else if (i == pos) std::cout << ">";
                        else std::cout << " ";
                    }
                    std::cout << "] " << int(progress * 100.0) << "% (" << total_sent << "/" << file_size << " bytes)" << std::flush;
                } else {
                    std::cout << "\n[RBFTP Error] Checksum receipt timeout." << std::endl;
                    upload_ok = false;
                    break;
                }
                sleep_ms(15); // Paced delay for rendering smoothness
            }
            file_in.close();
            std::cout << std::endl; // Carriage return reset

            if (!upload_ok) {
                continue;
            }

            // 3. FILE_END Finalization Packet
            Packet end_pkt;
            end_pkt.magic1 = MAGIC_B1;
            end_pkt.magic2 = MAGIC_B2;
            end_pkt.version = 0x01;
            end_pkt.type = PacketType::RBF_FILE_END;
            end_pkt.length = 0;

            std::cout << "[RBFTP] Finalizing upload block and requesting disk hash matches..." << std::endl;
            if (send_and_receive(conn_socket, end_pkt, response)) {
                std::string status_msg(response.payload.begin(), response.payload.end());
                if (response.type == PacketType::RBF_ERR) {
                    std::cout << "[RBFTP Error] Disk assembly failure: " << status_msg << std::endl;
                } else {
                    std::cout << "[RBFTP Success] Assembly verified: " << status_msg << std::endl;
                }
            } else {
                std::cout << "[RBFTP Error] Assembly verification timeout." << std::endl;
            }
        } 
        else if (cmd == "exit") {
            // Signal clean shutdown to server
            Packet exit_pkt;
            exit_pkt.magic1 = MAGIC_B1;
            exit_pkt.magic2 = MAGIC_B2;
            exit_pkt.version = 0x01;
            exit_pkt.type = PacketType::RBF_DATA;
            std::string exit_msg = "exit";
            exit_pkt.payload.assign(exit_msg.begin(), exit_msg.end());
            exit_pkt.length = static_cast<uint16_t>(exit_pkt.payload.size());
            
            Packet response;
            send_and_receive(conn_socket, exit_pkt, response);
            
            std::cout << "[Client] Closing system sockets. Goodbye!" << std::endl;
            break;
        } 
        else {
            std::cout << "Unknown command: \"" << cmd << "\". Type 'help' for support list." << std::endl;
        }
    }

    closesocket(conn_socket);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

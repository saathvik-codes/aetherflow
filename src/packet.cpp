#include "packet.h"
#include <iostream>

// Standard 16-bit internet checksum calculation
uint16_t calculate_checksum(const uint8_t* data, size_t length) {
    uint32_t sum = 0;
    
    // Add 16-bit words
    for (size_t i = 0; i < length - 1; i += 2) {
        uint16_t word = (data[i] << 8) | data[i + 1];
        sum += word;
    }
    
    // Handle odd byte if necessary
    if (length % 2 != 0) {
        sum += (data[length - 1] << 8);
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return static_cast<uint16_t>(~sum);
}

// Serialize Packet struct into binary byte stream (handling big-endian encoding)
std::vector<uint8_t> serialize_packet(const Packet& packet) {
    std::vector<uint8_t> buffer;
    buffer.reserve(8 + packet.payload.size());
    
    // Magic Bytes
    buffer.push_back(packet.magic1);
    buffer.push_back(packet.magic2);
    
    // Version
    buffer.push_back(packet.version);
    
    // Type
    buffer.push_back(static_cast<uint8_t>(packet.type));
    
    // Length (Big-Endian)
    buffer.push_back(static_cast<uint8_t>((packet.length >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(packet.length & 0xFF));
    
    // Checksum (Big-Endian)
    buffer.push_back(static_cast<uint8_t>((packet.checksum >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(packet.checksum & 0xFF));
    
    // Payload
    buffer.insert(buffer.end(), packet.payload.begin(), packet.payload.end());
    
    return buffer;
}

// Deserialize binary byte stream back into Packet struct
bool deserialize_packet(const std::vector<uint8_t>& buffer, Packet& out_packet) {
    if (buffer.size() < 8) {
        std::cerr << "Deserialization error: Buffer size too small (" << buffer.size() << " bytes)." << std::endl;
        return false;
    }
    
    // Magic Bytes check
    out_packet.magic1 = buffer[0];
    out_packet.magic2 = buffer[1];
    if (out_packet.magic1 != MAGIC_B1 || out_packet.magic2 != MAGIC_B2) {
        std::cerr << "Deserialization error: Invalid magic bytes." << std::endl;
        return false;
    }
    
    // Version check
    out_packet.version = buffer[2];
    if (out_packet.version != 0x01) {
        std::cerr << "Deserialization error: Unsupported version." << std::endl;
        return false;
    }
    
    // Type (supporting standard packets 0x01-0x04 and file packets 0x05-0x07)
    uint8_t raw_type = buffer[3];
    if (raw_type < 1 || raw_type > 7) {
        std::cerr << "Deserialization error: Invalid packet type." << std::endl;
        return false;
    }
    out_packet.type = static_cast<PacketType>(raw_type);
    
    // Length (Big-Endian)
    out_packet.length = (buffer[4] << 8) | buffer[5];
    
    // Checksum (Big-Endian)
    out_packet.checksum = (buffer[6] << 8) | buffer[7];
    
    // Check buffer contains the entire payload
    if (buffer.size() < static_cast<size_t>(8 + out_packet.length)) {
        std::cerr << "Deserialization error: Payload size mismatch. Expected: " 
                  << out_packet.length << ", got: " << (buffer.size() - 8) << std::endl;
        return false;
    }
    
    // Copy Payload
    out_packet.payload.assign(buffer.begin() + 8, buffer.begin() + 8 + out_packet.length);
    
    // Verify Checksum
    if (out_packet.length > 0) {
        uint16_t computed_checksum = calculate_checksum(out_packet.payload.data(), out_packet.length);
        if (computed_checksum != out_packet.checksum) {
            std::cerr << "Deserialization error: Checksum verification failed. Expected: " 
                      << out_packet.checksum << ", computed: " << computed_checksum << std::endl;
            return false;
        }
    }
    
    return true;
}

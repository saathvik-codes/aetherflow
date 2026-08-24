#ifndef PACKET_H
#define PACKET_H

#include <vector>
#include <cstdint>
#include <string>

// Magic bytes: 'Q' and 'C'
const uint8_t MAGIC_B1 = 0x51;
const uint8_t MAGIC_B2 = 0x43;

// Packet Types prefixed to prevent namespace clashes with Windows SDK headers
enum class PacketType : uint8_t {
    RBF_PING       = 0x01,
    RBF_DATA       = 0x02,
    RBF_ACK        = 0x03,
    RBF_ERR        = 0x04,
    RBF_FILE_START = 0x05,
    RBF_FILE_CHUNK = 0x06,
    RBF_FILE_END   = 0x07
};

// Structured binary packet
struct Packet {
    uint8_t magic1;
    uint8_t magic2;
    uint8_t version;
    PacketType type;
    uint16_t length;    // Payload length
    uint16_t checksum;  // Internet checksum (16-bit)
    std::vector<uint8_t> payload;
};

// Helper functions for checksum calculation and packet serialization
uint16_t calculate_checksum(const uint8_t* data, size_t length);
std::vector<uint8_t> serialize_packet(const Packet& packet);
bool deserialize_packet(const std::vector<uint8_t>& buffer, Packet& out_packet);

#endif // PACKET_H

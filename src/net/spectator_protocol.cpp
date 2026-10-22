#include "net/spectator_protocol.h"

namespace Net::Spectator {

const char* PacketTypeName(PacketType type) {
    switch (type) {
        case PacketType::Hello: return "Hello";
        case PacketType::HelloAck: return "HelloAck";
        case PacketType::Redirect: return "Redirect";
        case PacketType::MatchState: return "MatchState";
        case PacketType::FrameBatch: return "FrameBatch";
        case PacketType::PaletteState: return "PaletteState";
        case PacketType::PaletteData: return "PaletteData";
        case PacketType::Heartbeat: return "Heartbeat";
        case PacketType::Disconnect: return "Disconnect";
        case PacketType::ClientStatus: return "ClientStatus";
        case PacketType::PreMatchState: return "PreMatchState";
        default: return "Unknown";
    }
}

} // namespace Net::Spectator
/**
 * Alice Senki 2 - Custom netplay config storage
 *
 * Menu-owned configuration that is persisted locally, mirrored into the
 * game's legacy netplay fields, and exposed for future transport/session code.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NetplayConfig {

constexpr uint32_t kNicknameCap = 21;
constexpr uint32_t kRecentPeerLimit = 4;

struct RecentPeer {
    bool valid;
    char label[kNicknameCap];
    uint32_t target_ip[4];
    uint32_t target_port;
    uint32_t build_signature;
    uint32_t successful_connections;
    uint64_t last_used_unix;
};

struct Config {
    char nickname[kNicknameCap];
    uint16_t listen_port;
    uint32_t target_ip[4];
    uint32_t target_port;
    int preferred_delay_frames;
    bool verbose_logging;
    bool show_connection_stats;
    bool show_netplay_hud;
    uint32_t recent_peer_count;
    RecentPeer recent_peers[kRecentPeerLimit];
};

void SetDefaults(Config* out);
void Clamp(Config* config);
bool Load(Config* out);
bool Save(const Config* config);
bool Commit(Config* config);
void ApplyToGameMemory(const Config* config);
void RecordRecentPeer(Config* config, const char* label, uint32_t build_signature);
bool GetRecentPeer(const Config* config, uint32_t index, RecentPeer* out);
bool ApplyRecentPeer(Config* config, uint32_t index);
bool ParseEndpoint(const char* text, uint32_t out_ip[4], uint32_t* out_port);
bool ApplyEndpointString(Config* config, const char* text);
void FormatEndpoint(const Config* config, char* out, size_t cap);
void FormatRecentPeerLabel(const RecentPeer* peer, char* out, size_t cap);
const char* GetConfigFilename();

} // namespace NetplayConfig

#include "netplay_config.h"

#include "as2_rollback.h"
#include "log_window.h"
#include "netplay_hooks.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace NetplayConfig {

namespace {

constexpr uintptr_t ADDR_MY_NICKNAME = 0x8E9468;
constexpr uintptr_t ADDR_MY_PORT = 0x8E9480;
constexpr uintptr_t ADDR_MY_TARGET_IP1 = 0x8E9484;
constexpr uintptr_t ADDR_MY_TARGET_IP2 = 0x8E9488;
constexpr uintptr_t ADDR_MY_TARGET_IP3 = 0x8E948C;
constexpr uintptr_t ADDR_MY_TARGET_IP4 = 0x8E9490;
constexpr uintptr_t ADDR_MY_TARGET_PORT = 0x8E9494;
constexpr uintptr_t ADDR_LOBBY_PORT = 0x7AC234;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP1 = 0x7AC260;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP2 = 0x7AC264;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP3 = 0x7AC268;
constexpr uintptr_t ADDR_LOBBY_TARGET_IP4 = 0x7AC26C;
constexpr uintptr_t ADDR_LOBBY_TARGET_PORT = 0x7AC270;
constexpr uintptr_t ADDR_LOBBY_MY_NICK = 0x7AC274;

constexpr const char* kConfigFilename = "as2_netplay.cfg";
constexpr char kFileMagic[8] = { 'A', 'S', '2', 'N', 'P', '0', '1', '\0' };
constexpr uint32_t kFileVersion = 1;

#pragma pack(push, 1)
struct DiskRecentPeer {
    uint8_t valid;
    char label[kNicknameCap];
    uint32_t target_ip[4];
    uint32_t target_port;
    uint32_t build_signature;
    uint32_t successful_connections;
    uint64_t last_used_unix;
};

struct DiskConfigFile {
    char magic[8];
    uint32_t version;
    char nickname[kNicknameCap];
    uint16_t listen_port;
    uint32_t target_ip[4];
    uint32_t target_port;
    int32_t preferred_delay_frames;
    uint8_t verbose_logging;
    uint8_t show_connection_stats;
    uint8_t show_netplay_hud;
    uint8_t reserved0;
    uint32_t recent_peer_count;
    DiskRecentPeer recent_peers[kRecentPeerLimit];
};
#pragma pack(pop)

static void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, cap, src, _TRUNCATE);
}

static const char* SkipSpaces(const char* text) {
    if (!text) return "";
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    return text;
}

static void TrimTrailingSpaces(char* text) {
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0) {
        const char c = text[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        text[len - 1] = '\0';
        --len;
    }
}

static uint16_t ReadU16(uintptr_t a, uint16_t d = 0) {
    __try { return *(volatile uint16_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return d; }
}

static uint32_t ReadU32(uintptr_t a, uint32_t d = 0) {
    __try { return *(volatile uint32_t*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return d; }
}

static void WriteU16(uintptr_t a, uint16_t v) {
    __try { *(volatile uint16_t*)a = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void WriteU32(uintptr_t a, uint32_t v) {
    __try { *(volatile uint32_t*)a = v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int ClampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void WriteNickname(uintptr_t addr, const char* text) {
    char temp[kNicknameCap]{};
    CopyText(temp, sizeof(temp), text);
    __try {
        memset((void*)addr, 0, kNicknameCap);
        memcpy((void*)addr, temp, strlen(temp));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void LoadFromGameMemory(Config* out) {
    if (!out) return;
    SetDefaults(out);
    __try { strncpy_s(out->nickname, sizeof(out->nickname), (const char*)ADDR_MY_NICKNAME, _TRUNCATE); } __except (EXCEPTION_EXECUTE_HANDLER) { out->nickname[0] = '\0'; }
    if (!out->nickname[0]) CopyText(out->nickname, sizeof(out->nickname), "Player");
    out->listen_port = ReadU16(ADDR_MY_PORT, out->listen_port);
    out->target_ip[0] = (uint32_t)ClampInt((int)ReadU32(ADDR_MY_TARGET_IP1, out->target_ip[0]), 0, 255);
    out->target_ip[1] = (uint32_t)ClampInt((int)ReadU32(ADDR_MY_TARGET_IP2, out->target_ip[1]), 0, 255);
    out->target_ip[2] = (uint32_t)ClampInt((int)ReadU32(ADDR_MY_TARGET_IP3, out->target_ip[2]), 0, 255);
    out->target_ip[3] = (uint32_t)ClampInt((int)ReadU32(ADDR_MY_TARGET_IP4, out->target_ip[3]), 0, 255);
    out->target_port = ReadU32(ADDR_MY_TARGET_PORT, out->target_port);
    out->preferred_delay_frames = NetplayHooks::GetNetplayFrameDelay();
    out->verbose_logging = GetVerboseLogging();
}

static uint64_t CurrentUnixTime() {
    return (uint64_t)time(nullptr);
}

static bool SameEndpoint(const RecentPeer* a, const Config* cfg) {
    if (!a || !cfg || !a->valid) return false;
    return a->target_ip[0] == cfg->target_ip[0] &&
        a->target_ip[1] == cfg->target_ip[1] &&
        a->target_ip[2] == cfg->target_ip[2] &&
        a->target_ip[3] == cfg->target_ip[3] &&
        a->target_port == cfg->target_port;
}

} // namespace

void SetDefaults(Config* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    CopyText(out->nickname, sizeof(out->nickname), "Player");
    out->listen_port = 10700;
    out->target_ip[0] = 127;
    out->target_ip[1] = 0;
    out->target_ip[2] = 0;
    out->target_ip[3] = 1;
    out->target_port = 10700;
    out->preferred_delay_frames = 0;
    out->verbose_logging = false;
    out->show_connection_stats = true;
    out->show_netplay_hud = true;
}

void Clamp(Config* config) {
    if (!config) return;
    if (!config->nickname[0]) CopyText(config->nickname, sizeof(config->nickname), "Player");
    config->listen_port = (uint16_t)ClampInt((int)config->listen_port, 1, 65535);
    config->target_port = (uint32_t)ClampInt((int)config->target_port, 1, 65535);
    config->preferred_delay_frames = ClampInt(config->preferred_delay_frames, 0, 6);
    for (int i = 0; i < 4; ++i) {
        config->target_ip[i] = (uint32_t)ClampInt((int)config->target_ip[i], 0, 255);
    }
    if (config->recent_peer_count > kRecentPeerLimit) {
        config->recent_peer_count = kRecentPeerLimit;
    }
    for (uint32_t i = 0; i < kRecentPeerLimit; ++i) {
        RecentPeer& peer = config->recent_peers[i];
        if (!peer.valid) continue;
        if (!peer.label[0]) CopyText(peer.label, sizeof(peer.label), "Recent Peer");
        peer.target_port = (uint32_t)ClampInt((int)peer.target_port, 1, 65535);
        for (int octet = 0; octet < 4; ++octet) {
            peer.target_ip[octet] = (uint32_t)ClampInt((int)peer.target_ip[octet], 0, 255);
        }
    }
}

bool Load(Config* out) {
    if (!out) return false;

    LoadFromGameMemory(out);

    FILE* f = fopen(kConfigFilename, "rb");
    if (!f) {
        LOG_NETPLAY(LOG_DEBUG, "[Config] No %s found; using defaults/game memory.", kConfigFilename);
        Clamp(out);
        return false;
    }

    DiskConfigFile disk{};
    const size_t read = fread(&disk, sizeof(disk), 1, f);
    fclose(f);
    if (read != 1 || memcmp(disk.magic, kFileMagic, sizeof(kFileMagic)) != 0 || disk.version != kFileVersion) {
        LOG_NETPLAY(LOG_WARNING, "[Config] Ignoring invalid %s; using defaults/game memory.", kConfigFilename);
        Clamp(out);
        return false;
    }

    memset(out, 0, sizeof(*out));
    CopyText(out->nickname, sizeof(out->nickname), disk.nickname);
    out->listen_port = disk.listen_port;
    for (int i = 0; i < 4; ++i) out->target_ip[i] = disk.target_ip[i];
    out->target_port = disk.target_port;
    out->preferred_delay_frames = disk.preferred_delay_frames;
    out->verbose_logging = disk.verbose_logging != 0;
    out->show_connection_stats = disk.show_connection_stats != 0;
    out->show_netplay_hud = disk.show_netplay_hud != 0;
    out->recent_peer_count = disk.recent_peer_count;
    for (uint32_t i = 0; i < kRecentPeerLimit; ++i) {
        out->recent_peers[i].valid = disk.recent_peers[i].valid != 0;
        CopyText(out->recent_peers[i].label, sizeof(out->recent_peers[i].label), disk.recent_peers[i].label);
        for (int octet = 0; octet < 4; ++octet) {
            out->recent_peers[i].target_ip[octet] = disk.recent_peers[i].target_ip[octet];
        }
        out->recent_peers[i].target_port = disk.recent_peers[i].target_port;
        out->recent_peers[i].build_signature = disk.recent_peers[i].build_signature;
        out->recent_peers[i].successful_connections = disk.recent_peers[i].successful_connections;
        out->recent_peers[i].last_used_unix = disk.recent_peers[i].last_used_unix;
    }

    Clamp(out);
    LOG_NETPLAY(LOG_INFO,
        "[Config] Loaded %s: nick='%s' target=%u.%u.%u.%u:%u delay=%d recent=%u",
        kConfigFilename,
        out->nickname,
        out->target_ip[0], out->target_ip[1], out->target_ip[2], out->target_ip[3],
        out->target_port,
        out->preferred_delay_frames,
        out->recent_peer_count);
    return true;
}

bool Save(const Config* config) {
    if (!config) return false;

    Config safe = *config;
    Clamp(&safe);

    DiskConfigFile disk{};
    memcpy(disk.magic, kFileMagic, sizeof(kFileMagic));
    disk.version = kFileVersion;
    CopyText(disk.nickname, sizeof(disk.nickname), safe.nickname);
    disk.listen_port = safe.listen_port;
    for (int i = 0; i < 4; ++i) disk.target_ip[i] = safe.target_ip[i];
    disk.target_port = safe.target_port;
    disk.preferred_delay_frames = safe.preferred_delay_frames;
    disk.verbose_logging = safe.verbose_logging ? 1 : 0;
    disk.show_connection_stats = safe.show_connection_stats ? 1 : 0;
    disk.show_netplay_hud = safe.show_netplay_hud ? 1 : 0;
    disk.recent_peer_count = safe.recent_peer_count;
    for (uint32_t i = 0; i < kRecentPeerLimit; ++i) {
        disk.recent_peers[i].valid = safe.recent_peers[i].valid ? 1 : 0;
        CopyText(disk.recent_peers[i].label, sizeof(disk.recent_peers[i].label), safe.recent_peers[i].label);
        for (int octet = 0; octet < 4; ++octet) {
            disk.recent_peers[i].target_ip[octet] = safe.recent_peers[i].target_ip[octet];
        }
        disk.recent_peers[i].target_port = safe.recent_peers[i].target_port;
        disk.recent_peers[i].build_signature = safe.recent_peers[i].build_signature;
        disk.recent_peers[i].successful_connections = safe.recent_peers[i].successful_connections;
        disk.recent_peers[i].last_used_unix = safe.recent_peers[i].last_used_unix;
    }

    FILE* f = fopen(kConfigFilename, "wb");
    if (!f) {
        LOG_NETPLAY(LOG_WARNING, "[Config] Failed to open %s for writing.", kConfigFilename);
        return false;
    }

    const size_t written = fwrite(&disk, sizeof(disk), 1, f);
    fclose(f);
    if (written != 1) {
        LOG_NETPLAY(LOG_WARNING, "[Config] Failed to fully write %s.", kConfigFilename);
        return false;
    }

    return true;
}

void ApplyToGameMemory(const Config* config) {
    if (!config) return;
    Config safe = *config;
    Clamp(&safe);

    WriteNickname(ADDR_MY_NICKNAME, safe.nickname);
    WriteNickname(ADDR_LOBBY_MY_NICK, safe.nickname);
    WriteU16(ADDR_MY_PORT, safe.listen_port);
    WriteU32(ADDR_MY_TARGET_IP1, safe.target_ip[0]);
    WriteU32(ADDR_MY_TARGET_IP2, safe.target_ip[1]);
    WriteU32(ADDR_MY_TARGET_IP3, safe.target_ip[2]);
    WriteU32(ADDR_MY_TARGET_IP4, safe.target_ip[3]);
    WriteU32(ADDR_MY_TARGET_PORT, safe.target_port);
    WriteU32(ADDR_LOBBY_PORT, safe.listen_port);
    WriteU32(ADDR_LOBBY_TARGET_IP1, safe.target_ip[0]);
    WriteU32(ADDR_LOBBY_TARGET_IP2, safe.target_ip[1]);
    WriteU32(ADDR_LOBBY_TARGET_IP3, safe.target_ip[2]);
    WriteU32(ADDR_LOBBY_TARGET_IP4, safe.target_ip[3]);
    WriteU32(ADDR_LOBBY_TARGET_PORT, safe.target_port);
    NetplayHooks::SetNetplayFrameDelay(safe.preferred_delay_frames);
    SetVerboseLogging(safe.verbose_logging);
}

bool Commit(Config* config) {
    if (!config) return false;
    Clamp(config);
    ApplyToGameMemory(config);
    return Save(config);
}

void RecordRecentPeer(Config* config, const char* label, uint32_t build_signature) {
    if (!config) return;
    Clamp(config);

    uint32_t found = kRecentPeerLimit;
    for (uint32_t i = 0; i < config->recent_peer_count && i < kRecentPeerLimit; ++i) {
        if (SameEndpoint(&config->recent_peers[i], config)) {
            found = i;
            break;
        }
    }

    RecentPeer peer{};
    peer.valid = true;
    CopyText(peer.label, sizeof(peer.label), label && label[0] ? label : "Recent Peer");
    for (int i = 0; i < 4; ++i) peer.target_ip[i] = config->target_ip[i];
    peer.target_port = config->target_port;
    peer.build_signature = build_signature;
    peer.successful_connections = 1;
    peer.last_used_unix = CurrentUnixTime();

    if (found < kRecentPeerLimit) {
        const RecentPeer& existing = config->recent_peers[found];
        if (existing.successful_connections > 0) {
            peer.successful_connections = existing.successful_connections + 1;
        }
    }

    const uint32_t oldCount = config->recent_peer_count < kRecentPeerLimit ? config->recent_peer_count : kRecentPeerLimit;
    if (found == kRecentPeerLimit) {
        for (uint32_t i = oldCount; i > 0; --i) {
            if (i < kRecentPeerLimit) {
                config->recent_peers[i] = config->recent_peers[i - 1];
            }
        }
        config->recent_peers[0] = peer;
        if (config->recent_peer_count < kRecentPeerLimit) {
            ++config->recent_peer_count;
        }
    } else {
        for (uint32_t i = found; i > 0; --i) {
            config->recent_peers[i] = config->recent_peers[i - 1];
        }
        config->recent_peers[0] = peer;
    }

    LOG_NETPLAY(LOG_INFO,
        "[Config] Recorded recent peer '%s' at %u.%u.%u.%u:%u (build=%08X, total=%u)",
        peer.label,
        peer.target_ip[0], peer.target_ip[1], peer.target_ip[2], peer.target_ip[3],
        peer.target_port,
        peer.build_signature,
        peer.successful_connections);
}

bool GetRecentPeer(const Config* config, uint32_t index, RecentPeer* out) {
    if (!config || !out) return false;
    if (index >= config->recent_peer_count || index >= kRecentPeerLimit) return false;
    if (!config->recent_peers[index].valid) return false;
    *out = config->recent_peers[index];
    return true;
}

bool ApplyRecentPeer(Config* config, uint32_t index) {
    if (!config) return false;
    RecentPeer peer{};
    if (!GetRecentPeer(config, index, &peer)) return false;
    for (int i = 0; i < 4; ++i) config->target_ip[i] = peer.target_ip[i];
    config->target_port = peer.target_port;
    Clamp(config);
    return true;
}

bool ParseEndpoint(const char* text, uint32_t out_ip[4], uint32_t* out_port) {
    if (!text || !out_ip || !out_port) return false;

    char trimmed[64];
    CopyText(trimmed, sizeof(trimmed), SkipSpaces(text));
    TrimTrailingSpaces(trimmed);
    if (!trimmed[0]) return false;

    unsigned int ip0 = 0;
    unsigned int ip1 = 0;
    unsigned int ip2 = 0;
    unsigned int ip3 = 0;
    unsigned int port = 0;
    char tail[2] = {};
    const int matched = sscanf_s(trimmed, "%u.%u.%u.%u:%u%1s", &ip0, &ip1, &ip2, &ip3, &port, tail, (unsigned)sizeof(tail));
    if (matched != 5) return false;
    if (ip0 > 255 || ip1 > 255 || ip2 > 255 || ip3 > 255 || port == 0 || port > 65535) return false;

    out_ip[0] = ip0;
    out_ip[1] = ip1;
    out_ip[2] = ip2;
    out_ip[3] = ip3;
    *out_port = port;
    return true;
}

bool ApplyEndpointString(Config* config, const char* text) {
    if (!config) return false;

    uint32_t ip[4]{};
    uint32_t port = 0;
    if (!ParseEndpoint(text, ip, &port)) {
        return false;
    }

    for (int i = 0; i < 4; ++i) {
        config->target_ip[i] = ip[i];
    }
    config->target_port = port;
    Clamp(config);
    return true;
}

void FormatEndpoint(const Config* config, char* out, size_t cap) {
    if (!out || cap == 0) return;
    if (!config) {
        out[0] = '\0';
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%u.%u.%u.%u:%u",
        config->target_ip[0], config->target_ip[1], config->target_ip[2], config->target_ip[3], config->target_port);
}

void FormatRecentPeerLabel(const RecentPeer* peer, char* out, size_t cap) {
    if (!out || cap == 0) return;
    if (!peer || !peer->valid) {
        out[0] = '\0';
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s %u.%u.%u.%u:%u",
        peer->label,
        peer->target_ip[0], peer->target_ip[1], peer->target_ip[2], peer->target_ip[3], peer->target_port);
}

const char* GetConfigFilename() {
    return kConfigFilename;
}

} // namespace NetplayConfig

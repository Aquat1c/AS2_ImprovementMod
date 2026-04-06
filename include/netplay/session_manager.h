/**
 * Alice Senki 2 - Session Manager
 *
 * Real UDP session using ENet for transport.
 * Same public API shape (Mode, State, Snapshot, Start/Cancel/Disconnect/FrameUpdate).
 *
 * State machine:
 *   Idle -> Connecting -> Handshake -> Connected -> CharSel -> Error
 *
 * Host flow:
 *   1. Create ENet host on listen_port
 *   2. Wait for ENet CONNECT event from joiner
 *   3. Exchange AppHandshake (build hash + nickname + nonce)
 *   4. Enter Connected
 *
 * Join flow:
 *   1. Create ENet host on listen_port
 *   2. enet_host_connect to target
 *   3. Wait for ENet CONNECT event
 *   4. Exchange AppHandshake
 *   5. Enter Connected
 *
 * Connected:
 *   - ENet handles RTT measurement natively
 *   - Periodic StateDigest exchange during gameplay
 *   - Disconnect on timeout or user request
 */

#pragma once

#include "netplay_config.h"
#include "packet_codec.h"

#include <stdint.h>

namespace SessionManager {

// Same enums as NetplayMockSession for compatibility
enum class Mode : uint32_t {
    None = 0,
    Host,
    Join,
};

enum class State : uint32_t {
    Idle = 0,
    Connecting,
    Handshake,
    Connected,
    CharSel,
    Gameplay,
    Error,
};

struct Snapshot {
    bool     active;
    bool     has_error;
    Mode     mode;
    State    state;
    uint32_t revision;
    int      frames_remaining;
    char     peer_nickname[NetplayConfig::kNicknameCap];
    char     endpoint[48];
    char     status[128];
    char     last_error[128];
    // Extra fields not in mock
    float    rtt_ms;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t desync_count;
};

// Lifecycle
void Reset();
bool StartHost(const NetplayConfig::Config* config);
bool StartJoin(const NetplayConfig::Config* config);
void Cancel();
void Disconnect(const char* reason);

// Mode transitions
void EnterCharSel();
void EnterGameplay();
void ReturnToSession();

// Returns true (once) when the host has signaled this joiner to start CharSel.
// The menu controller polls this and auto-launches CharSel on the joiner side.
bool ConsumePendingCharSelStart();

// Must be called once per game frame from the menu controller
void FrameUpdate();

// Query
const char* GetModeName(Mode mode);
const char* GetStateName(State state);
bool GetSnapshot(Snapshot* out);

// Rollback session integration
// Buffer a received GekkoData payload for the rollback adapter to consume
void BufferGekkoPacket(const void* payload, int payloadLen);

// Drain buffered GekkoData payloads (called by rollback adapter)
struct BufferedPacket {
    uint8_t  data[1200];
    int      len;
};
int DrainGekkoPackets(BufferedPacket* out, int maxCount);

// Send a typed packet to the connected peer.
// reliable=true uses ENet reliable delivery (control/bootstrap/config).
// reliable=false sends unreliable (gameplay data, fire-and-forget).
bool SendToPeer(PacketCodec::PacketType type, const void* payload, size_t payloadSize, bool reliable);

bool IsHost();
uint64_t GetSessionId();
uint32_t GetConnectionId();

// Current smoothed RTT in milliseconds from ENet.
float GetRttMs();

// Lightweight socket poll — safe to call from timesync nudge wait.
// Drains pending packets without running full state-machine update.
void DriveSocketPoll();

} // namespace SessionManager

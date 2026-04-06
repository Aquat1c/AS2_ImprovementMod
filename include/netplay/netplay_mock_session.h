/**
 * Alice Senki 2 - Mock direct-play session driver
 *
 * Lightweight pre-transport state machine used to validate menu flow,
 * config commits, logging, and error surfaces before the real UDP session
 * manager is wired in.
 */

#pragma once

#include <stdint.h>

#include "netplay_config.h"

namespace NetplayMockSession {

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
    Error,
};

struct Snapshot {
    bool active;
    bool has_error;
    Mode mode;
    State state;
    uint32_t revision;
    int frames_remaining;
    char peer_nickname[NetplayConfig::kNicknameCap];
    char endpoint[48];
    char status[128];
    char last_error[128];
};

void Reset();
bool StartHost(const NetplayConfig::Config* config);
bool StartJoin(const NetplayConfig::Config* config);
void Cancel();
void Disconnect(const char* reason);
void EnterCharSel();
void ReturnToSession();
void FrameUpdate();
const char* GetModeName(Mode mode);
const char* GetStateName(State state);
bool GetSnapshot(Snapshot* out);

} // namespace NetplayMockSession

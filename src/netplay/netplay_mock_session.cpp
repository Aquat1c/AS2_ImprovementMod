#include "netplay_mock_session.h"

#include "log_window.h"
#include "netplay_config.h"
#include "netplay_hooks.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace NetplayMockSession {

namespace {

static Mode s_mode = Mode::None;
static State s_state = State::Idle;
static uint32_t s_revision = 0;
static int s_framesRemaining = 0;
static char s_peerNickname[NetplayConfig::kNicknameCap] = "";
static char s_endpoint[48] = "";
static char s_status[128] = "Mock session idle.";
static char s_lastError[128] = "";

static void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, cap, src, _TRUNCATE);
}

static bool IsZeroIp(const NetplayConfig::Config* config) {
    return config &&
        config->target_ip[0] == 0 &&
        config->target_ip[1] == 0 &&
        config->target_ip[2] == 0 &&
        config->target_ip[3] == 0;
}

static void SetState(State next, int framesRemaining, const char* fmt, ...) {
    char buffer[128];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);

    if (s_state != next || strcmp(s_status, buffer) != 0 || s_framesRemaining != framesRemaining) {
        LOG_NETPLAY(LOG_INFO, "[MockSession] %s -> %s | %s", GetStateName(s_state), GetStateName(next), buffer);
        s_state = next;
        s_framesRemaining = framesRemaining;
        CopyText(s_status, sizeof(s_status), buffer);
        ++s_revision;
    }
}

} // namespace

void Reset() {
    if (s_state != State::Idle || s_lastError[0] != '\0') {
        LOG_NETPLAY(LOG_INFO, "[MockSession] Resetting mock session state.");
    }
    s_mode = Mode::None;
    s_state = State::Idle;
    s_revision = 0;
    s_framesRemaining = 0;
    s_peerNickname[0] = '\0';
    s_endpoint[0] = '\0';
    s_lastError[0] = '\0';
    CopyText(s_status, sizeof(s_status), "Mock session idle.");
}

static bool StartInternal(const NetplayConfig::Config* config, Mode mode) {
    if (!config) return false;

    s_lastError[0] = '\0';
    if (!config->nickname[0]) {
        CopyText(s_lastError, sizeof(s_lastError), "Set a local nickname before connecting.");
        LOG_NETPLAY(LOG_WARNING, "[MockSession] Start rejected: missing nickname.");
        SetState(State::Error, 0, "%s", s_lastError);
        return false;
    }

    if (mode == Mode::Join && IsZeroIp(config)) {
        CopyText(s_lastError, sizeof(s_lastError), "Set a target IP before connecting.");
        LOG_NETPLAY(LOG_WARNING, "[MockSession] Start rejected: target IP is 0.0.0.0.");
        SetState(State::Error, 0, "%s", s_lastError);
        return false;
    }

    s_mode = mode;
    if (mode == Mode::Host) {
        _snprintf_s(s_endpoint, sizeof(s_endpoint), _TRUNCATE, "0.0.0.0:%u", config->listen_port);
        CopyText(s_peerNickname, sizeof(s_peerNickname), "Mock Joiner");
        SetState(State::Connecting, 36, "Hosting mock session on %s.", s_endpoint);
    } else {
        NetplayConfig::FormatEndpoint(config, s_endpoint, sizeof(s_endpoint));
        if (config->target_ip[0] == 127 && config->target_ip[1] == 0 && config->target_ip[2] == 0 && config->target_ip[3] == 1) {
            CopyText(s_peerNickname, sizeof(s_peerNickname), "Loopback Rival");
        } else {
            CopyText(s_peerNickname, sizeof(s_peerNickname), "Mock Peer");
        }
        SetState(State::Connecting, 30, "Dialing mock peer at %s.", s_endpoint);
    }
    return true;
}

bool StartHost(const NetplayConfig::Config* config) {
    return StartInternal(config, Mode::Host);
}

bool StartJoin(const NetplayConfig::Config* config) {
    return StartInternal(config, Mode::Join);
}

void Cancel() {
    if (s_state == State::Connecting || s_state == State::Handshake) {
        s_mode = Mode::None;
        SetState(State::Idle, 0, "Mock connect cancelled.");
    }
}

void Disconnect(const char* reason) {
    CopyText(s_lastError, sizeof(s_lastError), reason && reason[0] ? reason : "Mock session disconnected.");
    SetState(State::Error, 0, "%s", s_lastError);
}

void EnterCharSel() {
    if (s_state == State::Connected) {
        SetState(State::CharSel, 0, "Mock CharSel handoff ready for %s.", s_peerNickname[0] ? s_peerNickname : "peer");
    }
}

void ReturnToSession() {
    if (s_state == State::CharSel) {
        SetState(State::Connected, 0, "Mock session resumed with %s.", s_peerNickname[0] ? s_peerNickname : "peer");
    }
}

void FrameUpdate() {
    if (s_state == State::Connecting && s_framesRemaining > 0) {
        --s_framesRemaining;
        if (s_framesRemaining <= 0) {
            if (s_mode == Mode::Host) {
                SetState(State::Handshake, 22, "Mock joiner found. Comparing build %08X.", NetplayHooks::GetBuildSignature());
            } else {
                SetState(State::Handshake, 18, "Mock peer found. Comparing build %08X.", NetplayHooks::GetBuildSignature());
            }
        }
    } else if (s_state == State::Handshake && s_framesRemaining > 0) {
        --s_framesRemaining;
        if (s_framesRemaining <= 0) {
            if (s_mode == Mode::Host) {
                SetState(State::Connected, 0, "Mock host session ready with %s.", s_peerNickname[0] ? s_peerNickname : "peer");
            } else {
                SetState(State::Connected, 0, "Mock session ready with %s.", s_peerNickname[0] ? s_peerNickname : "peer");
            }
        }
    }
}

const char* GetModeName(Mode mode) {
    switch (mode) {
        case Mode::Host: return "Host";
        case Mode::Join: return "Join";
        default: return "None";
    }
}

const char* GetStateName(State state) {
    switch (state) {
        case State::Idle: return "Idle";
        case State::Connecting: return "Connecting";
        case State::Handshake: return "Handshake";
        case State::Connected: return "Connected";
        case State::CharSel: return "CharSel";
        case State::Error: return "Error";
        default: return "Unknown";
    }
}

bool GetSnapshot(Snapshot* out) {
    if (!out) return false;

    out->active = s_state != State::Idle && s_state != State::Error;
    out->has_error = s_state == State::Error;
    out->mode = s_mode;
    out->state = s_state;
    out->revision = s_revision;
    out->frames_remaining = s_framesRemaining;
    CopyText(out->peer_nickname, sizeof(out->peer_nickname), s_peerNickname);
    CopyText(out->endpoint, sizeof(out->endpoint), s_endpoint);
    CopyText(out->status, sizeof(out->status), s_status);
    CopyText(out->last_error, sizeof(out->last_error), s_lastError);
    return true;
}

} // namespace NetplayMockSession

/**
 * Alice Senki 2 - Win Screen Synchronization
 *
 * Syncs the A/C confirm gate at Mode 9 sub 3 between netplay peers.
 *
 * Source-of-truth: decompilation of sub_5FBD00 (Mode 9 handler).
 *   - Sub 3 checks: word_8E9EA2 || word_8E9EA6 || word_8E9F72 || word_8E9F76
 *     (P1 A just-pressed, P1 C just-pressed, P2 A just-pressed, P2 C just-pressed)
 *   - Any of these being non-zero advances past the confirm gate.
 *   - For VS Human (match_header+184 = 2 or 3), sub 0x24 exits via
 *     Game_ChangeMode(6, 1) → MODE_CHARSEL.
 *
 * Strategy:
 *   While active and in Mode 9 sub 3:
 *   1. Suppress all A/C just-pressed inputs (clear the 4 addresses)
 *   2. When local player presses A/C, record it and send WinScreenConfirm
 *   3. When remote sends WinScreenConfirm, record it
 *   4. When both confirmed: inject A/C into P1 just-pressed on next frame
 *   5. Game sees the input and advances through sub 3→8→36→CharSel
 */

#include "net/winscreen_sync.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "input/input_system.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using namespace Net;

// ============================================================================
// Game memory addresses for A/C just-pressed checks
// ============================================================================

// These are the exact addresses checked by the Mode 9 confirm gate
constexpr uintptr_t ADDR_P1_A_JUSTPRESSED = 0x8E9EA2;  // word_8E9EA2
constexpr uintptr_t ADDR_P1_C_JUSTPRESSED = 0x8E9EA6;  // word_8E9EA6
constexpr uintptr_t ADDR_P2_A_JUSTPRESSED = 0x8E9F72;  // word_8E9F72
constexpr uintptr_t ADDR_P2_C_JUSTPRESSED = 0x8E9F76;  // word_8E9F76

// Win screen confirm gate substate
constexpr uint32_t WINSCREEN_CONFIRM_SUB = 3;

// ============================================================================
// Internal state
// ============================================================================

static bool     s_initialized      = false;
static bool     s_active           = false;
static bool     s_localConfirmed   = false;
static bool     s_remoteConfirmed  = false;
static bool     s_injected         = false;  // Have we injected the final confirm?
static bool     s_confirmSent      = false;  // Have we sent our confirm packet?
static DWORD    s_beginTime        = 0;
static DWORD    s_lastResendTime   = 0;

// Resend interval for the confirm packet (in case of packet loss)
constexpr DWORD RESEND_INTERVAL_MS = 200;

// Timeout for win screen sync (if remote never confirms)
constexpr DWORD TIMEOUT_MS = 15000;

// ============================================================================
// Memory helpers
// ============================================================================

static uint16_t ReadU16(uintptr_t addr) {
    __try { return *(volatile uint16_t*)addr; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void WriteU16(uintptr_t addr, uint16_t val) {
    __try { *(volatile uint16_t*)addr = val; }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// ============================================================================
// Packet sending
// ============================================================================

static void SendConfirmPacket() {
    // WinScreenConfirm is a zero-payload reliable packet
    Session_SendPacket(CHANNEL_CONTROL, PacketType::WinScreenConfirm,
                       nullptr, 0, true);
}

// ============================================================================
// Input detection and suppression
// ============================================================================

/// Check if local player has pressed A or C this frame (from SDL input)
static bool LocalPlayerPressedConfirm() {
    const InputState_t* p1 = InputSystem_GetState(0);
    if (!p1) return false;
    return (p1->pressed & (INPUT_A | INPUT_C)) != 0;
}

/// Suppress all A/C just-pressed flags in game memory so the win screen
/// handler doesn't see them prematurely.
static void SuppressConfirmInputs() {
    WriteU16(ADDR_P1_A_JUSTPRESSED, 0);
    WriteU16(ADDR_P1_C_JUSTPRESSED, 0);
    WriteU16(ADDR_P2_A_JUSTPRESSED, 0);
    WriteU16(ADDR_P2_C_JUSTPRESSED, 0);
}

/// Inject A just-pressed for P1 so the game advances past the confirm gate.
static void InjectConfirmInput() {
    WriteU16(ADDR_P1_A_JUSTPRESSED, 1);
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void WinScreenSync_Init() {
    if (s_initialized) return;
    s_active = false;
    s_localConfirmed = false;
    s_remoteConfirmed = false;
    s_injected = false;
    s_confirmSent = false;
    s_initialized = true;
    LOG_NETPLAY(LOG_DEBUG, "[WinScreenSync] Initialized");
}

void WinScreenSync_Shutdown() {
    if (!s_initialized) return;
    s_active = false;
    s_initialized = false;
    LOG_NETPLAY(LOG_DEBUG, "[WinScreenSync] Shutdown");
}

void WinScreenSync_Begin() {
    if (!s_initialized) return;

    s_active = true;
    s_localConfirmed = false;
    s_remoteConfirmed = false;
    s_injected = false;
    s_confirmSent = false;
    s_beginTime = GetTickCount();
    s_lastResendTime = 0;

    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Begin — waiting for both peers to confirm");
}

void WinScreenSync_Abort() {
    if (!s_active) return;

    LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Abort (local=%s remote=%s)",
        s_localConfirmed ? "yes" : "no",
        s_remoteConfirmed ? "yes" : "no");

    s_active = false;
    s_localConfirmed = false;
    s_remoteConfirmed = false;
    s_injected = false;
    s_confirmSent = false;
}

bool WinScreenSync_FrameUpdate() {
    if (!s_active || !s_initialized) return false;

    uint32_t mode = GetGameMode();
    uint32_t sub  = GetSubstate();

    // Safety: if we're no longer in Mode 9, abort
    if (mode != MODE_WINSCREEN) {
        WinScreenSync_Abort();
        return false;
    }

    // Only active during the confirm gate substate
    if (sub != WINSCREEN_CONFIRM_SUB) {
        // Before or after the confirm gate — don't interfere
        if (s_injected) {
            // We already injected confirm, game is advancing through exit chain
            // Deactivate once past the confirm substate
            LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Confirm gate passed (sub=%u), deactivating", sub);
            s_active = false;
            return false;
        }
        return false;
    }

    // --- We are in Mode 9 Sub 3 (confirm gate) ---

    // Timeout check
    DWORD now = GetTickCount();
    if (now - s_beginTime > TIMEOUT_MS) {
        LOG_NETPLAY(LOG_WARNING, "[WinScreenSync] Timeout after %ums — forcing advance",
            now - s_beginTime);
        // Force advance on timeout — inject and let game proceed
        InjectConfirmInput();
        s_injected = true;
        s_active = false;
        return true;
    }

    // Step 1: Detect local confirm
    if (!s_localConfirmed) {
        if (LocalPlayerPressedConfirm()) {
            s_localConfirmed = true;
            s_confirmSent = false;  // Will send on this frame
            LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Local player confirmed");
        }
    }

    // Step 2: Send confirm packet (with resend for reliability)
    if (s_localConfirmed && Session_IsConnected()) {
        if (!s_confirmSent || (now - s_lastResendTime >= RESEND_INTERVAL_MS)) {
            SendConfirmPacket();
            s_confirmSent = true;
            s_lastResendTime = now;
        }
    }

    // Step 3: Suppress A/C inputs while waiting
    // This prevents the game from advancing before both peers agree
    SuppressConfirmInputs();

    // Step 4: Both confirmed — inject and advance
    if (s_localConfirmed && s_remoteConfirmed && !s_injected) {
        LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Both peers confirmed — injecting advance input");
        InjectConfirmInput();
        s_injected = true;
        return true;
    }

    return true;  // We are actively suppressing/managing input
}

bool WinScreenSync_IsActive() {
    return s_active;
}

bool WinScreenSync_LocalConfirmed() {
    return s_localConfirmed;
}

bool WinScreenSync_RemoteConfirmed() {
    return s_remoteConfirmed;
}

bool WinScreenSync_BothConfirmed() {
    return s_localConfirmed && s_remoteConfirmed;
}

void WinScreenSync_OnRemoteConfirm() {
    if (!s_active) {
        LOG_NETPLAY(LOG_WARNING, "[WinScreenSync] Remote confirm received but not active");
        return;
    }

    if (!s_remoteConfirmed) {
        s_remoteConfirmed = true;
        LOG_NETPLAY(LOG_INFO, "[WinScreenSync] Remote player confirmed (local=%s)",
            s_localConfirmed ? "yes" : "no");
    }
}

} // namespace Net

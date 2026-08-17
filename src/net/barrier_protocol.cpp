/**
 * Alice Senki 2 - Barrier Protocol Implementation
 *
 * Provides barrier state queries and helpers that aggregate information
 * from pregame_sync and match_bootstrap into a unified barrier view.
 *
 * This module does NOT own state — it queries the existing pregame_sync
 * and match_bootstrap modules and presents a simplified barrier-level view
 * for higher layers (menu controller, online wiring, diagnostics).
 */

#include "net/barrier_protocol.h"
#include "net/pregame_sync.h"
#include "net/session_manager.h"
#include "ui/log_window.h"

namespace Net {

// ============================================================================
// Barrier Phase Query
// ============================================================================

BarrierPhase BarrierProtocol_GetCurrentPhase() {
    PregameSnapshot pregame{};
    PregameSync_GetSnapshot(&pregame);

    if (!pregame.active) {
        return BarrierPhase::None;
    }

    switch (pregame.phase) {
        case PregamePhase::Idle:
            return BarrierPhase::None;

        case PregamePhase::SyncAnnounce:
        case PregamePhase::SyncExchange:
        case PregamePhase::SyncConfirmed:
            return BarrierPhase::SessionSync;

        case PregamePhase::FrontendCharSel:
        case PregamePhase::FrontendStageSel:
        case PregamePhase::FrontendLocked:
            return BarrierPhase::FrontendLockstep;

        case PregamePhase::ConfigExchange:
        case PregamePhase::ConfigAgreed:
            return BarrierPhase::ConfigAgreement;

        case PregamePhase::BootstrapLoading:
            return BarrierPhase::LoadWait;

        case PregamePhase::BootstrapBaseline:
        case PregamePhase::BootstrapReady:
            return BarrierPhase::BaselineAgreement;

        case PregamePhase::GameplayHandoff:
            return BarrierPhase::GameplayReady;

        case PregamePhase::Error:
            return BarrierPhase::None;

        default:
            return BarrierPhase::None;
    }
}

bool BarrierProtocol_IsBarrierActive() {
    return BarrierProtocol_GetCurrentPhase() != BarrierPhase::None;
}

bool BarrierProtocol_IsGameplayReady() {
    return BarrierProtocol_GetCurrentPhase() == BarrierPhase::GameplayReady;
}

// ============================================================================
// Send Helper
// ============================================================================

bool BarrierProtocol_SendPacket(PacketType type, const void* payload, size_t len) {
    bool reliable = BarrierProtocol_IsReliable(type);
    uint8_t channel = BarrierProtocol_GetChannel(type);
    return Session_SendPacket(channel, type, payload, len, reliable);
}

} // namespace Net

/**
 * Alice Senki 2 - Match Bootstrap
 *
 * Handles the post-CharSel/StageSel bootstrap sequence:
 *   1. Config exchange — host sends LockedMatchConfig, join validates
 *   2. Load barrier — both peers wait for asset loading to complete
 *   3. Baseline capture — both capture pre-frame-0 savestate
 *   4. Baseline agreement — exchange CRC32 checksums to verify determinism
 *   5. Gameplay start — coordinated signal to begin gameplay
 */

#pragma once

#include "net/locked_match_config.h"
#include "net/protocol.h"
#include <stdint.h>

namespace Net {

// ============================================================================
// Bootstrap Snapshot
// ============================================================================

struct MatchBootstrapSnapshot {
    bool     active;

    // Config exchange
    bool     config_sent;
    bool     config_received;
    bool     config_agreed;

    // Load barrier
    bool     local_loaded;
    bool     remote_loaded;
    bool     both_loaded;

    // Baseline
    bool     local_baseline_ready;
    bool     remote_baseline_ready;
    bool     baseline_agreed;
    uint32_t local_baseline_crc;
    uint32_t remote_baseline_crc;

    // Gameplay start
    bool     gameplay_start;
    uint32_t start_frame;

    // Error
    char     error[128];
};

// ============================================================================
// Lifecycle
// ============================================================================

void MatchBootstrap_Init();
void MatchBootstrap_Shutdown();

// ============================================================================
// Control
// ============================================================================

/// Begin config exchange phase. Host sends config, join validates.
void MatchBootstrap_BeginConfigExchange(const LockedMatchConfig* config);

/// Begin loading phase. Monitors game substate for load completion.
void MatchBootstrap_BeginLoading();

/// Begin baseline capture + agreement phase.
void MatchBootstrap_BeginBaseline();

/// Abort the bootstrap.
void MatchBootstrap_Abort();

// ============================================================================
// Per-Frame
// ============================================================================

/// Drive the bootstrap state machine.
void MatchBootstrap_FrameUpdate();

// ============================================================================
// Packet Reception (called by PregameSync packet handler)
// ============================================================================

void MatchBootstrap_OnConfigExchange(const ConfigExchangePayload* p);
void MatchBootstrap_OnConfigAck(const ConfigAckPayload* p);
void MatchBootstrap_OnLoadBarrier(const LoadBarrierPayload* p);
void MatchBootstrap_OnBaselineReady(const BaselineReadyPayload* p);
void MatchBootstrap_OnBaselineDigest(const BaselineDigestPayload* p);
void MatchBootstrap_OnGameplayStart(const GameplayStartPayload* p);

// ============================================================================
// Queries
// ============================================================================

void MatchBootstrap_GetSnapshot(MatchBootstrapSnapshot* out);

/// Get the agreed config (valid after config_agreed).
const LockedMatchConfig* MatchBootstrap_GetAgreedConfig();

} // namespace Net

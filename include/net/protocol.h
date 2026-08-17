/**
 * Alice Senki 2 - Network Protocol Definitions
 *
 * Packet types, wire header, version constants, and channel assignments
 * for the mod-owned ENet transport layer.
 *
 * All multi-byte fields are little-endian (x86 native).
 * ENet packets carry: [PacketType (2 bytes) | payload]
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Protocol Constants
// ============================================================================

// v2 wire protocol (re0.7 M1). 18 = last 0.6/0.7 field build; 19 deliberately
// skipped so interim re0.7 dev builds can never pair with field builds.
constexpr uint16_t PROTOCOL_VERSION = 20;
constexpr int      MAX_PACKET_SIZE  = 1200;     // Stay under typical MTU
constexpr int      MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - 2;  // minus PacketType
constexpr int      NETPLAY_PALETTE_BANK_COUNT = 12;
constexpr int      NETPLAY_PALETTE_BANK_SIZE = 1024;
constexpr int      NETPLAY_PALETTE_MAX_PAYLOAD = NETPLAY_PALETTE_BANK_SIZE;

// ============================================================================
// ENet Channel Assignments
// ============================================================================

constexpr uint8_t CHANNEL_CONTROL   = 0;  // Reliable ordered: handshake, session control
constexpr uint8_t CHANNEL_GAMEPLAY  = 1;  // Unreliable sequenced: gameplay input (future)
constexpr uint8_t CHANNEL_DEBUG     = 2;  // Unreliable: diagnostics, state digests
constexpr uint8_t NUM_CHANNELS      = 3;

// ============================================================================
// Packet Types
// ============================================================================

// Retired at v20 (deleted outright — no send/receive site remained):
//   SessionMeta (5), GameplayInput (20), WinScreenConfirm (41).
// Retired at M3 with the session2 cutover: Hello (1) / HelloAck (2) — the
// legacy handshake is superseded by the 5-step nonce exchange (70–74); the
// side data they carried (nickname/round/timing/HUD style) now rides
// PeerIdentity (79) after the handshake completes.
// Retired-by-plan but still live in the old backend until the M5 cutover
// (marked LEGACY below): DelayChangeReq/Ack (INV-23: knobs become
// peer-local), GekkoReady (superseded by TransitionBarrier GameplayStart),
// SyncAnnounce/SyncConfirm delay-negotiation fields.
enum class PacketType : uint16_t {
    // Session control (reliable, channel 0)
    Ready           = 3,    // Peer is ready for next phase
    Disconnect      = 4,    // Graceful disconnect with reason

    // Initial session sync (reliable, channel 0)
    SyncAnnounce    = 6,    // Session metadata announce for pre-game agreement
    SyncConfirm     = 7,    // Session sync confirmation / side assignment
    FrontendPhaseBarrier = 8,  // Frontend phase barrier / transition ack
    FrontendBoundaryDigest = 9, // Frontend boundary digest / watchdog state

    // NAT traversal coordination (reliable, channel 0)
    NatInfo         = 10,   // Exchange NAT/UPnP status
    NatTraversalSignal = 34, // Exchange ICE description/candidates over session control

    // Pre-game sync (reliable, channel 0)
    CharSelInput    = 11,   // CharSel custom-palette availability catalog exchange
    CharSelLock     = 12,   // Scoped character-selection lock notification
    StageSync       = 13,   // Stage selection exchange/lock
    ConfigExchange  = 14,   // LockedMatchConfig proposed by host
    ConfigAck       = 15,   // Join acknowledges config (includes config hash)
    LoadBarrier     = 16,   // Peer finished loading assets
    BaselineReady   = 17,   // Peer captured baseline savestate
    BaselineDigest  = 18,   // Normalized bootstrap agreement digest
    GameplayStart   = 19,   // Both peers ready — begin gameplay
    BaselineBreakdown = 25, // Detailed per-region baseline CRCs for mismatch diagnosis

    // CharSel lockstep (unreliable, channel 1)
    CharSelFrameInput = 40, // Per-frame charsel input with redundancy

    // Win screen / pause
    PauseQuit           = 42,  // Pause menu quit signal
    WinScreenFrameInput = 43,  // Mode 9 lockstep frame input with redundancy

    // Palette metadata control-plane (reliable, channel 0)
    PaletteConfig      = 50,
    PaletteData        = 51,
    PaletteAck         = 52,

    // Gameplay input stream (unreliable-sequenced, channel 1).
    // v20: replaces GekkoData under the same id. Until the engine2 cutover
    // (M5) the old backend still transports raw GekkoNet bytes under this id;
    // the v2 InputStreamPayload below is the target schema (defined, unsent).
    InputStream     = 23,

    // Startup gameplay-entry barrier (reliable, channel 0)
    // LEGACY — superseded by TransitionBarrier GameplayStart at the M5 cutover.
    GekkoReady      = 24,   // Startup barrier control (ready/ack)

    // Frontend shared-delay coordination
    // LEGACY — retired with the delay-negotiation flow (INV-23) at cutover.
    DelayChangeReq  = 21,
    DelayChangeAck  = 22,

    // Debug / diagnostics (unreliable, channel 2)
    Ping            = 30,   // Application-level ping (supplements ENet RTT)
    Pong            = 31,   // Application-level pong
    StateDigest     = 32,   // CRC32 state digest for desync detection
    FrameSyncStatus = 33,   // Lightweight frame-progress telemetry
    SyncTrace       = 35,   // Synchronized diagnostics trace, debug channel
    ChurnPause      = 36,   // Peer-visible device I/O pause hint (debug channel)

    // Wire-acknowledged phase transitions (reliable, channel 0) — 0.7 rework.
    // Cross-peer state transitions at match boundaries commit only after a
    // proposal/ack round trip; stale seqs are re-acked idempotently.
    PhaseTransitionProposal = 60,
    PhaseTransitionAck      = 61,
    ResyncRequest           = 62,  // Frontend starvation interrogation (INV-11)
    ResyncReply             = 63,  // Responder's identity tuple

    // v2 session handshake (reliable, channel 0) — 5-step nonce exchange
    // (master plan §4.2). Defined at M1; sent by session2 from M3.
    SessionHello      = 70,  // step 1 (client, 200 ms resend)
    SessionOffer      = 71,  // step 2 (host, echoes Hello verbatim — INV-13)
    SessionAck        = 72,  // step 3 (client, echoes Offer verbatim)
    SessionConfirm    = 73,  // step 4 (host, session_id)
    SessionConfirmAck = 74,  // step 5 (client, session_id) → Connected

    // v2 gameplay verification / timing (defined at M1; sent from M4+)
    SyncHash        = 75,   // reliable ch0: periodic confirmed-frame state hash
    SyncHashAck     = 76,   // reliable ch0: bounds sender's outstanding window
    TimeProbe       = 77,   // unreliable ch1: 4 Hz µs RTT probe
    TimeProbeAck    = 78,   // unreliable ch1: echo with responder dwell

    // v2 post-handshake identity exchange (reliable, channel 0) — M3.
    // The v2 handshake payloads (70–74) are wire-frozen and carry only the
    // fail-closed identity (version/build/cadence/nonces/short nickname);
    // the preserved PeerInfo contract (full nickname, round option, frame
    // timing, HUD style — inventory §2.1) is filled by this one-shot packet
    // sent by both sides on entering Connected.
    PeerIdentity    = 79,
};

enum class FrameTimingMode : uint8_t {
    Vanilla58_8 = 0,  // Native 17ms limiter, about 58.8235 fps
    Proper60    = 1,  // 17ms limiter corrected to 1000/60 cadence
};

inline bool FrameTimingMode_IsValid(uint8_t value) {
    return value == (uint8_t)FrameTimingMode::Vanilla58_8 ||
           value == (uint8_t)FrameTimingMode::Proper60;
}

inline const char* FrameTimingModeName(FrameTimingMode mode) {
    switch (mode) {
        case FrameTimingMode::Vanilla58_8: return "vanilla_58_8";
        case FrameTimingMode::Proper60:    return "proper_60";
        default:                           return "unknown";
    }
}

inline const char* FrameTimingModeDisplayName(FrameTimingMode mode) {
    switch (mode) {
        case FrameTimingMode::Vanilla58_8: return "58.8 FPS";
        case FrameTimingMode::Proper60:    return "60.0 FPS";
        default:                           return "Unknown";
    }
}

// ============================================================================
// Frontend phase identity (v2, §3.4)
// ============================================================================

// Fixed enum, identical on both builds by construction — replaces the per-side
// runtime `phase_serial` allocator as the acceptance key (INV-7). The continue
// prompt rides WinScreen's stream under id 3 per the shipped continue_flow
// design. The legacy phase_serial fields remain populated until the frontend
// acceptance-rule cutover (M5) so the old backend keeps running unchanged.
enum class FrontendPhaseId : uint8_t {
    None      = 0,
    CharSel   = 1,
    StageSel  = 2,
    WinScreen = 3,
};

inline const char* FrontendPhaseIdName(FrontendPhaseId id) {
    switch (id) {
        case FrontendPhaseId::None:      return "None";
        case FrontendPhaseId::CharSel:   return "CharSel";
        case FrontendPhaseId::StageSel:  return "StageSel";
        case FrontendPhaseId::WinScreen: return "WinScreen";
        default:                         return "Unknown";
    }
}

// ============================================================================
// Wire Payloads
// ============================================================================

#pragma pack(push, 1)

// One-shot identity exchange sent by both sides after the v2 handshake
// reaches Connected (PeerIdentity, 79). Carries the side data the legacy
// Hello/HelloAck used to piggyback so the preserved PeerInfo/HUD-style/round
// contracts keep working; the fail-closed fields (version/build/cadence)
// were already verified by the handshake and are NOT repeated here.
struct PeerIdentityPayload {
    char     nickname[64];       // Null-terminated UTF-8 nickname (full length)
    uint16_t listen_port;        // Port this peer is listening on
    uint8_t  round_count;        // Sender's current vanilla round option 0..2
    uint8_t  frame_timing_mode;  // Net::FrameTimingMode (handshake-verified equal)
    uint8_t  hud_style_valid;    // 1 = HUD style fields are meaningful
    uint8_t  hud_trail_r;
    uint8_t  hud_trail_g;
    uint8_t  hud_trail_b;
    uint8_t  hud_text_r;
    uint8_t  hud_text_g;
    uint8_t  hud_text_b;
    uint8_t  hud_trail_length;   // Encoded bar extend (64-320 px)
    uint8_t  hud_score_r;
    uint8_t  hud_score_g;
    uint8_t  hud_score_b;
    uint8_t  hud_font_size;      // NetplayHudStyle::HudFontSize
    uint8_t  hud_vertical_position; // NetplayHudStyle::HudVerticalPosition
    uint8_t  _pad[3];
};

// Enriched at M4 to the §3.2 terminal shape (INV-20): every terminal is
// self-describing on the wire — a coarse machine code (DisconnectReason,
// session_types.h), a stable 32-bit hash of the typed reason name
// (fnv1a32, machine-groupable across builds), and a bounded human string.
// Fault terminals are sticky: session2 resends every 100 ms during the
// goodbye window until the transport acks (reliable ch0) or the window ends.
struct DisconnectPayload {
    uint8_t  code;               // Net::DisconnectReason (coarse class)
    uint8_t  _pad;
    uint32_t reason_id;          // fnv1a32 of the typed terminal reason name
    char     human[96];          // Human-readable reason (null-terminated)
};

struct PingPayload {
    uint32_t ping_id;            // Echoed back in Pong
    uint32_t send_time_ms;       // Local timestamp (GetTickCount)
};

struct PongPayload {
    uint32_t ping_id;
    uint32_t original_send_time_ms;
    uint32_t responder_time_ms;
};

struct StateDigestPayload {
    uint32_t frame_number;
    uint32_t crc32;
};

// ============================================================================
// Wire-acknowledged phase transitions (0.7 rework, M4)
// ============================================================================

enum class NetTransitionKind : uint8_t {
    None             = 0,
    WinScreenExit    = 1,  // both sides release winscreen lockstep together
    PostMatchDecision= 2,  // intent carries the post-match route
    RematchStart     = 3,  // begin pregame sync for the next match
    GameplayStart    = 4,  // enter the match handoff
    SessionCancel    = 5,  // graceful teardown with reason
    EpochAlign       = 6,  // v2 (§4.5): adopt {epoch, first_phase, native_mode}
};

enum class PostMatchIntentWire : uint8_t {
    None             = 0,
    Rematch          = 1,
    ReturnToSession  = 2,
    Disconnect       = 3,
};

struct PhaseTransitionPayload {
    uint32_t transition_seq;  // monotonic per session, minted by the proposer
    uint8_t  kind;            // NetTransitionKind
    uint8_t  intent;          // PostMatchIntentWire for PostMatchDecision, else 0
    uint16_t _pad;
    uint32_t session_id;      // pregame session id context (0 if none)
    // v2 EpochAlign fields (§3.2/§4.5); zero for every other kind. Defined at
    // M1 (compile-only); populated once match_setup mints epochs (M5).
    uint32_t epoch;           // epoch being adopted (host-minted, u32, never 0)
    uint8_t  first_phase;     // FrontendPhaseId of the epoch's first phase
    uint8_t  native_mode;     // sender's native MODE_* at proposal time
    uint16_t _pad2;
};

inline const char* NetTransitionKindName(NetTransitionKind kind) {
    switch (kind) {
        case NetTransitionKind::None:              return "None";
        case NetTransitionKind::WinScreenExit:     return "WinScreenExit";
        case NetTransitionKind::PostMatchDecision: return "PostMatchDecision";
        case NetTransitionKind::RematchStart:      return "RematchStart";
        case NetTransitionKind::GameplayStart:     return "GameplayStart";
        case NetTransitionKind::SessionCancel:     return "SessionCancel";
        case NetTransitionKind::EpochAlign:        return "EpochAlign";
    }
    return "?";
}

struct FrameSyncStatusPayload {
    int32_t  rb_frame_current;              // Sender's current rollback-session-relative frame
    int32_t  game_abs_frame_current;        // Sender's current absolute engine frame
    int32_t  frame_origin_abs;              // Sender's absolute rollback origin
    int32_t  rb_frame_last_remote_received; // Sender's latest remote progress in rb_frame domain
    int32_t  rb_frame_confirmed;            // Sender's fully confirmed rollback frame
    int32_t  predicted_frames;              // Sender's outstanding predicted frames
    uint32_t checksum;                      // Sender's current state checksum
};

struct ChurnPausePayload {
    uint8_t  flags;                         // CHURN_PAUSE_FLAG_*
    uint8_t  reason;                          // Net::ChurnPauseReason
    uint16_t session_epoch;                   // Matches sender ChurnPause session epoch
    int32_t  rb_frame;                        // Sender rollback frame when pausing
    uint32_t sender_ms;                       // GetTickCount() at send time
    uint32_t grace_ms;                        // Suggested hold duration for peer
};

constexpr uint8_t CHURN_PAUSE_FLAG_ACTIVE = 1 << 0;
constexpr uint8_t CHURN_PAUSE_FLAG_CLEAR  = 1 << 1;

enum class SyncTraceDomain : uint8_t {
    None             = 0,
    FrontendLockstep = 1,
    GameplayRollback = 2,
    LifecycleEvent   = 3,
};

constexpr uint16_t SYNC_TRACE_FLAG_VALID_STATE_CRC  = 1 << 0;
constexpr uint16_t SYNC_TRACE_FLAG_VALID_PLAYER_CRC = 1 << 1;
constexpr uint16_t SYNC_TRACE_FLAG_ROLLBACK_SETTLED = 1 << 2;
constexpr uint16_t SYNC_TRACE_FLAG_ROLLING_BACK     = 1 << 3;
constexpr uint16_t SYNC_TRACE_FLAG_PASSIVE_PHASE    = 1 << 4;

struct SyncTracePayload {
    uint16_t schema;             // start at 1
    uint16_t flags;              // SYNC_TRACE_FLAG_*

    uint32_t epoch_id;           // frontend epoch, match epoch, or session token
    uint32_t seq;                // local monotonic trace sequence

    uint8_t  domain;             // SyncTraceDomain
    uint8_t  sync_mode;          // Net::SyncMode
    uint8_t  lockstep_context;   // Net::LockstepContext
    uint8_t  frontend_phase;     // Net::FrontendSyncPhase

    uint8_t  match_lifecycle_phase; // Net::MatchLifecyclePhase
    uint8_t  rollback_phase;         // Net::MatchRollbackPhase
    uint8_t  native_mode;            // MODE_*
    uint8_t  native_substate;        // *_SUB_*

    uint8_t  game_type;
    uint8_t  local_player;       // 0=P1, 1=P2, 0xFF unknown
    uint8_t  remote_player;      // 0=P1, 1=P2, 0xFF unknown
    uint8_t  _pad0;

    int32_t  game_abs_frame;     // absolute engine/sim frame if known, else -1
    int32_t  rb_frame;           // rollback frame for gameplay, else -1
    uint32_t frontend_frame;     // lockstep consume frame, else 0

    uint16_t local_input;        // normalized local input for compared frame
    uint16_t remote_input;       // normalized remote input for compared frame

    uint32_t local_input_frame;  // frontend local head or rb input head
    uint32_t remote_latest_frame;// frontend remote latest or rb remote received
    uint32_t consume_frame;      // frontend consume frame or rb current frame
    uint32_t remote_ack_frame;   // frontend ack or rb confirmed frame

    uint8_t  local_visible_delay;
    uint8_t  remote_visible_delay;
    uint8_t  local_effective_delay;
    uint8_t  remote_effective_delay;

    uint8_t  rollback_budget;
    uint8_t  predicted_frames;
    uint8_t  last_rollback_depth;
    uint8_t  max_rollback_depth;

    uint32_t state_crc;          // normalized phase-specific CRC
    uint32_t p1_crc;             // optional; 0 if not captured
    uint32_t p2_crc;             // optional; 0 if not captured
    uint32_t rng_seed;           // 0 if unavailable
};

// Initial session sync payloads

struct SyncAnnouncePayload {
    uint32_t session_id;         // Random session identifier for this match
    uint8_t  capability_flags;   // Bit 0: savestate baseline, Bit 1: desync diagnostics
    uint8_t  frontend_delay_proposal; // Local frontend delay recommendation for this epoch
    uint8_t  round_count;        // Host-owned vanilla option 0..2 when sent by host
    uint8_t  _pad;
};

struct SyncConfirmPayload {
    uint32_t session_id;         // Agreed session ID (host's ID is authoritative)
    uint8_t  confirmed;          // 1 = all checks passed
    uint8_t  assigned_side;      // Host decides: 0 = host is P1, 1 = host is P2
    uint8_t  frontend_delay_proposal; // Echo of sender's local recommendation
    uint8_t  frontend_shared_delay;   // Shared frontend delay agreed for this epoch
    uint8_t  round_count;        // Host-owned vanilla option 0..2 when sent by host
    uint8_t  _pad[3];
};

struct FrontendPhaseBarrierPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (current phase)
    uint16_t next_phase;         // Net::FrontendSyncPhase (next phase)
    uint32_t last_completed_frame; // Sender's final frame index for the completed phase
    uint8_t  reason_code;        // Barrier reason / transition category
    uint8_t  phase_id;           // v2: FrontendPhaseId (0 until M5 cutover)
    uint8_t  _pad[2];
};

struct FrontendBoundaryDigestPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase
    uint8_t  digest_kind;        // Net::FrontendDigestKind
    uint8_t  substate;           // Native substate for diagnostics
    uint32_t frame;              // Local phase frame when digest was captured
    uint32_t digest;             // CRC32 over the phase boundary state
    uint8_t  p1_character;
    uint8_t  p1_palette;
    uint8_t  p2_character;
    uint8_t  p2_palette;
    uint8_t  p1_palette_custom;
    uint8_t  p2_palette_custom;
    uint8_t  phase_id;           // v2: FrontendPhaseId (0 until M5 cutover)
    uint8_t  _palette_pad;
    uint8_t  stage_cursor;
    uint8_t  stage_confirmed;
    uint8_t  stage_counter;
    uint8_t  stage_aux;
    uint8_t  stage_cancel;
    uint8_t  confirm_menu_cursor;
    uint8_t  confirm_menu_action;
    uint8_t  committed_stage_id;
};

struct NatInfoPayload {
    uint8_t  flags;              // NAT_INFO_FLAG_*
    uint8_t  upnp_status;        // Net::NatStatus
    uint8_t  stun_status;        // Net::StunStatus
    uint8_t  extra_flags;        // NAT_INFO_EX_FLAG_*
    uint16_t listen_port;        // Local listen port used by ENet host
    uint16_t external_port;      // External/stun-discovered UDP port
    char     external_ip[48];    // UPnP/STUN external IP string (v4/v6 text)
};

struct NatTraversalSignalPayload {
    uint8_t  signal_type;        // Net::NatSignalType
    uint8_t  _pad;
    uint16_t text_len;           // bytes used in text[]
    char     text[1024];         // ICE SDP/candidate payload (trickle)
};

// Pre-game sync payloads

struct CharSelInputPayload {
    uint8_t  game_slot;          // 0 = P1, 1 = P2
    uint8_t  _pad;
    uint16_t custom_masks[256];  // Bit N set => stored custom bank exists for base palette N
};

struct CharSelLockPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (must be CharSel)
    uint8_t  character_id;       // Resolved character ID from grid table
    uint8_t  palette;            // Final palette
    uint8_t  flags;              // CHARSEL_LOCK_FLAG_*
    uint8_t  _pad[3];
};

struct StageSyncPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (must be StageSel)
    uint16_t frame;              // Sender's current phase frame
    uint8_t  stage_id;           // Sender's currently resolved stage ID
    uint8_t  confirmed;          // 0 = browsing, 1 = locked
    uint8_t  stage_cursor;       // Shared cursor/grid index
    uint8_t  stage_counter;      // Stage confirm/roulette counter byte
    uint8_t  stage_aux;          // Adjacent stage state byte (watchdog only)
    uint8_t  stage_cancel;       // Cancel flag
    uint8_t  confirm_menu_cursor; // Confirm-menu cursor index
    uint8_t  confirm_menu_action; // Confirm-menu action/armed flag
    uint8_t  committed_stage_id; // Final committed stage ID if available
    uint8_t  substate;           // Native charsel substate for diagnostics
    uint8_t  round_count;        // Host-owned vanilla option 0..2 (wins required = value + 1)
    uint8_t  _pad;
};

struct ConfigExchangePayload {
    uint8_t  p1_character;
    uint8_t  p1_palette;
    uint8_t  p2_character;
    uint8_t  p2_palette;
    uint8_t  stage_id;
    uint8_t  host_side;          // 0 = host is P1, 1 = host is P2
    uint8_t  round_count;        // Vanilla option 0..2 (wins required = value + 1)
    uint8_t  time_limit;
    uint32_t rng_seed;
    uint32_t session_seed;
    // Local rollback configuration announcement
    uint8_t  my_input_delay;     // Sender's local gameplay input delay
    uint8_t  my_max_rollback;    // Sender's input prediction window / max rollback
    uint8_t  gameplay_delay_mode; // Net::GameplayDelayMode
    uint8_t  frame_timing_mode;  // Host-authoritative Net::FrameTimingMode
};

struct ConfigAckPayload {
    uint32_t config_hash;        // CRC32 of the LockedMatchConfig peer built
    uint8_t  accepted;           // 1 = matches, 0 = mismatch
    // Local rollback configuration announcement
    uint8_t  my_input_delay;     // Sender's local gameplay input delay
    uint8_t  my_max_rollback;    // Sender's input prediction window / max rollback
    uint8_t  gameplay_delay_mode; // Net::GameplayDelayMode
    uint8_t  frame_timing_mode;  // Sender's effective Net::FrameTimingMode
};

struct LoadBarrierPayload {
    uint8_t  loaded;             // 1 = assets loaded
    uint8_t  mode;               // Local game mode when barrier was sent
    uint8_t  substate;           // Local substate when barrier was sent
    uint8_t  _pad;
    uint32_t sim_frame;          // Native sim frame at load completion
};

struct BaselineReadyPayload {
    uint8_t  captured;           // 1 = baseline savestate captured
    uint8_t  mode;               // Local game mode when baseline was captured
    uint8_t  substate;           // Local substate when baseline was captured
    uint8_t  _pad;
    uint32_t sim_frame;          // Native sim frame when baseline was captured
};

struct BaselineDigestPayload {
    uint32_t crc32;              // Normalized bootstrap agreement digest
};

struct BaselineBreakdownPayload {
    // Baseline CRC components
    uint32_t main_crc;
    uint32_t header_crc;
    uint32_t context_crc;
    uint32_t effect_crc;
    uint32_t summon_crc;
    uint32_t p1_entity_crc;
    uint32_t p2_entity_crc;

    // Normalized bootstrap-agreement CRCs. These intentionally exclude
    // volatile loader/handle regions that are useful for diagnostics but
    // not authoritative for baseline agreement.
    uint32_t header_agreement_crc;
    uint32_t p1_agreement_crc;
    uint32_t p2_agreement_crc;

    // Adjacent/non-hash diagnostics
    uint32_t pre_match_gap_crc;
    uint32_t p1_input_crc;
    uint32_t p2_input_crc;
    uint32_t per_frame_temp_crc;

    // Contextual counters
    uint32_t rng_seed;
    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t game_mode;
    uint32_t substate;
    uint32_t game_type;
    uint32_t match_phase_timer;
    uint32_t frame_simulation;
    uint32_t frame_display;
    uint32_t frame_write_idx;
    uint32_t frame_net_idx;
    uint32_t remote_frame_idx;

    // Raw critical bytes
    uint8_t  match_header_bytes[16];
    uint8_t  pre_match_gap_bytes[12];
};

struct GameplayStartPayload {
    uint32_t bootstrap_frame_abs;   // Absolute engine frame of the restored bootstrap baseline
    uint32_t host_game_abs_frame;   // Host absolute engine frame when GameplayStart was sent
};

struct GekkoReadyPayload {
    uint8_t  flags;              // GEKKO_READY_FLAG_*
    uint8_t  phase;              // Sender MatchLifecyclePhase at send time
    uint16_t _pad;
    int32_t  game_abs_frame;     // Sender absolute engine frame at startup barrier send time
};

struct CharSelFrameInputPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch (retired at M5)
    uint16_t phase;              // Net::FrontendSyncPhase (CharSel or StageSel)
    uint8_t  phase_id;           // v2: FrontendPhaseId (0 until M5 cutover)
    uint8_t  _phase_pad;
    uint32_t frame;              // Lockstep frame number
    uint32_t ack_frame;          // Sender's consumeFrame (frame they need from us)
    uint16_t inputs[16];         // Redundant history: [frame, frame-1, ..., frame-15]
    uint16_t input_count;        // Number of valid entries in inputs[] (1-16)
    uint16_t _pad;
};

struct WinScreenFrameInputPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch (retired at M5)
    uint16_t phase;              // Net::FrontendSyncPhase (WinScreen)
    uint8_t  phase_id;           // v2: FrontendPhaseId (0 until M5 cutover)
    uint8_t  _phase_pad;
    uint32_t frame;              // Lockstep frame number
    uint32_t ack_frame;          // Sender's consumeFrame (frame they need from us)
    uint16_t inputs[16];         // Redundant history: [frame, frame-1, ..., frame-15]
    uint16_t input_count;        // Number of valid entries in inputs[] (1-16)
    uint16_t _pad;
};

struct PaletteConfigPayload {
    uint32_t epoch;
    uint32_t config_hash;
    uint8_t  game_slot;          // 0 = P1, 1 = P2
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  flags;
    uint16_t payload_size;
    uint16_t _pad;
    uint32_t payload_crc;
};

struct PaletteDataPayload {
    uint32_t epoch;
    uint32_t config_hash;
    uint8_t  game_slot;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  _pad0;
    uint32_t payload_crc;
    uint16_t payload_size;
    uint16_t _pad1;
    uint8_t  payload[NETPLAY_PALETTE_BANK_SIZE];
};

struct PaletteAckPayload {
    uint32_t epoch;
    uint32_t config_hash;
    uint8_t  game_slot;
    uint8_t  accepted;
    uint8_t  received_data;
    uint8_t  _pad;
    uint32_t payload_crc;
};

struct DelayChangeReqPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase
    uint16_t new_delay;          // Requested shared frontend delay
    uint32_t apply_from_frame;   // Apply point inside the phase timeline
    uint8_t  reason_code;        // Net::FrontendDelayBumpReason
    uint8_t  _pad[3];
};

struct DelayChangeAckPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase
    uint16_t acked_delay;        // Accepted shared frontend delay
    uint32_t apply_from_frame;   // Apply point inside the phase timeline
    uint8_t  accepted;           // 1 = accepted, 0 = rejected
    uint8_t  reason_code;        // Net::FrontendDelayBumpReason
    uint8_t  _pad[2];
};

// ============================================================================
// v2 wire payloads (master plan §3.2) — defined at M1, unsent until M3+.
// ============================================================================

// --- 5-step nonce handshake (§4.2) ---

struct SessionHelloPayload {
    uint16_t proto_ver;          // must equal PROTOCOL_VERSION (fail-closed)
    uint32_t build_hash;         // exact build fingerprint (fail-closed)
    uint16_t cadence_num;        // §2.8.2 cadence profile rational, numerator
    uint16_t cadence_den;        // §2.8.2 cadence profile rational, denominator
    uint64_t client_nonce;       // fresh nonzero per attempt (replay inertness)
    char     nickname[16];       // null-terminated UTF-8
};

// Echo-verbatim (INV-13): confirm packets are built from the received BYTES,
// never recomputed — the `shared=2` class of bug is structurally impossible.
struct SessionOfferPayload {
    SessionHelloPayload hello_echo;  // every Hello field, byte-exact
    uint64_t host_nonce;             // fresh nonzero
    uint32_t host_seed;
    char     host_nickname[16];
};

struct SessionAckPayload {
    SessionOfferPayload offer_echo;  // every Offer field, byte-exact
};

// session_id = fnv1a64(client_nonce || host_nonce || host_seed); equality of
// the two directions is the final echo check.
struct SessionConfirmPayload {
    uint64_t session_id;
};

struct SessionConfirmAckPayload {
    uint64_t session_id;
};

// --- Gameplay input stream (replaces GekkoData semantics at the M5 cutover) ---

constexpr uint32_t INPUT_STREAM_MAX_INPUTS = 32;

// Piggybacked peer-status block; advisory only. adv_delay/adv_rollback are
// HUD/coverage-math inputs, never applied locally (INV-23); no pacing decision
// may read produced_through as an error term (INV-1).
struct PressureReport {
    uint32_t produced_through;   // newest local input sealed (frontier)
    uint32_t confirmed_frontier; // all inputs <= N are actual
    uint8_t  prediction_depth;   // current speculative depth
    uint8_t  run_state;          // Rollback::RunState
    uint8_t  adv_delay;          // sender's D_local (advisory)
    uint8_t  adv_rollback;       // sender's R_local (advisory)
};

// Redundant window anchored at peer_ack_through+1: oldest included frame =
// max(newest_frame - (INPUT_STREAM_MAX_INPUTS-1), peer_ack_through+1).
// inputs[0] = frame (newest_frame - count + 1) .. inputs[count-1] = newest.
// Senders transmit the used prefix only; receivers merge idempotently.
struct InputStreamPayload {
    uint64_t session_id;
    uint32_t epoch;
    uint32_t newest_frame;
    uint32_t ack_through;        // sender's contiguous remote-actual prefix
    uint8_t  count;              // 1..INPUT_STREAM_MAX_INPUTS
    uint8_t  _pad[3];
    uint16_t inputs[INPUT_STREAM_MAX_INPUTS];
    PressureReport pressure;
};

// --- Confirmed-frame verification (§2.7.7) ---

struct SyncHashPayload {
    uint64_t session_id;
    uint32_t epoch;
    uint32_t frame;              // canonical frame (confirmed on sender)
    uint64_t gameplay_hash;      // authoritative sim-affecting-state digest
    uint32_t rng_state;          // diagnostic
    uint16_t hp0;                // diagnostic
    uint16_t hp1;                // diagnostic
};

struct SyncHashAckPayload {
    uint32_t epoch;
    uint32_t frame;
};

// --- µs RTT probe (§2.9.3) ---

struct TimeProbePayload {
    uint32_t generation;         // estimator generation (stale replies inert)
    uint64_t stamp_us;           // opaque QPC µs stamp, echoed verbatim
};

struct TimeProbeAckPayload {
    uint32_t generation;
    uint64_t stamp_us;           // echoed request stamp
    uint32_t dwell_us;           // responder processing dwell to subtract
};

// --- Frontend starvation interrogation (INV-11) ---

struct ResyncRequestPayload {
    uint32_t epoch;
    uint8_t  phase_id;           // FrontendPhaseId
    uint8_t  native_mode;        // sender's native MODE_*
    uint16_t _pad;
    uint32_t local_frame;        // sender's current phase frame
};

// Same tuple, responder's view; mismatch triggers an EpochAlign re-run.
struct ResyncReplyPayload {
    uint32_t epoch;
    uint8_t  phase_id;
    uint8_t  native_mode;
    uint16_t _pad;
    uint32_t local_frame;
};

#pragma pack(pop)

// GekkoReadyPayload flags
constexpr uint8_t GEKKO_READY_FLAG_READY = 1 << 0;  // Local reached post-intro interactive boundary
constexpr uint8_t GEKKO_READY_FLAG_ACK   = 1 << 1;  // Local has observed peer READY

constexpr uint8_t CHARSEL_LOCK_FLAG_CUSTOM_PALETTE = 1 << 0;

constexpr uint8_t NETPLAY_PALETTE_FLAG_TRANSPORT_ENABLED = 1 << 0;
constexpr uint8_t NETPLAY_PALETTE_FLAG_REMOTE_PREVIEW_ENABLED = 1 << 1;
constexpr uint8_t NETPLAY_PALETTE_FLAG_HAS_CUSTOM_DATA = 1 << 2;
constexpr uint8_t NETPLAY_PALETTE_FLAG_SPECTATOR_PROPAGATE = 1 << 3;

static_assert(sizeof(PacketType) + sizeof(PaletteDataPayload) <= MAX_PACKET_SIZE,
    "PaletteDataPayload must fit inside one transport packet");
static_assert(sizeof(PacketType) + sizeof(CharSelInputPayload) <= MAX_PACKET_SIZE,
    "CharSelInputPayload must fit inside one transport packet");
static_assert(sizeof(PeerIdentityPayload) == 84,
    "PeerIdentityPayload wire size must remain stable");
static_assert(sizeof(DisconnectPayload) == 102,
    "DisconnectPayload wire size must remain stable (M4 §3.2 terminal shape)");
static_assert(sizeof(SyncAnnouncePayload) == 8,
    "SyncAnnouncePayload wire size must remain stable");
static_assert(sizeof(SyncConfirmPayload) == 12,
    "SyncConfirmPayload wire size must remain stable");
static_assert(sizeof(StageSyncPayload) == 24,
    "StageSyncPayload wire size must remain stable");
static_assert(sizeof(PacketType) + sizeof(SyncTracePayload) <= MAX_PACKET_SIZE,
    "SyncTracePayload must fit inside one transport packet");
static_assert(sizeof(PacketType) + sizeof(ChurnPausePayload) <= MAX_PACKET_SIZE,
    "ChurnPausePayload must fit inside one transport packet");

// v2 payload size pins (M1 facade/wire freeze — see docs/re0.7/API_FREEZE.md)
static_assert(sizeof(SessionHelloPayload) == 34,
    "SessionHelloPayload wire size must remain stable");
static_assert(sizeof(SessionOfferPayload) == 62,
    "SessionOfferPayload wire size must remain stable");
static_assert(sizeof(SessionAckPayload) == 62,
    "SessionAckPayload wire size must remain stable");
static_assert(sizeof(SessionConfirmPayload) == 8 && sizeof(SessionConfirmAckPayload) == 8,
    "Session confirm payload wire sizes must remain stable");
static_assert(sizeof(PressureReport) == 12,
    "PressureReport wire size must remain stable");
static_assert(sizeof(InputStreamPayload) == 24 + 2 * INPUT_STREAM_MAX_INPUTS + 12,
    "InputStreamPayload wire size must remain stable");
static_assert(sizeof(SyncHashPayload) == 32,
    "SyncHashPayload wire size must remain stable");
static_assert(sizeof(SyncHashAckPayload) == 8,
    "SyncHashAckPayload wire size must remain stable");
static_assert(sizeof(TimeProbePayload) == 12 && sizeof(TimeProbeAckPayload) == 16,
    "TimeProbe payload wire sizes must remain stable");
static_assert(sizeof(ResyncRequestPayload) == 12 && sizeof(ResyncReplyPayload) == 12,
    "Resync payload wire sizes must remain stable");
static_assert(sizeof(PhaseTransitionPayload) == 20,
    "PhaseTransitionPayload wire size must remain stable");
static_assert(sizeof(PacketType) + sizeof(InputStreamPayload) <= MAX_PACKET_SIZE,
    "InputStreamPayload must fit inside one transport packet");

// M1 contract-freeze pins for byte-stable payloads carried over from v18.
static_assert(sizeof(PaletteConfigPayload) == 20,
    "PaletteConfigPayload wire size must remain stable");
static_assert(sizeof(PaletteDataPayload) == 20 + NETPLAY_PALETTE_BANK_SIZE,
    "PaletteDataPayload wire size must remain stable");
static_assert(sizeof(PaletteAckPayload) == 16,
    "PaletteAckPayload wire size must remain stable");
static_assert(sizeof(CharSelFrameInputPayload) == 56,
    "CharSelFrameInputPayload wire size must remain stable");
static_assert(sizeof(WinScreenFrameInputPayload) == 56,
    "WinScreenFrameInputPayload wire size must remain stable");
static_assert(sizeof(FrontendPhaseBarrierPayload) == 20,
    "FrontendPhaseBarrierPayload wire size must remain stable");
static_assert(sizeof(FrontendBoundaryDigestPayload) == 36,
    "FrontendBoundaryDigestPayload wire size must remain stable");

// NatInfoPayload flags
constexpr uint8_t NAT_INFO_FLAG_UPNP_ENABLED      = 1 << 0;
constexpr uint8_t NAT_INFO_FLAG_UPNP_MAPPED       = 1 << 1;
constexpr uint8_t NAT_INFO_FLAG_STUN_ENABLED      = 1 << 2;
constexpr uint8_t NAT_INFO_FLAG_STUN_OK           = 1 << 3;
constexpr uint8_t NAT_INFO_FLAG_HOLE_PUNCH        = 1 << 4;
constexpr uint8_t NAT_INFO_FLAG_RELAY_FALLBACK    = 1 << 5;
constexpr uint8_t NAT_INFO_FLAG_PREFER_DIRECT     = 1 << 6;
constexpr uint8_t NAT_INFO_FLAG_IPV6_ENDPOINTS    = 1 << 7;
constexpr uint8_t NAT_INFO_EX_FLAG_TURN_ENABLED   = 1 << 0;
constexpr uint8_t NAT_INFO_EX_FLAG_PCP_ENABLED    = 1 << 1;

// ============================================================================
// Helpers
// ============================================================================

inline const char* SyncTraceDomainName(SyncTraceDomain domain) {
    switch (domain) {
        case SyncTraceDomain::None:             return "None";
        case SyncTraceDomain::FrontendLockstep: return "FrontendLockstep";
        case SyncTraceDomain::GameplayRollback: return "GameplayRollback";
        case SyncTraceDomain::LifecycleEvent:   return "LifecycleEvent";
        default:                                return "Unknown";
    }
}

inline const char* PacketTypeName(PacketType type) {
    switch (type) {
        case PacketType::Ready:          return "Ready";
        case PacketType::Disconnect:     return "Disconnect";
        case PacketType::SyncAnnounce:   return "SyncAnnounce";
        case PacketType::SyncConfirm:    return "SyncConfirm";
        case PacketType::FrontendPhaseBarrier: return "FrontendPhaseBarrier";
        case PacketType::FrontendBoundaryDigest: return "FrontendBoundaryDigest";
        case PacketType::NatInfo:        return "NatInfo";
        case PacketType::NatTraversalSignal: return "NatTraversalSignal";
        case PacketType::CharSelInput:   return "CharSelInput";
        case PacketType::CharSelLock:    return "CharSelLock";
        case PacketType::StageSync:      return "StageSync";
        case PacketType::ConfigExchange: return "ConfigExchange";
        case PacketType::ConfigAck:      return "ConfigAck";
        case PacketType::LoadBarrier:    return "LoadBarrier";
        case PacketType::BaselineReady:  return "BaselineReady";
        case PacketType::BaselineDigest: return "BaselineDigest";
        case PacketType::BaselineBreakdown: return "BaselineBreakdown";
        case PacketType::GameplayStart:  return "GameplayStart";
        case PacketType::GekkoReady:     return "GekkoReady";
        case PacketType::DelayChangeReq:  return "DelayChangeReq";
        case PacketType::DelayChangeAck:  return "DelayChangeAck";
        case PacketType::InputStream:     return "InputStream";
        case PacketType::CharSelFrameInput:   return "CharSelFrameInput";
        case PacketType::PauseQuit:           return "PauseQuit";
        case PacketType::WinScreenFrameInput: return "WinScreenFrameInput";
        case PacketType::PaletteConfig:       return "PaletteConfig";
        case PacketType::PaletteData:         return "PaletteData";
        case PacketType::PaletteAck:          return "PaletteAck";
        case PacketType::Ping:           return "Ping";
        case PacketType::Pong:           return "Pong";
        case PacketType::StateDigest:    return "StateDigest";
        case PacketType::FrameSyncStatus:return "FrameSyncStatus";
        case PacketType::SyncTrace:      return "SyncTrace";
        case PacketType::ChurnPause:     return "ChurnPause";
        case PacketType::PhaseTransitionProposal: return "PhaseTransitionProposal";
        case PacketType::PhaseTransitionAck:      return "PhaseTransitionAck";
        case PacketType::ResyncRequest:           return "ResyncRequest";
        case PacketType::ResyncReply:             return "ResyncReply";
        case PacketType::SessionHello:            return "SessionHello";
        case PacketType::SessionOffer:            return "SessionOffer";
        case PacketType::SessionAck:              return "SessionAck";
        case PacketType::SessionConfirm:          return "SessionConfirm";
        case PacketType::SessionConfirmAck:       return "SessionConfirmAck";
        case PacketType::SyncHash:                return "SyncHash";
        case PacketType::SyncHashAck:             return "SyncHashAck";
        case PacketType::TimeProbe:               return "TimeProbe";
        case PacketType::TimeProbeAck:            return "TimeProbeAck";
        case PacketType::PeerIdentity:            return "PeerIdentity";
        default:                         return "Unknown";
    }
}

/// Validate that a received buffer is large enough for [PacketType + payload].
/// Returns false if the buffer is too small for the packet type header.
inline bool ValidatePacketSize(const void* data, size_t length) {
    return data != nullptr && length >= sizeof(PacketType);
}

/// Extract PacketType from the front of a raw buffer.
inline PacketType ReadPacketType(const void* data) {
    return *static_cast<const PacketType*>(data);
}

/// Get a pointer to the payload after the PacketType header.
inline const void* GetPayloadPtr(const void* data) {
    return static_cast<const uint8_t*>(data) + sizeof(PacketType);
}

/// Get payload size from total packet length.
inline size_t GetPayloadSize(size_t totalLength) {
    return totalLength > sizeof(PacketType) ? totalLength - sizeof(PacketType) : 0;
}

} // namespace Net

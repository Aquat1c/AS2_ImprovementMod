/**
 * Alice Senki 2 - Rollback Session Implementation
 *
 * Bridges GekkoNet rollback library ↔ mod systems:
 *   - GekkoNetAdapter sends/receives through our UdpSocket via PacketCodec
 *   - GekkoAdvanceEvent → AS2_SetSynchronizedInputOverride(p1, p2)
 *   - GekkoSaveEvent → AS2_SaveStateToBuffer()
 *   - GekkoLoadEvent → AS2_LoadStateFromBuffer()
 */

#include "rollback_session.h"

#include "session_manager.h"
#include "packet_codec.h"

#include <winsock2.h>  // For sockaddr_in used by GekkoNet address matching

#include "log_window.h"
#include "as2_rollback.h"
#include "audio_hooks.h"
#include "render_hooks.h"
#include "rng_hooks.h"
#include "desync_diagnostics.h"
#include "test_harness.h"

#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC
#endif
#include <gekkonet.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace RollbackSession {

// ============================================================================
// Constants
// ============================================================================

static constexpr int    kMaxSavestateSlots  = 16;   // Ring buffer for GekkoNet savestates
static constexpr int    kMaxPendingRecv     = 32;   // Max packets to drain per frame
static constexpr int    kDefaultPredWindow  = 8;    // Default input prediction window
static constexpr int    kDefaultCheckDist   = 20;   // Desync check every N frames

// ============================================================================
// Internal state
// ============================================================================

namespace {

// GekkoNet session
static GekkoSession*    s_session       = nullptr;
static GekkoNetAdapter  s_adapter       = {};
static GekkoConfig      s_gekkoConfig   = {};

// Session state
static State            s_state         = State::Inactive;
static Config           s_config        = {};
static bool             s_rollingBack   = false;
static int              s_localFrame    = 0;
static int              s_remoteFrame   = 0;
static uint16_t         s_currentP1Input = 0;
static uint16_t         s_currentP2Input = 0;
static bool             s_currentInputsValid = false;
static int              s_rollbackDepth = 0;
static int              s_lastRollbackDepth = 0;  // persists after rollback ends for HUD display

// Stats
static uint32_t         s_saveCount     = 0;
static uint32_t         s_loadCount     = 0;
static uint32_t         s_advanceCount  = 0;
static uint32_t         s_rollbackCount = 0;
static uint32_t         s_adapterSendCount = 0;
static uint32_t         s_adapterRecvCount = 0;
static bool             s_warnedSyncFrameAdvance = false;
static uint32_t         s_lastHeartbeatMs = 0;
static uint32_t         s_timesyncSkipCount = 0;

// Network context: dummy address for GekkoNet address matching
static sockaddr_in      s_dummyPeerAddr = {};

// GekkoNet player handles
static int              s_localHandle   = -1;
static int              s_remoteHandle  = -1;

// Savestate ring buffer — GekkoNet manages which frames to save/load,
// we just provide storage. Each slot holds a full CompactSaveState_t.
static CompactSaveState_t* s_savestates = nullptr;
static int              s_savestateCount = 0;

// Receive buffer — GekkoNetResult array for adapter
static GekkoNetResult*  s_recvResults[kMaxPendingRecv] = {};
static int              s_recvCount = 0;

// Status text
static char             s_status[128]   = "Inactive";

// Two-phase event stepping: store pending events between BeginFrame/ProcessNextEvent
static GekkoGameEvent** s_pendingEvents    = nullptr;
static int              s_pendingEventCount = 0;
static int              s_pendingEventIdx   = 0;

// ============================================================================
// Helpers
// ============================================================================

static void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, args);
    va_end(args);
}

static const char* StateToString(State state) {
    switch (state) {
        case State::Inactive: return "Inactive";
        case State::Syncing:  return "Syncing";
        case State::Running:  return "Running";
        case State::Error:    return "Error";
        default:              return "Unknown";
    }
}

static void LogHeartbeat(bool force, const char* reason) {
    if (!s_session || s_state == State::Inactive) return;

    const uint32_t now = PacketCodec::GetTimestampMs();
    if (!force && (now - s_lastHeartbeatMs) < 1000) {
        return;
    }
    s_lastHeartbeatMs = now;

    RenderHooks::Stats renderStats = {};
    AudioHooks::Stats audioStats = {};
    RenderHooks::GetStats(&renderStats);
    AudioHooks::GetStats(&audioStats);

    uint32_t simFrame = 0;
    uint32_t renderFrame = 0;
    uint32_t displayFrame = 0;
    uint32_t writeFrame = 0;
    uint32_t netFrame = 0;

    __try {
        simFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_SIMULATION);
        renderFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_COUNTER);
        displayFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_DISPLAY);
        writeFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_WRITE_IDX);
        netFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_NET_IDX);
    } __except (1) {
    }

    LOG_NET_INFO(
        "[Rollback] Heartbeat(%s): state=%s local=%d remote=%d ahead=%.2f pending=%d "
        "saves=%u loads=%u adv=%u rb=%u adapter[sent=%u recv=%u] rolling=%d "
        "render[supp=%d total=%u draw=%u skip=%u] audio[supp=%d total=%u pass=%u skip=%u] "
        "frames[sim=%u render=%u disp=%u write=%u net=%u match=%u]",
        reason ? reason : "tick",
        StateToString(s_state),
        s_localFrame,
        s_remoteFrame,
        s_session ? gekko_frames_ahead(s_session) : 0.0f,
        s_pendingEventCount - s_pendingEventIdx,
        s_saveCount,
        s_loadCount,
        s_advanceCount,
        s_rollbackCount,
        s_adapterSendCount,
        s_adapterRecvCount,
        s_rollingBack ? 1 : 0,
        RenderHooks::IsSuppressed() ? 1 : 0,
        renderStats.total_calls,
        renderStats.rendered_calls,
        renderStats.suppressed_calls,
        AudioHooks::IsSuppressed() ? 1 : 0,
        audioStats.total_calls,
        audioStats.passed_calls,
        audioStats.suppressed_calls,
        simFrame,
        renderFrame,
        displayFrame,
        writeFrame,
        netFrame,
        AS2_GetMatchVisualFrame());
}

// ============================================================================
// GekkoNetAdapter Implementation
// ============================================================================

// Called by GekkoNet to send data to the remote peer.
// Sends through SessionManager's SendToPeer (ENet transport).
static void AdapterSendData(GekkoNetAddress* addr, const char* data, int length) {
    if (length <= 0 || !data) {
        LOG_NET_ERROR("[Rollback] AdapterSendData abort: len=%d data=%p", length, data);
        return;
    }

    SessionManager::SendToPeer(PacketCodec::PacketType::GekkoData, data, (size_t)length, false);

    s_adapterSendCount++;
    LOG_GEKKO_DEBUG("[Gekko][Adapter] Send payload=%d count=%u",
                    length, s_adapterSendCount);
    if (s_adapterSendCount <= 8 || s_state != State::Running) {
        LOG_NET_DEBUG("[Rollback] AdapterSendData #%u payload=%d",
                      s_adapterSendCount, length);
    }
}

// Called by GekkoNet to receive all pending packets.
// We drain from SessionManager's GekkoData buffer (packets already decoded by SessionManager).
// Note: GekkoNet will call free_data on each result after processing.
static GekkoNetResult** AdapterReceiveData(int* length) {
    *length = 0;
    s_recvCount = 0;

    // Drain buffered GekkoData payloads from SessionManager
    SessionManager::BufferedPacket packets[kMaxPendingRecv];
    int count = SessionManager::DrainGekkoPackets(packets, kMaxPendingRecv);

    for (int i = 0; i < count; i++) {
        if (packets[i].len <= 0) continue;

        // Allocate a GekkoNetResult for GekkoNet to consume
        GekkoNetResult* result = (GekkoNetResult*)malloc(sizeof(GekkoNetResult));
        if (!result) continue;

        void* dataCopy = malloc(packets[i].len);
        if (!dataCopy) {
            free(result);
            continue;
        }
        memcpy(dataCopy, packets[i].data, packets[i].len);

        // Build peer address for GekkoNet (it needs to match what we gave it)
        result->addr.data = malloc(sizeof(sockaddr_in));
        if (!result->addr.data) {
            free(dataCopy);
            free(result);
            continue;
        }
        memcpy(result->addr.data, &s_dummyPeerAddr, sizeof(sockaddr_in));
        result->addr.size = sizeof(sockaddr_in);
        result->data = dataCopy;
        result->data_len = (unsigned int)packets[i].len;

        s_recvResults[s_recvCount++] = result;
        if (s_recvCount >= kMaxPendingRecv) break;
    }

    if (s_recvCount > 0) {
        s_adapterRecvCount += (uint32_t)s_recvCount;
        LOG_GEKKO_DEBUG("[Gekko][Adapter] Recv drained=%d total=%u",
                        s_recvCount, s_adapterRecvCount);
        if (s_adapterRecvCount <= 8 || s_state != State::Running) {
            LOG_NET_DEBUG("[Rollback] AdapterReceiveData drained %d packet(s) (total=%u)",
                          s_recvCount, s_adapterRecvCount);
        }
    }

    *length = s_recvCount;
    return s_recvCount > 0 ? s_recvResults : nullptr;
}

// Called by GekkoNet to free memory we allocated in receive_data
static void AdapterFreeData(void* data_ptr) {
    if (!data_ptr) return;

    // GekkoNet calls free_data separately on result->addr.data, result->data,
    // and the GekkoNetResult object itself, so this callback must free exactly
    // the pointer it receives.
    free(data_ptr);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void HandleAdvanceEvent(const GekkoGameEvent* event) {
    int frame = event->data.adv.frame;
    bool rolling_back = event->data.adv.rolling_back;
    const uint8_t* inputs = event->data.adv.inputs;
    unsigned int input_len = event->data.adv.input_len;
    const uint32_t traceEventId = AS2_NextRollbackTraceEventId();

    const uint32_t beforeSim = ReadMemory<uint32_t>(ADDR_FRAME_SIMULATION);
    const uint32_t beforeDisplay = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);
    const uint32_t beforeWrite = ReadMemory<uint32_t>(ADDR_FRAME_WRITE_IDX);
    const uint32_t beforeNet = ReadMemory<uint32_t>(ADDR_FRAME_NET_IDX);

    AS2_LogRollbackTraceMessage("advance",
                                frame,
                                traceEventId,
                                "begin rolling=%d input_len=%u local_before=%d remote_before=%d live[sim=%u disp=%u write=%u net=%u]",
                                rolling_back ? 1 : 0,
                                input_len,
                                s_localFrame,
                                s_remoteFrame,
                                beforeSim,
                                beforeDisplay,
                                beforeWrite,
                                beforeNet);
    if (inputs && input_len > 0) {
        AS2_DumpRollbackTraceBytes("advance", frame, traceEventId, "gekkonet_input_payload", 0, nullptr, 0, inputs, input_len);
    }

    s_rollingBack = rolling_back;
    s_localFrame = frame;

    // Keep Frame_Simulation in sync with GekkoNet frame.
    // Hook_AdvanceFrame suppresses the vanilla increment, so nothing else
    // updates this counter.  Without this write the client (which may have
    // 0 rollback loads) would report Sim=0 forever, and the host's value
    // would drift to the last-loaded frame.
    if (frame >= 0) {
        WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, (uint32_t)frame);
    }

    // Suppress audio and rendering during rollback resimulation (§17A)
    AudioHooks::SetSuppressed(rolling_back);
    RenderHooks::SetSuppressed(rolling_back);

    if (rolling_back) {
        s_rollbackDepth++;
    } else {
        if (s_rollbackDepth > 0) {
            LOG_GEKKO_INFO("[Gekko][Event] Rollback complete: depth=%d frames resimulated, now at frame %d",
                           s_rollbackDepth, frame);
            s_lastRollbackDepth = s_rollbackDepth;  // persist for HUD
            s_rollbackCount++;
            TestHarness::RecordRollback((uint32_t)((frame >= 0) ? frame : 0), s_rollbackDepth, s_rollbackDepth);
        }
        s_rollbackDepth = 0;
        
        // Capture desync checksum on non-rollback frames
        DesyncDiag::CaptureFrame((uint32_t)frame);
    }

    // GekkoNet provides inputs for all players concatenated:
    // [player0_input (input_size bytes)] [player1_input (input_size bytes)]
    // input_size = sizeof(uint16_t) = 2
    // player 0 = P1, player 1 = P2 (regardless of who is local)
    uint16_t p1Input = 0, p2Input = 0;
    if (input_len >= 4 && inputs) {
        p1Input = *(const uint16_t*)(inputs);
        p2Input = *(const uint16_t*)(inputs + 2);

        s_currentP1Input = p1Input;
        s_currentP2Input = p2Input;
        s_currentInputsValid = true;
        AS2_SetSynchronizedInputOverride(p1Input, p2Input);
    } else {
        s_currentP1Input = 0;
        s_currentP2Input = 0;
        s_currentInputsValid = false;
        AS2_SetSynchronizedInputOverride(0, 0);
        LOG_NET_WARN("[Rollback] Advance event missing input payload at frame %d (len=%u)",
                     frame, input_len);
    }

    s_advanceCount++;

    const uint32_t afterSim = ReadMemory<uint32_t>(ADDR_FRAME_SIMULATION);
    const uint32_t afterDisplay = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);
    const uint32_t afterWrite = ReadMemory<uint32_t>(ADDR_FRAME_WRITE_IDX);
    const uint32_t afterNet = ReadMemory<uint32_t>(ADDR_FRAME_NET_IDX);
    AS2_LogRollbackTraceMessage("advance",
                                frame,
                                traceEventId,
                                "end rolling=%d depth=%d p1=0x%04X p2=0x%04X valid=%d live[sim=%u disp=%u write=%u net=%u] adv=%u",
                                rolling_back ? 1 : 0,
                                s_rollbackDepth,
                                p1Input,
                                p2Input,
                                s_currentInputsValid ? 1 : 0,
                                afterSim,
                                afterDisplay,
                                afterWrite,
                                afterNet,
                                s_advanceCount);

    // Log every advance during first 60 frames and rollback frames;
    // afterwards log every 60th frame to avoid spam
    if (frame < 60 || rolling_back || (frame % 60) == 0) {
        LOG_GEKKO_DEBUG("[Gekko][Event] Advance frame=%d rb=%d depth=%d p1=0x%04X p2=0x%04X "
                        "inputLen=%u adv#=%u ahead=%.1f",
                        frame, rolling_back ? 1 : 0, s_rollbackDepth,
                        p1Input, p2Input, input_len, s_advanceCount,
                        s_session ? gekko_frames_ahead(s_session) : 0.0f);
    }
}

static void HandleSaveEvent(const GekkoGameEvent* event) {
    int frame = event->data.save.frame;
    const uint32_t traceEventId = AS2_NextRollbackTraceEventId();

    // Find a savestate slot (ring buffer by frame)
    int slotIdx = frame % kMaxSavestateSlots;
    if (slotIdx < 0) slotIdx += kMaxSavestateSlots;
    CompactSaveState_t* slot = &s_savestates[slotIdx];

    AS2_LogRollbackTraceMessage("save", frame, traceEventId, "begin slot=%d rolling=%d", slotIdx, s_rollingBack ? 1 : 0);

    // Capture current game state (fast path — no memset, no digest, no CRC)
    AS2_SaveStateFastTrace(slot, "save-captured", frame, traceEventId);

    const auto preOverrideGlobal = slot->global;
    const auto preOverrideInput = slot->input;
    uint8_t preOverrideMatchHeader[16] = {};
    memcpy(preOverrideMatchHeader, slot->match.match_header, sizeof(preOverrideMatchHeader));

    // ── FIX: Override frame counters with deterministic GekkoNet values ──
    //
    // The game increments several counters inside the while(!InputDispatcher)
    // simulation loop. During rollback resimulation the loop iterates N times,
    // causing these counters to accumulate differently on host vs client
    // depending on their rollback histories. By overriding with values derived
    // from the GekkoNet frame, every saved state is deterministic regardless of
    // where it was captured (forward pass, resim pass 1, resim pass 2, etc.).
    //
    // CRITICAL: GekkoNet's event order is Save(N) → Advance(N).
    // Save captures the state BEFORE simulation of frame N runs.
    // At that point, N simulation steps have already completed (frames 0..N-1),
    // so the live counters are:
    //   Frame_Simulation = N-1 (set by HandleAdvanceEvent, not yet incremented for frame N)
    //   Frame_Display    = N   (incremented once per sim step inside the while loop)
    //   Frame_WriteIdx   = N   (incremented once per PublishRollbackInputs call)
    //
    // We normalize all counters to their expected pre-sim-N values so that
    // Load(N) + Advance(N) produces identical state to the original forward pass.
    //
    // Match header announcer timer (match_base + 2, uint16) — also incremented
    //   inside the sim loop. Set to 0xFFFF (-1) which disables the announcer
    //   check entirely in saved states. The game re-enables it at round
    //   transitions. This prevents accumulated drift from causing different
    //   audio-related side effects (rand() consumption, etc.) between sides.
    if (frame >= 0) {
        slot->global.frame_counter = (uint32_t)frame;
        slot->input.read_idx = (uint32_t)frame;
        slot->input.display_idx = (uint32_t)frame;
        // Frame_WriteIdx (Frame_Inputs, 0x816498) is also incremented inside
        // the simulation loop (once per sim step, same as display_idx).
        // Without normalization, different rollback histories between host
        // and client produce different captured write_idx values — the game
        // uses write_idx to index into input history buffers, so drift here
        // can cause divergent input reads and eventual gameplay desync.
        slot->input.write_idx = (uint32_t)frame;
        // Normalize announcer timer to disabled (-1) to prevent drift.
        // The timer is a uint16 at match_header[2..3].
        slot->match.match_header[2] = 0xFF;
        slot->match.match_header[3] = 0xFF;
    }

    AS2_DumpRollbackTraceBytes("save-override", frame, traceEventId, "saved_global", 0, &preOverrideGlobal, sizeof(preOverrideGlobal), &slot->global, sizeof(slot->global));
    AS2_DumpRollbackTraceBytes("save-override", frame, traceEventId, "saved_input", 0, &preOverrideInput, sizeof(preOverrideInput), &slot->input, sizeof(slot->input));
    AS2_DumpRollbackTraceBytes("save-override", frame, traceEventId, "saved_match_header", 0, preOverrideMatchHeader, sizeof(preOverrideMatchHeader), slot->match.match_header, sizeof(preOverrideMatchHeader));

    // Compute checksum from the normalized savestate so GekkoNet health checks
    // verify the same state bytes we just handed it.
    uint32_t checksum = AS2_ComputeCompactStateChecksum(slot);
    slot->checksum = checksum;
    TestHarness::RecordChecksum((uint32_t)((frame >= 0) ? frame : 0), checksum);

    // Tell GekkoNet about the state
    if (event->data.save.checksum) {
        *(event->data.save.checksum) = checksum;
    }
    if (event->data.save.state_len) {
        *(event->data.save.state_len) = sizeof(CompactSaveState_t);
    }
    // GekkoNet provides state buffer — we copy our state into it
    if (event->data.save.state) {
        memcpy(event->data.save.state, slot, sizeof(CompactSaveState_t));
    }

    s_saveCount++;
    AS2_LogRollbackTraceMessage("save",
                                frame,
                                traceEventId,
                                "end slot=%d checksum=0x%08X state_len=%zu save_count=%u",
                                slotIdx,
                                checksum,
                                sizeof(CompactSaveState_t),
                                s_saveCount);

    // Log save events: first 30 frames, then every 60th — but NOT during
    // rollback resimulation where this fires many times per visual frame.
    // AS2_FormatCompactStateSummary iterates 300 entries + 60-arg snprintf.
    if (!s_rollingBack && (frame < 30 || (frame % 60) == 0)) {
        char summary[512] = {};
        const char* summaryText = AS2_FormatCompactStateSummary(slot, summary, sizeof(summary))
            ? summary
            : "summary-unavailable";
        LOG_INFO("[Gekko][Event] Save frame=%d slot=%d checksum=0x%08X stateSize=%zu save#=%u snapshot=%s",
                 frame, slotIdx, checksum, sizeof(CompactSaveState_t),
                 s_saveCount, summaryText);

        // Log per-field digest breakdown for the first 10 frames to help
        // identify exactly which digest component diverges between peers.
        if (frame < 10) {
            AS2_LogDigestBreakdown(slot, frame);
        }
    }
}

static void HandleLoadEvent(const GekkoGameEvent* event) {
    int frame = event->data.load.frame;
    const uint32_t traceEventId = AS2_NextRollbackTraceEventId();
    AS2_LogRollbackTraceMessage("load",
                                frame,
                                traceEventId,
                                "begin state_len=%u rolling=%d",
                                event->data.load.state_len,
                                s_rollingBack ? 1 : 0);

    // Log pre-load live state — skip during rollback to avoid 12× ReadMemory + snprintf per resim step
    if (!s_rollingBack) {
        uint32_t preSim = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        uintptr_t p1 = GetEntityBase(0);
        uintptr_t p2 = GetEntityBase(1);
        LOG_INFO("[Gekko][Event] Load PRE frame=%d liveSim=%u "
                 "p1=@(%d,%d) f=%u push=%d act=%u "
                 "p2=@(%d,%d) f=%u push=%d act=%u",
                 frame, preSim,
                 ReadMemory<int16_t>(p1 + ENTITY_OFF_X_POS),
                 ReadMemory<int16_t>(p1 + ENTITY_OFF_Y_POS),
                 ReadMemory<uint8_t>(p1 + ENTITY_OFF_FACING),
                 (int)(int8_t)ReadMemory<uint8_t>(p1 + ENTITY_OFF_PUSH_DIR),
                 ReadMemory<uint32_t>(p1 + ENTITY_OFF_ACTION_ID),
                 ReadMemory<int16_t>(p2 + ENTITY_OFF_X_POS),
                 ReadMemory<int16_t>(p2 + ENTITY_OFF_Y_POS),
                 ReadMemory<uint8_t>(p2 + ENTITY_OFF_FACING),
                 (int)(int8_t)ReadMemory<uint8_t>(p2 + ENTITY_OFF_PUSH_DIR),
                 ReadMemory<uint32_t>(p2 + ENTITY_OFF_ACTION_ID));
    }

    bool usedGekkoBuffer = false;
    const CompactSaveState_t* loadedState = nullptr;
    // GekkoNet provides the state buffer to restore from
    if (event->data.load.state && event->data.load.state_len >= sizeof(CompactSaveState_t)) {
        const CompactSaveState_t* state = (const CompactSaveState_t*)event->data.load.state;
        loadedState = state;
        AS2_LoadStateFastTrace(state, "load-apply", frame, traceEventId);
        usedGekkoBuffer = true;
    } else {
        // Fallback: load from our ring buffer
        int slotIdx = frame % kMaxSavestateSlots;
        if (slotIdx < 0) slotIdx += kMaxSavestateSlots;
        loadedState = &s_savestates[slotIdx];
        AS2_LoadStateFastTrace(loadedState, "load-apply", frame, traceEventId);
    }

    // ── FIX: Force sim counter to match GekkoNet frame after load ────
    // AS2_LoadStateFromBuffer restores the saved sim counter value, but
    // during rollback the saved value may have been captured while sim was
    // stale (sub_562760 only runs at loop exit). Force it to the GekkoNet
    // frame to ensure resimulation starts from the correct counter value.
    if (frame >= 0) {
        const uint32_t beforeForcedSim = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, (uint32_t)frame);
        const uint32_t afterForcedSim = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        AS2_DumpRollbackTraceBytes("load-post",
                                   frame,
                                   traceEventId,
                                   "forced_sim_frame",
                                   ADDR_SIM_FRAME_COUNTER,
                                   &beforeForcedSim,
                                   sizeof(beforeForcedSim),
                                   &afterForcedSim,
                                   sizeof(afterForcedSim));
    }

    s_loadCount++;
    AS2_LogRollbackTraceMessage("load",
                                frame,
                                traceEventId,
                                "end src=%s load_count=%u quick=0x%04X",
                                usedGekkoBuffer ? "gekko" : "ringbuf",
                                s_loadCount,
                                AS2_GetQuickChecksum());

    // Skip expensive summary formatting during rollback resimulation
    if (!s_rollingBack) {
        int slotIdxLog = frame % kMaxSavestateSlots;
        if (slotIdxLog < 0) slotIdxLog += kMaxSavestateSlots;

        char summary[512] = {};
        const char* summaryText = (loadedState && AS2_FormatCompactStateSummary(loadedState, summary, sizeof(summary)))
            ? summary
            : "summary-unavailable";

        LOG_INFO("[Gekko][Event] Load frame=%d slot=%d load#=%u src=%s stateLen=%u snapshot=%s",
                 frame, slotIdxLog, s_loadCount,
                 usedGekkoBuffer ? "gekko" : "ringbuf",
                 event->data.load.state_len,
                 summaryText);
    }
}

static void HandleSessionEvents() {
    if (!s_session) return;

    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(s_session, &count);
    if (!events || count <= 0) return;

    for (int i = 0; i < count; i++) {
        GekkoSessionEvent* ev = events[i];
        if (!ev) continue;

        switch (ev->type) {
        case GekkoPlayerSyncing:
            LOG_GEKKO_INFO("[Gekko][Session] PlayerSyncing handle=%d current=%u max=%u",
                           ev->data.syncing.handle,
                           ev->data.syncing.current,
                           ev->data.syncing.max);
            LOG_NET_INFO("[Rollback] Player %d syncing (%u/%u)",
                        ev->data.syncing.handle,
                        ev->data.syncing.current,
                        ev->data.syncing.max);
            SetStatus("Syncing... (%u/%u)", ev->data.syncing.current, ev->data.syncing.max);
            AS2_LogFrameCounterState(s_localFrame, "GekkoSyncing");
            LogHeartbeat(true, "player-syncing");
            break;

        case GekkoPlayerConnected:
            LOG_GEKKO_INFO("[Gekko][Session] PlayerConnected handle=%d",
                           ev->data.connected.handle);
            LOG_NET_INFO("[Rollback] Player %d connected", ev->data.connected.handle);
            break;

        case GekkoPlayerDisconnected:
            LOG_GEKKO_WARN("[Gekko][Session] PlayerDisconnected handle=%d",
                           ev->data.disconnected.handle);
            LOG_NET_WARN("[Rollback] Player %d disconnected",
                        ev->data.disconnected.handle);
            s_state = State::Error;
            SetStatus("Player disconnected.");
            break;

        case GekkoSessionStarted:
            LOG_GEKKO_INFO("[Gekko][Session] SessionStarted");
            LOG_NET_INFO("[Rollback] Session started — rollback active!");
            s_state = State::Running;
            SetStatus("Rollback active.");
            AS2_ResetMatchVisualFrame();
            AS2_LogFrameCounterState(s_localFrame, "GekkoSessionStarted");
            LogHeartbeat(true, "session-started");
            break;

        case GekkoDesyncDetected:
            LOG_GEKKO_ERROR("[Gekko][Session] DESYNC frame=%d local=0x%08X remote=0x%08X",
                        ev->data.desynced.frame,
                        ev->data.desynced.local_checksum,
                        ev->data.desynced.remote_checksum);
            LOG_NET_ERROR("[Rollback] DESYNC at frame %d! local=0x%08X remote=0x%08X",
                        ev->data.desynced.frame,
                        ev->data.desynced.local_checksum,
                        ev->data.desynced.remote_checksum);
            // Dump comprehensive state on desync for debugging
            {
                uint32_t quickCksum = AS2_GetQuickChecksum();
                uint32_t simFrame = *reinterpret_cast<volatile uint32_t*>(ADDR_FRAME_SIMULATION);
                LOG_GEKKO_ERROR("[Gekko][Session] DESYNC state: quickChecksum=0x%04X simFrame=%u "
                                "localFrame=%d remoteFrame=%d saves=%u loads=%u rollbacks=%u",
                                quickCksum, simFrame, s_localFrame, s_remoteFrame,
                                s_saveCount, s_loadCount, s_rollbackCount);
            }
                    TestHarness::RecordDesync((uint32_t)ev->data.desynced.frame,
                                      ev->data.desynced.local_checksum,
                                      ev->data.desynced.remote_checksum,
                                      "GekkoSession");
            break;

        default:
            break;
        }
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

bool Create(const Config* config) {
    if (s_session) {
        LOG_NET_WARN("[Rollback] Session already active — destroying first.");
        Destroy();
    }

    if (!config) {
        LOG_NET_ERROR("[Rollback] Create failed: invalid parameters.");
        return false;
    }

    // Store context
    memcpy(&s_config, config, sizeof(Config));

    // Set up dummy peer address for GekkoNet address matching
    memset(&s_dummyPeerAddr, 0, sizeof(s_dummyPeerAddr));
    s_dummyPeerAddr.sin_family = AF_INET;
    s_dummyPeerAddr.sin_addr.s_addr = htonl(0x7F000001); // 127.0.0.1
    s_dummyPeerAddr.sin_port = htons(12345);

    // Allocate savestate ring buffer
    s_savestates = (CompactSaveState_t*)calloc(kMaxSavestateSlots, sizeof(CompactSaveState_t));
    if (!s_savestates) {
        LOG_NET_ERROR("[Rollback] Failed to allocate savestate buffer (%zu bytes).",
                    (size_t)kMaxSavestateSlots * sizeof(CompactSaveState_t));
        return false;
    }
    s_savestateCount = kMaxSavestateSlots;

    // Create GekkoNet session
    if (!gekko_create(&s_session, GekkoGameSession)) {
        LOG_NET_ERROR("[Rollback] gekko_create(GekkoGameSession) failed.");
        free(s_savestates);
        s_savestates = nullptr;
        return false;
    }

    // Configure
    memset(&s_gekkoConfig, 0, sizeof(s_gekkoConfig));
    s_gekkoConfig.num_players = 2;
    s_gekkoConfig.max_spectators = 0;
    s_gekkoConfig.input_prediction_window = config->max_rollback > 0 ? config->max_rollback : kDefaultPredWindow;
    s_gekkoConfig.spectator_delay = 0;
    s_gekkoConfig.input_size = sizeof(uint16_t);               // 2 bytes per input
    s_gekkoConfig.state_size = sizeof(CompactSaveState_t);     // Full savestate size
    s_gekkoConfig.limited_saving = false;                       // Save every frame — avoids forced resim cycles
    s_gekkoConfig.desync_detection = config->desync_detection;
    s_gekkoConfig.check_distance = kDefaultCheckDist;

    gekko_start(s_session, &s_gekkoConfig);

    // Set up network adapter AFTER starting session — GekkoNet's gekko_start (Init)
    // explicitly nulls out the _host/adapter pointers, so we must set it afterwards.
    s_adapter.send_data = AdapterSendData;
    s_adapter.receive_data = AdapterReceiveData;
    s_adapter.free_data = AdapterFreeData;
    gekko_net_adapter_set(s_session, &s_adapter);

    // Add players
    // Build peer address for GekkoNet
    GekkoNetAddress localAddr = {};
    GekkoNetAddress remoteAddr = {};
    remoteAddr.data = (void*)&s_dummyPeerAddr;
    remoteAddr.size = sizeof(sockaddr_in);

    if (config->is_host) {
        // Host = local player 0 (P1), remote player 1 (P2)
        s_localHandle = gekko_add_actor(s_session, GekkoLocalPlayer, &localAddr);
        s_remoteHandle = gekko_add_actor(s_session, GekkoRemotePlayer, &remoteAddr);
    } else {
        // Join = remote player 0 (P1), local player 1 (P2)
        s_remoteHandle = gekko_add_actor(s_session, GekkoRemotePlayer, &remoteAddr);
        s_localHandle = gekko_add_actor(s_session, GekkoLocalPlayer, &localAddr);
    }

    if (s_localHandle < 0 || s_remoteHandle < 0) {
        LOG_NET_ERROR("[Rollback] Failed to add Gekko actors (local=%d remote=%d).",
                    s_localHandle, s_remoteHandle);
        gekko_destroy(&s_session);
        s_session = nullptr;
        free(s_savestates);
        s_savestates = nullptr;
        s_savestateCount = 0;
        return false;
    }

    // Set local input delay
    int delay = config->input_delay;
    if (delay < 0) delay = 0;
    if (delay > 15) delay = 15;
    gekko_set_local_delay(s_session, s_localHandle, (unsigned char)delay);

    // Reset state
    s_state = State::Syncing;
    s_rollingBack = false;
    s_localFrame = 0;
    s_remoteFrame = 0;
    s_rollbackDepth = 0;
    s_lastRollbackDepth = 0;
    s_saveCount = 0;
    s_loadCount = 0;
    s_advanceCount = 0;
    s_rollbackCount = 0;
    s_recvCount = 0;
    s_adapterSendCount = 0;
    s_adapterRecvCount = 0;
    s_warnedSyncFrameAdvance = false;
    s_lastHeartbeatMs = 0;
    s_timesyncSkipCount = 0;

    SetStatus("Created. Waiting for sync...");
    LOG_GEKKO_INFO("[Gekko][Session] Created host=%d delay=%d local=%d remote=%d",
                   config->is_host ? 1 : 0, delay, s_localHandle, s_remoteHandle);
    LOG_GEKKO_INFO("[Gekko][Session] Config: players=%d inputSize=%u stateSize=%u "
                   "predWindow=%d limitedSaving=%d desyncDetect=%d checkDist=%d",
                   s_gekkoConfig.num_players, s_gekkoConfig.input_size,
                   s_gekkoConfig.state_size, s_gekkoConfig.input_prediction_window,
                   s_gekkoConfig.limited_saving ? 1 : 0,
                   s_gekkoConfig.desync_detection ? 1 : 0,
                   s_gekkoConfig.check_distance);
    LOG_GEKKO_INFO("[Gekko][Session] Savestate ring buffer: %d slots x %zu bytes = %zu bytes total",
                   kMaxSavestateSlots, sizeof(CompactSaveState_t),
                   (size_t)kMaxSavestateSlots * sizeof(CompactSaveState_t));
    LOG_NET_INFO("[Rollback] Created session: host=%s delay=%d prediction=%d stateSize=%zu",
                config->is_host ? "yes" : "no", delay,
                s_gekkoConfig.input_prediction_window,
                sizeof(CompactSaveState_t));
    LogHeartbeat(true, "create");

    return true;
}

void Destroy() {
    if (s_session) {
        gekko_destroy(&s_session);
        s_session = nullptr;
    }

    if (s_savestates) {
        free(s_savestates);
        s_savestates = nullptr;
    }
    s_savestateCount = 0;

    // GekkoNet owns and frees adapter receive buffers during Poll().
    // We only keep these pointers as a transient array for the current call.
    for (int i = 0; i < kMaxPendingRecv; i++) {
        s_recvResults[i] = nullptr;
    }
    s_recvCount = 0;

    s_currentP1Input = 0;
    s_currentP2Input = 0;
    s_currentInputsValid = false;

    AS2_ClearSynchronizedInputOverride();
    AudioHooks::SetSuppressed(false);
    RenderHooks::SetSuppressed(false);

    s_state = State::Inactive;
    s_localHandle = -1;
    s_remoteHandle = -1;
    s_rollingBack = false;
    SetStatus("Inactive");
    s_lastHeartbeatMs = 0;

    LOG_GEKKO_INFO("[Gekko][Session] Destroyed");
    LOG_NET_INFO("[Rollback] Session destroyed.");
}

void Update(uint16_t localInput) {
    // Legacy single-call API: calls both phases internally.
    // Note: This does NOT interleave game simulation between events,
    // so Save events after Advance events capture pre-simulation state.
    // For correct rollback, use BeginFrame() + ProcessNextEvent() instead.
    BeginFrame(localInput);
    while (ProcessNextEvent() == EventResult::FrameAdvance) {
        // Caller is responsible for game simulation in the two-phase path.
        // In the single-call path we just consume all events.
    }
}

void NetworkPoll() {
    if (!s_session || s_state == State::Inactive || s_state == State::Error) {
        return;
    }

    LOG_GEKKO_DEBUG("[Gekko][Session] NetworkPoll state=%s local=%d remote=%d pending=%d",
                    StateToString(s_state), s_localFrame, s_remoteFrame,
                    s_pendingEventCount - s_pendingEventIdx);
    gekko_network_poll(s_session);
    HandleSessionEvents();
    LogHeartbeat(false, "network-poll");
}

void BeginFrame(uint16_t localInput) {
    if (!s_session || s_state == State::Inactive || s_state == State::Error) {
        s_pendingEvents = nullptr;
        s_pendingEventCount = 0;
        s_pendingEventIdx = 0;
        return;
    }

    if (s_pendingEvents && s_pendingEventIdx < s_pendingEventCount) {
        LOG_NET_WARN("[Rollback] BeginFrame deferred: %d pending event(s) still queued (localInput=0x%04X)",
                     s_pendingEventCount - s_pendingEventIdx,
                     localInput);
        return;
    }

    // Feed local input to GekkoNet
    gekko_add_local_input(s_session, s_localHandle, &localInput);

    // Update session — triggers network send/receive, returns game events
    s_pendingEvents = gekko_update_session(s_session, &s_pendingEventCount);
    s_pendingEventIdx = 0;

    // Log event batch: first 120 frames always, then every 60th frame
    if (s_pendingEventCount > 0 && (s_localFrame < 120 || (s_localFrame % 60) == 0)) {
        // Count event types for summary
        int saveCount = 0, loadCount = 0, advCount = 0;
        for (int i = 0; i < s_pendingEventCount; i++) {
            if (!s_pendingEvents[i]) continue;
            switch (s_pendingEvents[i]->type) {
                case GekkoSaveEvent: saveCount++; break;
                case GekkoLoadEvent: loadCount++; break;
                case GekkoAdvanceEvent: advCount++; break;
                default: break;
            }
        }
        LOG_GEKKO_DEBUG("[Gekko][BeginFrame] frame=%d input=0x%04X events=%d (save=%d load=%d adv=%d) "
                        "state=%s ahead=%.1f",
                        s_localFrame, localInput, s_pendingEventCount,
                        saveCount, loadCount, advCount,
                        StateToString(s_state),
                        s_session ? gekko_frames_ahead(s_session) : 0.0f);
    }

    // Process session-level events (connect/disconnect/desync)
    HandleSessionEvents();

    if (s_state == State::Syncing && !s_warnedSyncFrameAdvance) {
        uint32_t simFrame = AS2_GetFrameNumber();
        if (simFrame != 0) {
            s_warnedSyncFrameAdvance = true;
            LOG_NET_WARN("[Rollback] Game sim frame advanced to %u while rollback session is still syncing", simFrame);
            AS2_LogFrameCounterState(s_localFrame, "SyncFrameAdvancedWhileSyncing");
        }
    }

    // Update remote frame estimate
    float ahead = gekko_frames_ahead(s_session);
    s_remoteFrame = s_localFrame - (int)ahead;
    LogHeartbeat(false, "begin-frame");
}

EventResult ProcessNextEvent() {
    if (!s_session || s_state == State::Inactive) {
        return EventResult::NoMoreEvents;
    }
    if (s_state == State::Error) {
        return EventResult::Error;
    }

    // Process events until we hit an Advance or exhaust the array
    while (s_pendingEvents && s_pendingEventIdx < s_pendingEventCount) {
        GekkoGameEvent* ev = s_pendingEvents[s_pendingEventIdx++];
        if (!ev) continue;

        switch (ev->type) {
        case GekkoSaveEvent:
            HandleSaveEvent(ev);
            break;
        case GekkoLoadEvent:
            HandleLoadEvent(ev);
            break;
        case GekkoAdvanceEvent:
            HandleAdvanceEvent(ev);
            return EventResult::FrameAdvance;
        default:
            break;
        }
    }

    return EventResult::NoMoreEvents;
}

bool HasPendingEvents() {
    return s_pendingEvents && s_pendingEventIdx < s_pendingEventCount;
}

bool IsActive() {
    return s_state != State::Inactive;
}

State GetState() {
    return s_state;
}

bool GetSnapshot(Snapshot* out) {
    if (!out) return false;

    out->state = s_state;
    out->local_frame = s_localFrame;
    out->remote_frame = s_remoteFrame;
    out->rollback_frames = s_rollingBack ? s_rollbackDepth : s_lastRollbackDepth;
    out->frames_ahead = s_session ? gekko_frames_ahead(s_session) : 0.0f;
    out->save_count = s_saveCount;
    out->load_count = s_loadCount;
    out->advance_count = s_advanceCount;
    out->rollback_count = s_rollbackCount;
    out->adapter_send_count = s_adapterSendCount;
    out->adapter_recv_count = s_adapterRecvCount;
    out->pending_event_count = (uint32_t)(s_pendingEventCount - s_pendingEventIdx);
    out->rolling_back = s_rollingBack;
    out->timesync_skips = s_timesyncSkipCount;
    strncpy_s(out->status, sizeof(out->status), s_status, _TRUNCATE);

    return true;
}

bool IsRollingBack() {
    return s_rollingBack;
}

int GetCurrentFrame() {
    return s_localFrame;
}

float GetFramesAhead() {
    return s_session ? gekko_frames_ahead(s_session) : 0.0f;
}

bool GetCurrentInputs(uint16_t* outP1, uint16_t* outP2) {
    if (outP1) *outP1 = s_currentP1Input;
    if (outP2) *outP2 = s_currentP2Input;
    return s_currentInputsValid;
}

void RecordTimesyncSkip() {
    s_timesyncSkipCount++;
}

bool SetDelay(int newDelay) {
    if (!s_session || s_state == State::Inactive) return false;
    if (newDelay < 0) newDelay = 0;
    if (newDelay > 15) newDelay = 15;
    gekko_set_local_delay(s_session, s_localHandle, (unsigned char)newDelay);
    LOG_NET_INFO("[Rollback] Delay changed to %d", newDelay);
    return true;
}

} // namespace RollbackSession

# re0.7 Frontend/Backend Inventory — KEEP vs REBUILD Boundary

Branch: `re0.7` @ 9f9c32d. Date: 2026-08-17.
Scope: full module inventory of `src/{net,rollback,patches,ui,input,core,replay,training,testing}` to define the boundary for the netcode backend rewrite (QOH99-style single-canonical-timeline, custom rollback engine, GekkoNet removed).

Method: full include-graph extraction across all 90 translation units + header reads of every seam module + callsite greps of every KEEP module against the doomed set.

---

## 0. Executive summary

- **GekkoNet is fully encapsulated**: `#include <gekkonet.h>` appears in exactly ONE file, `src/rollback/rollback_session.cpp`. Every other "Gekko" reference is a name (packet type `GekkoData`/`GekkoReady`, telemetry fields `gekko_avg_ping`/`gekko_jitter`) or a comment. The custom engine can be built behind the existing `RollbackSession_*` facade with near-zero frontend churn.
- **The frontend never touches the transport.** All KEEP modules reach the network exclusively through `Net::Session_SendPacket` / `Session_GetRole` / `Session_IsConnected` / `Session_GetSnapshot` (+ `PregameSync_*` lifecycle). The only exceptions are the spectator sidecar and the test harness, which use the `Transport_*ForHost` helpers to run their own ENet host.
- **Verdict on ENet: KEEP the library.** Rebuild `network_thread`/`session_manager` internals on top of it (§8).
- **Savestate/game_snapshot: KEEP — verified engine-agnostic** (§7).
- The rewrite splits into an **incremental phase** (transport/session under an unchanged `Session_*` contract) and a **big-bang core** (rollback engine + pregame/bootstrap + wiring, shipped together behind a protocol version bump) (§10).

---

## 1. Classification table (all modules)

Legend: **K** = KEEP AS-IS · **KA** = KEEP WITH ADAPTER (survives, small edits at the seam) · **R** = REBUILD (new implementation; contract may be preserved) · **D** = DELETE (folded away or removed)

### src/net

| Module | Class | Justification / dependency edges into doomed set |
|---|---|---|
| `netplay_menu_controller` | **K** | Frontend menu state machine. Calls doomed: `Session_*` (StartHost/StartJoin/Cancel/SignalReady/Update/GetSnapshot), `PregameSync_*` (Begin/Abort/GetPhase/GetSnapshot/IsActive), `OnlineWiring_On{Disconnect,Rematch,ReturnToSession}`, `RollbackSession_GetErrorReason`, `DelayPolicy_*` setters, `SyncPolicy_EnforceStageSelectForNetplay/ResetConfirmArm`, `TickHooks_IsFrameLimiter60FpsSessionOverrideActive`, `MatchLifecycle_*`. Full re-expose list in §2. |
| `netplay_menu_ui` / `netplay_menu_state` | **K** | Pure ImGui rendering over `MenuSnapshot`; only backend call is `Session_GetRole`. |
| `mode_ownership` | **K** | Game-mode/socket ownership hooks (MinHook on 0x5FB200/0x5D2EB0/vanilla socket fns). Backend edges: `Session_GetRole`, `Session_GetSnapshot`, `MatchLifecycle_IsMatchOwned`, `ContinueFlow_IsRematchLatched/ConsumeRematchLatch`. Transport-agnostic. |
| `frontend_input_sync` | **K** | The frontend lockstep core (epoch/phase/frame timeline, delay negotiation, phase barriers, boundary digests). Transport-agnostic: sends via `BarrierProtocol_SendPacket` only. Doomed edges: `DelayPolicy_GetConfiguredDelay/GetMeasurement` (delay proposal), `PregameSync_GetPhase` (phase gating). |
| `charsel_sync` | **K** | CharSel/StageSel lockstep over `FrontendInputSync`. Doomed edge: `Session_GetRole` only. |
| `stagesel_sync` | **K** | Stateless shared-cursor merge + confirm-ack gate. Zero doomed edges. |
| `winscreen_sync` | **K** | Mode 9 lockstep. Doomed edge: `Session_GetRole`. Drives `ContinueFlow_OnConsumedFrame`. |
| `continue_flow` | **K** | Continue-screen rematch prompt (0.7). Transport-agnostic by design (decisions only from consumed lockstep frames). Doomed edges: `PregameSync_BeginRematch(const LockedMatchConfig*)`, `PregameSync_GetLockedConfig()`, `Session_GetRole/IsConnected`, `OnlineWiring_OnRematch`. |
| `transition_barrier` | **K** | Wire-acked 4-way phase transitions (M4, modeled on the GekkoReady barrier). Doomed edge: `Session_SendPacket` only. Exactly the primitive the QOH99 design needs — keep verbatim. |
| `connection_supervisor` | **K** | Single liveness authority (Healthy 0-1s / Degraded 1s / Interrupted 3s / Dead 20s; heartbeat Ping every 250ms once silence ≥500ms; on Dead fires `NetMenu::HandleDisconnection` exactly once). Doomed edges: `Session_GetMsSinceLastInbound`, `Session_GetSnapshot` (reads `.active`+`.state` only), `Session_SendPacket` (reliable Ping on CHANNEL_CONTROL, deliberately bypassing barrier_protocol's classification). Its `#include "net/network_thread.h"` is **dead** — no `NetworkThread_*` symbol is called; it does NOT currently use `enet_silence_ms` (an improvement opportunity for the rewrite: the protocol-level silence signal is strictly better than app-level silence). |
| `barrier_protocol` | **K** | Reliability/channel classification (two pure `inline` header functions — keep verbatim) + `BarrierProtocol_SendPacket` funnel (a one-line wrapper over `Session_SendPacket` — the single choke point through which the whole frontend lockstep layer talks to the wire). Doomed edges: `Session_SendPacket`, `PregameSync_GetSnapshot` (reads `.active`+`.phase` for the `BarrierPhase` projection). Its `#include "net/match_bootstrap.h"` is **dead** — no `MatchBootstrap_*` symbol is called; delete it. |
| `game_settings_sync` | **K** | Round-count/frame-timing settings mirror. Doomed edge: `TickHooks_GetFrameLimiter60FpsPreferenceEnabled`. |
| `stage_watchdog_tracker` | **K** | Pure state tracker; zero doomed edges. |
| `netplay_palette_runtime` / `netplay_palette_storage` | **K** | Palette control plane (§4). Doomed edges: `Session_SendPacket/GetRole/IsConnected`, `GameplayBridge_IsSessionActive` (replace with `RollbackSession_IsActive`). |
| `match_lifecycle` | **K** | Mod-owned match phase state machine over game memory (Mode 8/9 substates). Pure classification + event sink; no transport or Gekko coupling. Its events are *called by* doomed modules — the new backend must keep calling `MatchLifecycle_OnMatchEnter/OnMatchExit/OnDisconnect/OnRematch/OnReturnToSession`. |
| `pause_handler` | **K** | Pause lockstep/quit handling. Doomed edges: `Session_SendPacket/IsConnected/Cancel`. |
| `player_side_mapping` | **K** | Host/join → P1/P2 mapping. Zero doomed edges. |
| `set_tracker` | **K** | Win/set score tracking. Doomed edge: `Session_GetSnapshot` (nicknames). |
| `nat_traversal` | **K** | UPnP/STUN/ICE/PCP orchestration (libjuice/miniupnpc/libpcpnatpmp). Transport-agnostic signaling via `NatTraversalSignal` packets; endpoint hand-off consumed by session start. Survives regardless of transport choice. |
| `churn_pause` | **KA** | Device-churn mutual pause hints (`ChurnPause` packets with session epoch). Doomed edges: `Session_SendPacket/IsConnected`, `OnlineWiring_IsGameplayActive`. Its `rollback_session.h` include is **type-only** (`RollbackTimesyncTelemetry` parameter) — zero `RollbackSession_*` call sites. Adapter: move the telemetry struct to its own header and this module decouples completely. |
| `netplay_phase_runtime` | **K** | Pure phase classification (`MatchRollbackPhase`) derived from `MatchLifecycle` + `PregameSync`. Keep; re-derive from new backend states. |
| `sync_policy` | **K** | Lockstep/Rollback/Passive classification + confirm arming + stage-select enforcement. Pure logic over game state + lifecycle. Doomed edges: `Session_*` queries, `MatchLifecycle_*`. |
| `delay_policy` | **KA** | Delay/rollback-budget policy math + measurement. Engine-agnostic except `DelayPolicy_OnRollbackApplied`/`IsRollbackSynced` (fed today from Gekko apply events) — new engine feeds the same two calls. |
| `sync_trace` | **KA** | Synchronized diagnostics traces. Reads `RollbackSession_GetSnapshot/IsActive/IsRollingBack`, `DelayPolicy_GetSnapshot`, `SyncPolicy_GetSnapshot`, `NetplayPhaseRuntime_GetPhase` — all preserved contracts. |
| `spectator_runtime/_manager/_client/_playback/_protocol` | **KA** | §6. Own sidecar ENet host; consume `Transport_*ForHost` helpers + `Session_GetRole/GetSnapshot/IsConnected`. Frame source is a push API the new engine must call. |
| `session_manager` | **R** | The session state machine + handshake. Rebuild internals (single-canonical event pump, supervisor-driven teardown); **preserve the public header contract** (§2, §9) — 36 files include it. |
| `network_thread` | **R** | ENet service worker + SPSC queues. Rebuild around the new session core; keep the stats surface `connection_supervisor` needs. |
| `enet_transport` | **KA** | Thin ENet wrapper; see §8. The `*ForHost` static helpers and autopunch/fault-injection layers are reusable as-is; the single-peer session paths get absorbed into the new transport. |
| `pregame_sync` | **R** | Bootstrap phase machine (14 phases, §3 of header) + owner of the pre-game packet callback. Rebuild per QOH99 model; **preserve the 12-function public API** (§2.2) — 10 external callers. |
| `match_bootstrap` | **R** | Config exchange / load barrier / baseline agreement / GameplayStart. Rebuild (merge into new pregame flow). Public API is only consumed by pregame_sync, barrier_protocol, online_wiring — internal to the doomed set. |
| `baseline_sync` | **KA** | Baseline CRC breakdown capture/compare/dump — pure diagnostics over `BaselineBreakdownPayload`. Only consumer is match_bootstrap; keep and re-wire into the new bootstrap. |
| `netplay_pacing` | **KA** | Tick-scale pacing (SoftHold/HardHold/StallHold, NetQuality classifier) — operates purely on `Rollback::RollbackTimesyncTelemetry` + `MatchRollbackPhase` passed by value; drives `tick_hooks` (`SetNetplayTickScaleTarget/SetNetplayPacingActive/ResetNetplayTickScaleState`). Zero `RollbackSession_*` call sites (type-only include, same as churn_pause). Keep; the new engine must populate the same telemetry struct (rename `gekko_avg_ping`/`gekko_jitter` → `link_avg_ping`/`link_jitter`). |
| `gameplay_bridge` | **D** | Thin pass-through to `RollbackSession_*` + side mapping (header says so itself). Fold into the new engine; replace the two external uses (`GameplayBridge_IsSessionActive` in netplay_palette_runtime, savestate) with `RollbackSession_IsActive`. |
| `protocol.h` / `session_types.h` | **KA** | The wire protocol and session types are the shared vocabulary. Keep; bump `PROTOCOL_VERSION`, delete `GameplayInput` (dead — comment says "no send site"), replace `GekkoData`/`GekkoReady` with the custom engine's input-stream/startup-barrier packets, keep everything else byte-stable. |
| `locked_match_config` | **K** | 20-byte deterministic match config + hash. Zero doomed edges. Central to config exchange; keep byte-stable. |

### src/rollback

| Module | Class | Justification / edges |
|---|---|---|
| `rollback_session` | **R** | The ONLY GekkoNet TU. Gekko API used: `gekko_create/destroy/start/add_actor/add_local_input/update_session/session_events/network_poll/network_stats/frames_ahead/current_frame/min_received_frame/last_received_frame/set_local_delay/net_adapter_set`. Rebuild as the custom single-canonical-timeline engine **behind the same `RollbackSession_*` facade** (§9.3): 23 files include the header; most use only `IsActive`. Drop `RollbackSession_BufferGekkoPacket` (replaced by the custom input-packet ingest). |
| `online_wiring` | **R** | The wiring hub (largest include fan-out in the repo, ~40 includes). Bootstrap→engine handoff, startup barrier, disconnect teardown, rematch handoff. Rebuild as the new "match director"; **preserve the event/query surface** (§2.3) — called by menu controller, continue_flow, churn_pause, input_override, pregame_sync. |
| `savestate` | **K** (verified) | §7. Engine-agnostic memory-region capture (match region, RNG via `_getptd()+0x14`, sim frame, input buffers). Its includes of session_manager/gameplay_bridge/spectator_playback are *guards* (gating manual save/load during netplay) — one-line adapter. Used by training, mod_menu, bootstrap baseline. |
| `game_snapshot` | **K** (verified) | Pure `GameSnapshot_Clear/Capture/Restore` over fixed address ranges from `as2_constants.h`. Zero net/Gekko edges. Used by savestate, resimulation, replay. |
| `resimulation` (+ StateHistory) | **K** | Gekko-free ring-buffer savestate history + replay executor (`StateHistory_*`, `Resim_Execute`). **replay_runtime depends on it** (StateHistory_CaptureFrame/LoadFrame/HasFrame/DiscardFramesAfter/Reset). Keep; it becomes a building block of the custom engine. |
| `input_timeline` | **R** (resurrect) | Frame-indexed local/remote input ring with confirm/predict/misprediction tracking — this *is* the mod-owned rollback core from pre-Gekko days. Currently orphaned (only `prediction.cpp` includes it). Resurrect as the canonical-timeline store of the new engine. |
| `prediction` | **R** (resurrect) | Repeat-last-input prediction policy. Currently dead (zero includers of the header). Resurrect for the new engine. |
| `frame_lineage` | **D** (absorb) | Only consumer is rollback_session.cpp. Absorb into the new engine or drop. |
| `rollback_audio` / `rollback_combo_fx` / `rollback_status_fx` | **KA** | SFX/combo/status-FX journals that keep presentation consistent across rewinds. Consume exactly 4 preserved queries each: `RollbackSession_IsActive/IsRollingBack/GetCurrentFrame/GetCurrentGameAbsFrame`. **Engine obligation**: the new engine must keep calling `RollbackAudio/ComboFx/StatusFx_OnSessionBegin(budget)` / `_OnSessionEnd(reason)` at session start/end (today invoked from rollback_session.cpp). Hook installation stays in hook_installer. |
| `rollback_debug` | **KA** | The most backend-coupled diagnostic: per-frame checksums, `StateDigest`/`FrameSyncStatus` exchange (sends via `Session_SendPacket` on CHANNEL_DEBUG), desync declaration, ImGui. Consumes 9 `RollbackSession_*` queries + `OnlineWiring_GetSnapshot` — all preserved contracts; update Gekko-named fields. |
| `desync_dump` / `determinism_verify` | **K** | State CRC/dump diagnostics over game memory; engine-agnostic (desync_dump's only doomed symbol is `RollbackSession_GetSnapshot`; determinism_verify has zero — it is consumed BY the backend for RNG-seed logging). |
| `netplay_log` / `owner_diagnostics` | **K** | Logging infra (feeds `diagnostics/async_log`). Zero doomed logic. |
| `rematch_cleanup` | **KA** | Post-match state scrub. Doomed edges: `PregameSync_Abort/GetSnapshot/IsActive`, `Session_GetSnapshot` (preserved contracts). |
| `stress_hooks` | **K** | Test-only fault injection inside the rollback path (drop/latency/jitter/forced-mispredict). Dependency is inverted: the engine queries `StressHooks_ShouldDropPacket/GetOutgoingDelayMs/AdjustDeliveryFrame/MaybeCorruptPrediction` — the new engine should keep these query points to preserve the test methodology. |
| `rematch_cleanup` (see also src/rollback row below) | — | Clarification from callsite audit: it does NOT call `RollbackSession_End` or any `Transport_*` — engine teardown is online_wiring's job; this module only scrubs match-scoped residue (aborts winscreen/pregame/charsel lockstep, clears freezes, frontend-safe 56-byte input clear guarded by CRC static_asserts). |

### src/patches

| Module | Class | Justification / edges |
|---|---|---|
| `hook_installer` | **K** | Central MinHook installer. Installs (all stay): input hooks (`Hook_KeyboardState/JoystickState/InputProcess/InputDispatcher`, DInput KB/Joy refresh, `GetKeyboardState`, `ClipCursor`, `GetProcAddress`, `SystemParametersInfoA`, `WINNLSEnableIME`, DInput SetCooperativeLevel), `tick_hooks` (sub_635F80), locale/filesystem/palette_asset/charsel hooks, `render_guard`, `session_pump_hook`, `netplay_background_run`, `shell_hotkey_patch`, `netplay_hud_vanilla`, replay hooks, rollback audio/combo/status fx hooks. None are Gekko-specific. |
| `input_override` | **KA** (netplay dispatch section **R** in place) | The `Hook_InputDispatcher` netplay branch is the game-facing pump of the doomed engine: calls `RollbackSession_BeginFrame/ProcessNextEvent/GetAdvanceInputs/HasPendingFrame/PollSession/End/FramesAhead/GetTimesyncTelemetry/IsSessionRunning/…`, `NetplayPacing_BeginFrame/OnSessionSample/OnHoldSample/ResetSession/NotifyLocalMode`, `OnlineWiring_IsStartupReleased/IsGameplayEntryAdvanceBlocked`, `PregameSync_GetPhase/IsActive`, `DelayPolicy_GetStallThreshold`, `Session_Update/Cancel/GetState/IsConnected`. Everything else in the file (SDL input, DInput bypass, buffer injection) stays untouched. The two-phase dispatcher contract (`BeginFrame` → loop `ProcessNextEvent` → `Advance`/`Done`) is a good shape for the custom engine — keep it. |
| `input_sync_hooks` | **K** | Vanilla-netplay suppression + the two freeze primitives the backend drives: `InputSyncHooks_SetLoadBarrierFreeze(bool)` / `InputSyncHooks_SetTimesyncFreeze(bool)` / `InputSyncHooks_IsModOwnedSync()`. New backend keeps calling these. |
| `session_pump_hook` | **K** | Keeps `Session_Update()` alive during blocking native asset loads (hooks `Asset_LoadFromArchive`). Only needs `Session_IsConnected` + `Session_Update` — preserved contract. |
| `tick_hooks` | **K** | Tick source hook + global/netplay tick scale + 60fps limiter correction. Generic timing infrastructure consumed by pacing, menu, replay, settings sync. Explicitly non-rollback ("rollback-specific tick warp paths removed"). |
| `charsel_palette_select` / `charsel_select_actions` | **K** | CharSel input/palette hooks; edges only to keep-side sync modules + `Session_IsConnected`, `BarrierProtocol`. |
| `palette_asset_hook` | **K** | Palette asset load intercept feeding `NetplayPaletteRuntime_OnAssetBankCaptured/OnLiveBankObserved` (+ replay/spectator override banks). |
| `netplay_background_run` | **K** | Keeps game simulating when unfocused during netplay (`Session_*` query only). |
| `render_guard`, `filesystem_patch`, `locale_patch`, `shell_hotkey_patch`, `unlock_patch`, `memory_utils` | **K** | Generic mod patches; zero doomed edges. |

### src/ui, src/input, src/core, src/replay, src/training, src/testing, src/diagnostics

| Module | Class | Edges into doomed set |
|---|---|---|
| `ui/netplay_hud`, `netplay_hud_style` | **K** | None (renders from snapshots passed in). |
| `ui/netplay_hud_vanilla` | **K** | `RollbackSession_IsActive` only. |
| `ui/mod_menu` | **K** | `RollbackSession_IsActive`, `Savestate_RenderImGui`, `TickHooks_*` (all preserved). |
| `ui/log_window`, `menu_utils`, `hitbox_display`, `hitbox_viewer`, `palette_editor` | **K** | hitbox_viewer: `RollbackSession_IsActive`. palette_editor: palette runtime only. |
| `input/input_system`, `gamepad_worker` | **K** | input_system: `churn_pause` notify only. |
| `core/mod_main` | **KA** | Composition root — includes everything; init/shutdown order and the per-frame `ModOnFrame` call sequence get re-pointed at the new backend modules. |
| `core/game_console`, `game_state`, `as2_constants` | **K** | None. |
| `replay/replay_runtime` | **K** | §6. Uses `GameSnapshot_*`, `StateHistory_*` (both KEEP), `Session_GetRole/GetSnapshot` (recording metadata), palette runtime, `tick_hooks`. **No Gekko coupling.** |
| `training/*` (practice_tools, practice_runtime, frame_advantage, input_macro, hotkey_config, action_state_classifier) | **K** | `RollbackSession_IsActive` (gating), `Savestate_LoadRoundStart/GetRoundStartInfo` — preserved contracts. |
| `testing/autoconnect_harness` | **K** | `Session_GetSnapshot`, `RollbackSession_GetSnapshot` — preserved. |
| `testing/rematch_soak` | **KA** | `PregameSync_Begin/GetPhase/GetSnapshot`, `Session_GetSnapshot`, `Transport_FaultInjectionActive` — all preserved; drop its `enet_transport` include if the fault-injection query moves. |
| `testing/scripted_input_runner`, `test_scenarios`, `harness_shared_memory` | **K** | Input-system only. |
| `diagnostics/async_log` | **K** | None. |

### Third-party

| Component | Class | Note |
|---|---|---|
| GekkoNet (`lib/GekkoNet`) | **D** | Removed from CMake (`GEKKONET_DIR`), link line, and `rollback_session.cpp`. |
| ENet (`lib/enet`) | **K** | §8. |
| libjuice / miniupnpc / libpcpnatpmp | **K** | NAT traversal stack is transport-agnostic (feeds endpoints/candidates to session start). |

---

## 2. API surface the new backend MUST re-expose (exact signatures)

These are the signatures currently consumed by KEEP modules, verbatim from the headers. The new backend must provide them (same names or via a shim header) for the frontend to keep compiling.

### 2.1 `Net::Session_*` (from `include/net/session_manager.h`) — 36 including TUs

Consumed by: menu controller/ui, mode_ownership, all lockstep sync modules, continue_flow, connection_supervisor, transition_barrier, barrier_protocol, palette runtime, pause_handler, set_tracker, spectator, replay, rematch_cleanup, input_override, input_sync_hooks, session_pump_hook, netplay_background_run, charsel_palette_select, testing.

```cpp
void Session_Init();
void Session_Shutdown();
bool Session_StartHost(const SessionConfig* config);
bool Session_StartJoin(const SessionConfig* config);
void Session_Cancel();
void Session_NotifyGameExit();
uint32_t Session_GetMsSinceLastInbound();          // connection_supervisor
void Session_SignalReady();
void Session_Update();                              // menu ctrl, input_override, session_pump_hook
bool Session_SendPacket(uint8_t channel, PacketType type,
                        const void* payload, size_t payloadLen, bool reliable);
typedef void (*PacketCallback)(PacketType type, const void* payload, size_t payloadLen);
void Session_SetPacketCallback(PacketCallback cb);  // incl. deferred-flush semantics
void Session_GetSnapshot(SessionSnapshot* out);
SessionState Session_GetState();
SessionRole  Session_GetRole();
bool Session_IsConnected();
const PeerInfo* Session_GetRemotePeer();
void Session_GetStats(ConnectionStats* out);
```
Plus the types in `session_types.h` (`SessionState`, `SessionRole`, `SessionConfig`, `PeerInfo`, `ConnectionStats`, `NatTraversalConfig`, `ConnectPreference`) and `SessionSnapshot` — all byte/shape-stable.

### 2.2 `Net::PregameSync_*` (from `include/net/pregame_sync.h`) — external callers: menu controller, continue_flow, frontend_input_sync, rematch_cleanup, barrier_protocol, netplay_phase_runtime, input_override, online_wiring, rematch_soak

```cpp
void PregameSync_Init();
void PregameSync_Shutdown();
void PregameSync_FrameUpdate();
bool PregameSync_Begin();
bool PregameSync_BeginRematch(const LockedMatchConfig* previousConfig);  // continue_flow fast path
void PregameSync_Abort(const char* reason);
PregamePhase PregameSync_GetPhase();
bool PregameSync_IsActive();
void PregameSync_GetSnapshot(PregameSnapshot* out);
const LockedMatchConfig* PregameSync_GetLockedConfig();
bool PregameSync_IsComplete();
bool PregameSync_HandleCrossPhaseSessionPacket(PacketType type, const void* payload, size_t payloadLen);
```
`PregamePhase` enum and `PregameSnapshot` struct are consumed by the menu HUD — keep names (internal phase machine may change; the enum is the reporting vocabulary).

### 2.3 `Rollback::OnlineWiring_*` (from `include/rollback/online_wiring.h`) — external callers: menu controller, continue_flow, churn_pause, input_override, pregame_sync, mod_main, rollback_debug

```cpp
void OnlineWiring_Init();
void OnlineWiring_Shutdown();
void OnlineWiring_FrameUpdate();
bool OnlineWiring_IsGameplayActive();               // churn_pause
bool OnlineWiring_IsStartupReleased();              // input_override
bool OnlineWiring_IsGameplayEntryAdvanceBlocked();  // input_override
void OnlineWiring_OnGameplayStart();
void OnlineWiring_OnGameplayPause(const char* reason);
void OnlineWiring_OnMatchEnd();
void OnlineWiring_OnDisconnect(const char* reason); // menu controller
void OnlineWiring_OnRematch();                      // menu controller, continue_flow
void OnlineWiring_OnReturnToSession();              // menu controller
void OnlineWiring_GetSnapshot(OnlineWiringSnapshot* out);
```

### 2.4 `Rollback::RollbackSession_*` (from `include/rollback/rollback_session.h`) — the custom engine's facade

Minimum external surface (23 including TUs; the dispatcher set is used only by `input_override.cpp`):

```cpp
// Broad KEEP-side usage (HUD, menus, training, diagnostics):
bool RollbackSession_IsActive();
const char* RollbackSession_GetErrorReason();       // menu controller
void RollbackSession_GetSnapshot(RollbackSessionSnapshot* out);  // sync_trace, autoconnect_harness, rollback_debug
bool RollbackSession_IsRollingBack();               // sync_trace, fx modules
bool RollbackSession_ShouldSuppressSideEffects();   // rollback_audio/combo_fx/status_fx

// Dispatcher contract (input_override netplay branch — keep the two-phase shape):
void RollbackSession_BeginFrame(uint16_t localInput);
EventResult RollbackSession_ProcessNextEvent();     // Advance | Done | Error
void RollbackSession_GetAdvanceInputs(uint16_t* p1, uint16_t* p2);
bool RollbackSession_HasPendingFrame();
bool RollbackSession_PollSession();
void RollbackSession_End();
bool RollbackSession_IsSessionRunning();
float RollbackSession_FramesAhead();
int32_t RollbackSession_GetCurrentFrame();
int  RollbackSession_GetRollbackBudget();
void RollbackSession_GetTimesyncTelemetry(RollbackTimesyncTelemetry* out);  // netplay_pacing input
int32_t RollbackSession_GetFrameOriginAbs();
int32_t RollbackSession_GetCurrentGameAbsFrame();
uint32_t RollbackSession_ComputeLiveStateChecksum();
```
Deleted from the facade: `RollbackSession_BufferGekkoPacket` (Gekko ingest — the new engine owns its own input-packet handler via the session packet callback). `RollbackTimesyncTelemetry` keeps its shape; rename `gekko_avg_ping`/`gekko_jitter`.

### 2.5 Policy/lifecycle contracts consumed by the frontend (all KEEP modules — listed for completeness)

- `DelayPolicy_*`: `SetConfiguredDelay/SetRollbackBudget/SetRollbackToleranceK/SetGameplayDelayMode/GetActiveDelay/GetSnapshot/ComputeRecommendedMaxRollback` (menu), `GetConfiguredDelay/GetMeasurement` (frontend_input_sync), `GetStallThreshold` (input_override), `BuildNegotiationData/NegotiateSession` (bootstrap-side), `OnRollbackApplied/IsRollbackSynced` (engine feeds).
- `SyncPolicy_*`: `EnforceStageSelectForNetplay/ResetConfirmArm` (menu), `GetSnapshot/GetCurrentMode/GetLockstepContext/FeedConfirmInput/IsConfirmArmed` (lockstep + trace).
- `MatchLifecycle_*`: full header (§1) — the new backend calls the `On*` events; frontend reads phase/queries.
- `NetplayPhaseRuntime_GetPhase/Is*Phase` — derived classification; keep.
- `TickHooks`: `SetNetplayTickScale{,Target}/SetNetplayPacingActive/ResetNetplayTickScaleState` (pacing writes), limiter-override getters (menu/settings).
- `InputSyncHooks_SetLoadBarrierFreeze/SetTimesyncFreeze/IsModOwnedSync` — backend-driven freeze primitives.
- `TransitionBarrier_Propose/IsCommitted/RemoteProposed/GetRemoteIntent/ConsumeCommit/OnPacket/FrameUpdate/Reset` — the new session lifecycle must route `PhaseTransitionProposal/Ack` packets into `TransitionBarrier_OnPacket` and keep `FrameUpdate` in the pump.
- `ConnectionSupervisor_*` — the new backend consumes the verdict (`GetHealth/IsInterrupted/IsDead`) instead of private timers, and calls `OnSessionStart/OnSessionEnd`.

---

## 3. Frontend lockstep — required transport/session primitives

`frontend_input_sync` + charsel/stagesel/winscreen sync + continue_flow are pure logic over these primitives (all already abstract):

1. **Typed reliable/unreliable send to the single peer**: `Session_SendPacket(channel, type, payload, len, reliable)` — routed through `BarrierProtocol_SendPacket` which picks channel/reliability per `PacketType`.
2. **Inbound dispatch**: a packet callback (`Session_SetPacketCallback`) that routes `CharSelFrameInput`, `WinScreenFrameInput`, `CharSelLock`, `StageSync`, `FrontendPhaseBarrier`, `FrontendBoundaryDigest`, `DelayChangeReq/Ack`, `PhaseTransitionProposal/Ack`, `ResyncRequest` to the keep-side handlers (`FrontendInputSync_OnRemote*`, `CharSelSync_OnRemote*`, `WinScreenSync_OnRemote*`, `TransitionBarrier_OnPacket`). Today pregame_sync owns this callback — the new backend keeps a single dispatch owner with deferred-flush semantics (reliable packets arriving before a handler is installed are queued, not dropped).
3. **Identity/role**: `Session_GetRole()` (host=P1 mapping), `Session_IsConnected()`, epoch/session id from pregame.
4. **Liveness verdict** (not raw timers): `ConnectionSupervisor_GetHealth()`.
5. **RTT measurement** for the delay proposal: `DelayPolicy_GetMeasurement` fed from transport stats.
6. Wire payloads in `protocol.h` (packet types 8, 9, 11–13, 21–22, 40–43, 60–62) must remain available with byte-stable payload structs.

`continue_flow` additionally needs the rematch fast path: `PregameSync_BeginRematch(prevConfig)` (skip charsel/stagesel, host mints fresh seeds, straight to ConfigExchange) and `WinScreenSync_FinalizeFromContinueFlow`.

## 4. Palette pipeline — hooks into config exchange

Keep whole. Two independent exchanges; the new backend must preserve these integration points:

- **CharSel catalog (pre-lock)**: `charsel_palette_select` sends `CharSelInput(11)` (`CharSelInputPayload.custom_masks[256]` — bit N = stored custom bank exists for base palette N) via `BarrierProtocol_SendPacket`; inbound routed by the pregame callback → `CharSelPaletteSelect_OnRemoteCatalog`. The per-selection custom flag rides `CharSelLockPayload.flags & CHARSEL_LOCK_FLAG_CUSTOM_PALETTE`.
- **Bank exchange (post-config-lock)**: `netplay_palette_runtime` sends `PaletteConfig(50)/PaletteData(51)/PaletteAck(52)` via `Session_SendPacket` directly, CHANNEL_CONTROL, reliable, with a 500ms resend pump until remote ack. A full 1024-byte bank fits one packet (static_assert-pinned). **Critical routing requirement**: these three types are dispatched by BOTH the pregame packet callback AND the in-match gameplay callback today — the new backend's dispatcher must deliver them in both regimes, race-tolerant across the callback handoff.
- **Config lock event**: at ConfigAgreed the backend calls `NetplayPaletteRuntime_OnLockedMatchConfig(const LockedMatchConfig*)` — computes `config_hash` (gates every Palette packet), derives slot via `PlayerMapping_DeriveFromRole(host_side, isHost)`, bumps epoch.
- **Lifecycle events**: `OnRoundRestart / OnWinScreenEnter / OnMatchEnd(reason) / OnDisconnect(reason)` (called today from online_wiring/pregame at 6+ sites) plus `CharSelPaletteSelect_ResetNetplaySessionState(reason)` on rematch teardown. `NetplayPaletteRuntime_OnDisconnect` also calls `NetplayPaletteStorage_ClearCache()` — must keep happening between sessions.
- **Gating queries**: `GameplayBridge_IsSessionActive()` (→ becomes `RollbackSession_IsActive()`), `MatchLifecycle_GetPhase()` (IntroActive window), `Session_IsConnected/GetRole`, `PregameSync_GetLockedConfig()`.
- **Asset side**: `palette_asset_hook` → `OnAssetBankCaptured/OnLiveBankObserved/ConsumeLiveReloadRequest/OnLiveReloadComplete`; override precedence chain replay > spectator > netplay (zero backend symbols).
- **Spectator propagation**: `NetplayPaletteRuntime_CopySpectatorBank` feeds the sidecar `PaletteState/PaletteData` spectator packets.

## 5. Recently added resilience modules — KEEP, confirmed transport-agnostic

- `connection_supervisor` — only consumes `Session_GetMsSinceLastInbound`, `Session_GetSnapshot`, `Session_SendPacket`, plus `NetworkThread_GetStats().enet_silence_ms`. Requirement on the new transport: publish a **protocol-level inbound silence** metric (acks/pings count, not just app packets).
- `transition_barrier` — pure proposal/ack state machine over `Session_SendPacket`; explicitly modeled on the one barrier that never failed in the field. Becomes a first-class primitive of the new session lifecycle.
- `continue_flow` — decisions computed exclusively from consumed lockstep frames (no wall clocks, no out-of-band messages); survives any transport.

## 6. Spectator + replay survival requirements

**Spectator** (all KA): bypasses session_manager entirely — `spectator_manager` creates its own multi-peer ENet server host (8 peers, 2 channels) with raw `enet_*` calls; `spectator_client` creates a client host plus an optional downstream relay host; `spectator_playback` has **zero** backend symbols (drives the vanilla game through charsel from `LockedMatchConfig`, no state snapshots ever cross the wire). The gameplay-session symbols consumed are only `Session_IsConnected/GetRole/GetSnapshot` (host-only serving gate, listen port to advertise, P1/P2 nicknames) plus the host-parameterized transport helpers — these must survive in whatever file owns ENet:

```cpp
bool Transport_GlobalInit();
bool Transport_GetHostBoundPort(ENetHost* host, uint16_t* outPort);
bool Transport_SendHolePunchBurstForHost(ENetHost*, const char* host, uint16_t port, int burstCount, uint32_t intervalMs);
void Transport_AutopunchStartForHost(ENetHost*, const char* logLabel, const char* relayHost, uint16_t relayPort, uint16_t localPort, const char* targetHost, uint16_t targetPort);
void Transport_AutopunchStopForHost(ENetHost*, const char* reason);
void Transport_AutopunchServiceForHost(ENetHost*, uint32_t nowMs, bool peerConnected);
```

Frame/data sources are **push APIs the new engine must call** (today called from rollback_session/online_wiring/pregame):

```cpp
void SpectatorRuntime_OnSelectionCommitted(const LockedMatchConfig* config); // at chars+stage locked, pre-loading
void SpectatorRuntime_OnMatchBegin(const LockedMatchConfig* config);
void SpectatorRuntime_OnRollbackStarted(int32_t frame_origin_abs);
void SpectatorRuntime_OnGameplayFrame(int32_t rb_frame, int32_t game_abs_frame,
                                      uint16_t p1_input, uint16_t p2_input,
                                      bool rolling_back, int32_t confirmed_rb_frame);
void SpectatorRuntime_OnMatchEnd(const char* reason);
void SpectatorRuntime_OnDisconnect(const char* reason);
```
i.e. the new backend must provide: **per-frame canonical input pairs with a confirmed-frame watermark and rollback-rewrite flagging** (`FRAME_FLAG_CONFIRMED` / `FRAME_FLAG_ROLLBACK_REWRITE` in the sidecar protocol), plus the locked config and frame origin. Today `OnGameplayFrame` is called from inside rollback_session's Gekko advance-event handler, with the watermark sourced from `gekko_min_received_frame()`; the new engine must expose an equivalent monotone "all inputs ≤ N are final" watermark and emit the callback on every simulated frame including resim rewrites (archive slots are overwritten in place). A single-canonical-timeline engine makes this *simpler*: confirmed frames are final by construction, so rewrites disappear. The match-identity formula `match_id = session_seed ^ (LockedMatchConfig_Hash(config) << 1)` must stay stable (correlates PreMatchState with MatchState packets).

**Replay** (K): records/plays via `GameSnapshot_*` + `StateHistory_*` (`CaptureFrame/LoadFrame/HasFrame/DiscardFramesAfter/Reset`) — both KEEP and Gekko-free. Only session touch is metadata (`Session_GetRole/GetSnapshot` for nicknames). No changes required beyond keeping StateHistory alive (do NOT let the new engine privatize it).

## 7. Savestate / game_snapshot verdict — KEEP (engine-agnostic, verified)

- `game_snapshot.h/.cpp`: raw capture/restore of fixed memory regions (`ADDR_MATCH_BASE`…P2 entity end, pre-match gap, input buffers, RNG seed, sim frame, mode/substate) — zero net/Gekko includes. **KEEP AS-IS.**
- `savestate.h/.cpp`: slot management (manual F5/F6, round-start autosave, netplay baseline slot: `Savestate_CaptureRollbackBaseline/RestoreRollbackBaseline/ClearRollbackBaseline`) on top of game_snapshot. Its `session_manager`/`gameplay_bridge`/`spectator_playback` includes are only netplay-active *guards* on manual save/load — replace with `RollbackSession_IsActive()`/`Session_IsConnected()`. **KEEP WITH one-line ADAPTER.** The baseline slot API is exactly what the new bootstrap needs.
- `resimulation`'s StateHistory ring is the per-frame variant used for rollback — also engine-agnostic and already the replay scrub-buffer. The custom engine should reuse it rather than reimplement.

## 8. ENet vs custom UDP — **KEEP ENet**

Features actually in use (from `enet_transport.h/.cpp` + `network_thread`):
- 3 channels: reliable-ordered control (0), unreliable-sequenced gameplay (1), unreliable debug (2);
- built-in RTT/variance/loss per peer (feeds ConnectionStats, delay recommendations);
- connect/disconnect events with data words; peer timeout/ping/throttle tuning (`Transport_ConfigurePeerResilience`);
- coexisting raw-UDP layers on the same socket: hole-punch bursts + autopunch relay keepalive (heals mid-match NAT rebinds);
- egress fault injection for resilience testing;
- a second independent host for the spectator sidecar.

A custom UDP stack would have to reimplement the reliable control channel (every barrier, config exchange, palette transfer, and transition rides it), the sequencing, the stats, and the NAT-keepalive socket sharing — none of which is implicated in the problems motivating this rewrite. The latency-critical path (gameplay inputs) already uses unreliable-sequenced sends with app-level redundancy (16-frame input history per packet), which is the QOH99 pattern. **The problem was GekkoNet owning the timeline, not ENet owning the wire.** Decision: keep ENet; rebuild `network_thread` (queues/eventing) and absorb the single-peer parts of `enet_transport` into it; preserve the `*ForHost` helpers for the spectator sidecar.

## 9. New backend module plan (replacements)

| Doomed module | Replacement | Contract |
|---|---|---|
| `enet_transport` (single-peer paths) + `network_thread` | `net/transport2` (worker thread, ENet, canonical event queue, protocol-silence metric) | internal; `NetworkThreadStats`-equivalent for supervisor |
| `session_manager` | `net/session2` | **`Session_*` header preserved verbatim** |
| `pregame_sync` + `match_bootstrap` | `net/match_setup` (single phase machine: announce → frontend handoff → config → load → baseline → start, every cross-peer transition via `TransitionBarrier`) | **`PregameSync_*` API preserved**; `MatchBootstrap_*` retired (internal-only) |
| `rollback_session` (Gekko) + `gameplay_bridge` + `frame_lineage` | `rollback/engine2` — single canonical timeline: `input_timeline` (resurrected) + `prediction` (resurrected) + `StateHistory`/`Resim_Execute` + savestate baseline; input packets = redundant-history datagrams on channel 1 (like `CharSelFrameInputPayload`); startup barrier via `TransitionBarrier` (replacing `GekkoReady`) | **`RollbackSession_*` facade preserved** (§2.4) |
| `online_wiring` | `rollback/match_director` (thin: lifecycle events ↔ engine, teardown, startup barrier) | **`OnlineWiring_*` surface preserved** (§2.3) |
| `delay_policy`, `sync_policy`, `netplay_pacing`, `netplay_phase_runtime`, `churn_pause`, `baseline_sync` | kept, re-fed by engine2 | unchanged headers (telemetry field renames only) |
| `input_override` dispatch branch, `tick_hooks`, `session_pump_hook`, `input_sync_hooks` | kept; dispatch branch rewired to engine2 through the same two-phase contract | unchanged |

## 10. Dependency-ordered rebuild sequence

**Incremental (frontend keeps compiling and running throughout):**
0. **Zero-risk pre-refactors** (each independently landable, no behavior change):
   - Extract `RollbackTimesyncTelemetry` into `include/rollback/rollback_telemetry.h` and rename `gekko_avg_ping/gekko_jitter` → `link_avg_ping/link_jitter` — this alone fully decouples `churn_pause` and `netplay_pacing` (both are type-only includers of rollback_session.h).
   - Extract online_wiring's `OnGameplayPacket` (~300 lines, 22 packet cases of which only 3 touch the engine) into a standalone `gameplay_packet_router.cpp`.
   - Delete the dead includes: `network_thread.h` from connection_supervisor.cpp, `match_bootstrap.h` from barrier_protocol.cpp.
   - Fix the inverted dependency: `frontend_input_sync` gates WinScreen sends on `PregameSync_GetPhase()` — inject that predicate instead of reaching up into pregame.
1. **Facade freeze**: commit to the preserved contracts (§2). Rename packet types (`GekkoData`→`InputStream`, `GekkoReady`→retired in favor of TransitionBarrier `GameplayStart` kind); delete dead wire symbols (`GameplayInput` — no send site; `SessionMeta` — no sender/handler; `ResyncRequest` — declared only; `WinScreenConfirm` — receive-only legacy, no sender remains); bump `PROTOCOL_VERSION`. Pure rename pass, no behavior change.
2. **Transport/session swap**: implement `transport2` + `session2` behind the unchanged `Session_*` header; delete `network_thread`, absorb `enet_transport` single-peer paths, keep `*ForHost` helpers. Verifiable in isolation (menu connect/handshake/supervisor/NAT all exercise it without any rollback code).
3. **Engine bring-up offline**: build `engine2` on `input_timeline`+`prediction`+`StateHistory` and validate against `determinism_verify`/`desync_dump` in local two-instance harness (`autoconnect_harness`, fault injection) while Gekko path still ships.
   - 3a. `savestate` guard adapter + `GameplayBridge_IsSessionActive`→`RollbackSession_IsActive` in netplay_palette_runtime (2-line edits, can land any time).

**Big-bang core (one release, protocol-incompatible with 0.6 by design):**
4. **`match_setup`** replaces pregame_sync+match_bootstrap (all cross-peer transitions through TransitionBarrier; palette + spectator + lifecycle callbacks re-wired per §4/§6).
5. **`match_director` + engine2 cutover**: replace online_wiring; rewire `input_override`'s dispatcher branch; pacing/delay policy fed from engine2 telemetry; remove GekkoNet from CMake and delete `rollback_session.cpp`'s Gekko internals.
6. **Spectator/replay re-hookup**: engine2 emits `SpectatorRuntime_On*` (simpler: confirmed-only stream, no rewrite flags); StateHistory stays public for replay.
7. **Soak**: `rematch_soak`, `RESILIENCE_TESTING` fault matrix, spectator + continue-flow rematch end-to-end.

Steps 4–5 must ship together (pregame hands the baseline to the engine; the barrier kinds span both). Everything else is independently landable.

---

## 11. Deleted-symbol checklist (things the frontend must NOT reference after the rewrite)

`RollbackSession_BufferGekkoPacket`, `GameplayBridge_*` (all), `PacketType::GameplayInput` (already dead), `PacketType::GekkoData`, `PacketType::GekkoReady` (+ `GekkoReadyPayload`, `GEKKO_READY_FLAG_*`), `RollbackTimesyncTelemetry.gekko_*` / `RollbackSessionSnapshot.gekko_*` field names, `MatchBootstrap_*` external uses (barrier_protocol's phase query → `match_setup` equivalent), `NetworkThread_*` (supervisor's stats query → transport2 equivalent). Current external references: gameplay_bridge (2 call sites), NetworkThread_GetStats (1), MatchBootstrap via barrier_protocol (1) — total adapter cost outside the doomed set is ~6 lines.

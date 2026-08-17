# re0.7 API Freeze Checklist (M1)

Date: 2026-08-17. Branch: `re0.7`.

This is the M1 "facade freeze" commitment from `RE07_MASTER_REBUILD_PLAN.md` §6:
the contracts below are **preserved verbatim** through the whole rebuild. Any
change to a listed signature, struct shape, or enum value is a rejected review
until this document is amended first. Source of truth for the lists:
`FRONTEND_BACKEND_INVENTORY.md` §2.1–2.5.

Compile-time pins added at M1:

| Pin | Location |
|---|---|
| `sizeof(LockedMatchConfig) == 20` | `include/net/locked_match_config.h` (pre-existing) |
| `sizeof(SessionSnapshot) == 472`, `sizeof(PeerInfo) == 96`, `sizeof(ConnectionStats) == 40` | `include/net/session_manager.h` |
| `sizeof(PaletteConfigPayload) == 20`, `sizeof(PaletteDataPayload) == 1044`, `sizeof(PaletteAckPayload) == 16` | `include/net/protocol.h` |
| Frontend frame/barrier/digest payload sizes (56/56/20/36) | `include/net/protocol.h` |
| All new v2 payload sizes (§3.2) | `include/net/protocol.h` |

## 1. `Net::Session_*` (include/net/session_manager.h) — preserved verbatim

All 17 functions, byte/shape-stable types (`SessionState`, `SessionRole`,
`SessionConfig`, `PeerInfo`, `ConnectionStats`, `NatTraversalConfig`,
`ConnectPreference`, `SessionSnapshot`):

- [ ] `Session_Init` / `Session_Shutdown`
- [ ] `Session_StartHost(const SessionConfig*)` / `Session_StartJoin(const SessionConfig*)`
- [ ] `Session_Cancel` / `Session_NotifyGameExit`
- [ ] `Session_GetMsSinceLastInbound` (re-implemented over `protocol_silence_ms` at M3; header contract unchanged)
- [ ] `Session_SignalReady` / `Session_Update`
- [ ] `Session_SendPacket(channel, type, payload, len, reliable)`
- [ ] `Session_SetPacketCallback(PacketCallback)` incl. deferred-flush semantics
- [ ] `Session_GetSnapshot` / `Session_GetState` / `Session_GetRole`
- [ ] `Session_IsConnected` / `Session_GetRemotePeer` / `Session_GetStats`

## 2. `Net::PregameSync_*` (include/net/pregame_sync.h) — preserved

12 functions; `PregamePhase` enum values kept as the *reporting vocabulary*
(internal states of match_setup may differ); `PregameSnapshot` shape kept:

- [ ] `PregameSync_Init` / `Shutdown` / `FrameUpdate`
- [ ] `PregameSync_Begin` / `PregameSync_BeginRematch(const LockedMatchConfig*)`
- [ ] `PregameSync_Abort(const char*)`
- [ ] `PregameSync_GetPhase` / `IsActive` / `GetSnapshot` / `IsComplete`
- [ ] `PregameSync_GetLockedConfig`
- [ ] `PregameSync_HandleCrossPhaseSessionPacket`

`MatchBootstrap_*` is NOT frozen — internal to the doomed set, retired at M5.

## 3. `Rollback::OnlineWiring_*` (include/rollback/online_wiring.h) — preserved

All 13 functions + `OnlineWiringSnapshot`:

- [ ] `OnlineWiring_Init` / `Shutdown` / `FrameUpdate`
- [ ] `OnlineWiring_IsGameplayActive` / `IsStartupReleased` / `IsGameplayEntryAdvanceBlocked`
- [ ] `OnlineWiring_OnGameplayStart` / `OnGameplayPause` / `OnMatchEnd`
- [ ] `OnlineWiring_OnDisconnect` / `OnRematch` / `OnReturnToSession`
- [ ] `OnlineWiring_GetSnapshot`

M0 additions (packet sinks used by `gameplay_packet_router`, replaced at M5 by
packet_router→engine ingest — additions are allowed, removals are not):
`OnlineWiring_HandleEngineDataPacket`, `OnlineWiring_HandleStartupBarrierPacket`.

M6: provider is now `src/rollback/match_director.cpp` (online_wiring.cpp
deleted; header/facade unchanged). Additive M6 API (INV-9 match-end ladder):
`OnlineWiring_MatchEndLadderAllows(kind)` /
`OnlineWiring_MatchEndLadderNotifyConsumed(kind)`.
`OnlineWiringSnapshot` shape unchanged (`target/current_tick_scale` report
the scheduler speed scale; `stall_threshold` reports 0 — retired knob).

## 4. `Rollback::RollbackSession_*` (include/rollback/rollback_session.h) — preserved facade

- [ ] `RollbackSession_IsActive` / `IsSessionRunning` / `IsPeerInterrupted`
- [ ] `RollbackSession_GetErrorReason` / `TakeErrorReason` / `GetSnapshot`
- [ ] `RollbackSession_IsRollingBack` / `ShouldSuppressSideEffects`
- [ ] Two-phase dispatcher contract: `BeginFrame(uint16_t)` → loop
      `ProcessNextEvent()` (`Advance | Done | Error`) → `GetAdvanceInputs(p1,p2)`;
      `HasPendingFrame` / `PollSession` / `DrainPendingNonAdvanceEvents` / `End`
- [ ] `RollbackSession_FramesAhead` (telemetry only in engine2 — no decision consumes it)
- [ ] `RollbackSession_GetCurrentFrame` / `GetFrameOriginAbs` / `GetCurrentGameAbsFrame` / `RbFrameToGameAbs`
- [ ] `RollbackSession_GetActiveDelay` / `SetLocalDelay` / `GetRollbackBudget`
- [ ] `RollbackSession_GetTimesyncTelemetry(RollbackTimesyncTelemetry*)`
      (struct extracted to `rollback/rollback_telemetry.h` at M0;
      `gekko_avg_ping`/`gekko_jitter` → `link_avg_ping`/`link_jitter`)
- [ ] `RollbackSession_ComputeLiveStateChecksum` / `InjectLocalInput`

Done at M5: `RollbackSession_BufferGekkoPacket` renamed
`RollbackSession_OnInputStreamPacket` (both adapters);
`RollbackSession_OnSyncHashPacket` added (engine2 ingest; Gekko no-op);
`RollbackSessionSnapshot.gekko_*` field names renamed `link_*`.

Done at M6 (additive, both adapters): `RollbackSession_SuspendBetweenMatches`
(engine2: engine stays armed across the match boundary, next Begin under a
higher epoch ROTATES; Gekko: alias to End) and
`RollbackSession_SetMatchExitPending` (engine2 §2.7.6 exact-window signal;
Gekko no-op). `RollbackSessionSnapshot` gains peer advisory readouts
(`peer_produced_frontier`, `peer_prediction_depth`, `peer_run_state`,
`peer_adv_delay`, `peer_adv_rollback` — zeros on Gekko).

## 5. Policy/lifecycle contracts consumed by the frontend — preserved

- [ ] `DelayPolicy_*` per inventory §2.5 (incl. `OnRollbackApplied`/`IsRollbackSynced` fed by engine2). Done at M6: `DelayPolicy_GetStallThreshold`/`GetProtectionWindow` DELETED with the input_override rewrite (§2.8.7); snapshot fields report 0; additive `DelayPolicy_ClassifyLocalCoverage` (§2.9.1) + measurement re-fed from `net/time_probe` (new module, packets 77/78).
- [ ] `SyncPolicy_*`, `MatchLifecycle_*` (full header), `NetplayPhaseRuntime_*`
- [ ] `InputSyncHooks_SetLoadBarrierFreeze` / `SetTimesyncFreeze` / `IsModOwnedSync`
- [ ] `TransitionBarrier_*` full surface; `PhaseTransitionProposal/Ack` routed by every dispatch owner
- [ ] `ConnectionSupervisor_*` (verdict consumed instead of private timers)
- [ ] Spectator: `Transport_*ForHost` helper set + `SpectatorRuntime_On*` push API (inventory §6)
- [ ] `StateHistory_*` stays public (replay depends on it — do NOT privatize in engine2)
- [ ] `BarrierProtocol_IsReliable` / `GetChannel` / `SendPacket`
- [ ] Wire payloads byte-stable: `LockedMatchConfig` (20 B), palette 50–52, NAT signaling, ChurnPause, Ping, SyncTrace

## 6. Wire symbols — v20 state

Deleted at M1 (were dead): `SessionMeta`, `GameplayInput`, `WinScreenConfirm`.
Renamed at M1: `GekkoData` → `InputStream` (same id 23; carries raw Gekko bytes
until M5, then the v2 `InputStreamPayload` schema).
Deleted at M3 (session2 cutover): `Hello` (1), `HelloAck` (2) — superseded by
the live 5-step nonce handshake; their side data (nickname/round/timing/HUD
style) moved to `PeerIdentity` (79, new at M3, sent post-handshake).
Deleted at M5: `DelayChangeReq` (21) / `DelayChangeAck` (22) + payloads
(INV-23 — the frontend delay is locally derived, no negotiation flow);
`GekkoReady` (24) + payload + flags (the startup gameplay-entry barrier rides
`TransitionBarrier` kind `GameplayStart`); the `SyncAnnounce`/`SyncConfirm`
delay fields (byte positions kept as `_retired*` pads, sizes 8/12 stable).
Changed at M5 (§3.4 acceptance cutover): the frontend `phase_serial` fields
became `_retired_serial` pads (sent 0, never read); acceptance is keyed on
`(epoch, phase_id)`; `ResyncRequest` (62) / `ResyncReply` (63) are live
(INV-11 interrogation); `NetTransitionKind::EpochAlign` payload fields are
live (host-minted epoch authority, match_setup).
Live from M3: `SessionHello/Offer/Ack/Confirm/ConfirmAck` (the handshake),
`PeerIdentity`; `DisconnectReason::Busy` (5) as ENet disconnect data.
Live from M5 on the engine2 configuration: `SyncHash` (75) with the router
session_id gate. Defined, still unsent: `SyncHashAck`,
`TimeProbe/TimeProbeAck` (time_probe lands at M6).
Changed at M4 (§3.2 terminal shape, INV-20): `DisconnectPayload` is now
`{code u8, reason_id u32 (fnv1a32 of the typed reason name), human[96]}`
(102 B pin); session2 fault terminals resend it at 100 ms across the bounded
goodbye window. Wire break is legal (v20 is dev-only until M6).

/**
 * Alice Senki 2 - SyncTrace diagnostics
 *
 * Full CSV/network trace requires AS2_SYNC_TRACE=1.
 * Optional rollback integrity compare requires AS2_SYNC_TRACE_INTEGRITY=1.
 * Authoritative drift detection uses StateDigest during normal rollback.
 */

#pragma once

#include "net/frontend_input_sync.h"
#include "net/protocol.h"

#include <stdint.h>

namespace Net {

void SyncTrace_Init();
void SyncTrace_Shutdown();
void SyncTrace_SetEnabled(bool enabled, const char* reason);
bool SyncTrace_IsEnabled();
void SyncTrace_SetIntegrityActive(bool active, const char* reason);
bool SyncTrace_IsIntegrityActive();
bool SyncTrace_ShouldArmIntegrityOnRollback();
bool SyncTrace_IsCompareActive();
void SyncTrace_ResetSession(const char* reason);

void SyncTrace_FrameUpdate();
void SyncTrace_OnFrontendFrameConsumed(uint32_t epochId,
                                       FrontendSyncPhase phase,
                                       uint32_t frame,
                                       uint16_t localInput,
                                       uint16_t remoteInput);
void SyncTrace_OnLifecycleTransition(const char* reason);
void SyncTrace_OnRemoteTrace(const SyncTracePayload* payload);

} // namespace Net

/**
 * Alice Senki 2 - Rollback Interface
 * 
 * Abstract interface for rollback netcode implementation.
 * This allows swapping out different rollback implementations
 * without changing the rest of the mod code.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Rollback connection states
enum RollbackState {
    ROLLBACK_STATE_DISCONNECTED = 0,
    ROLLBACK_STATE_INITIALIZING,
    ROLLBACK_STATE_SYNCING,
    ROLLBACK_STATE_RUNNING,
    ROLLBACK_STATE_DESYNCED,
    ROLLBACK_STATE_ERROR
};

// Status structure for UI display
struct RollbackStatus {
    RollbackState state;
    int localPlayer;           // 0 = P1, 1 = P2
    int syncProgress;          // 0-100%
    int localFrame;
    int confirmedFrame;
    int rollbackFrames;
    float ping;
    float jitter;
    bool desyncDetected;
    uint32_t desyncFrame;
    uint32_t localChecksum;
    uint32_t remoteChecksum;
};

// ============================================================================
// Rollback System Interface - STUB IMPLEMENTATION
// ============================================================================
// These functions are currently stubs. Replace with actual rollback
// integration (custom implementation, etc.)

#ifdef __cplusplus
extern "C" {
#endif

// Initialization
void Rollback_Init(void);
void Rollback_Shutdown(void);

// Session management
bool Rollback_StartSession(int localPlayer, const char* remoteIP, uint16_t remotePort, uint16_t localPort);
bool Rollback_StartLocalTest(void);
void Rollback_EndSession(void);
void Rollback_ResetForNewMatch(void);

// Session state queries
bool Rollback_IsActive(void);
bool Rollback_IsRunning(void);
bool Rollback_IsResimulating(void);
bool Rollback_IsSessionActive(void);
RollbackState Rollback_GetConnectionState(void);

// Frame advancement
bool Rollback_ProcessFrame(uint16_t localInput);
void Rollback_OnFrame(uint16_t localInput);
void Rollback_AdvanceSession(void);

// Input handling
bool Rollback_AddLocalInput(int player, uint16_t input);
bool Rollback_GetCurrentInputs(uint16_t* p1Input, uint16_t* p2Input);
uint16_t Rollback_GetInput(int player);
bool Rollback_IsInputConfirmed(int player);

// Status and statistics
void Rollback_GetStatus(RollbackStatus* status);
int Rollback_GetMatchFrame(void);
size_t Rollback_GetPendingAdvanceCount(void);
void Rollback_GetNetworkStats(int player, float* ping, float* jitter);
int Rollback_GetRollbackCount(void);
int Rollback_GetMaxRollbackDepth(void);
float Rollback_GetFrameAdvantage(void);
int Rollback_GetLocalPlayerIndex(void);

// Configuration
void Rollback_SetInputDelay(int delay);
int Rollback_GetInputDelay(void);
void Rollback_SetDesyncDetection(bool enabled);
void Rollback_SetInputSyncAllowed(bool allowed);

#ifdef __cplusplus
}
#endif

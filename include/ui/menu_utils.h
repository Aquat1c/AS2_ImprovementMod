/**
 * Alice Senki 2 - Menu Utilities
 *
 * Reusable UI helpers for the netplay menu frontend:
 *   - Public IP lookup (async, via api.ipify.org)
 *   - "Your Address" formatting (external IP + listen port)
 *   - Clipboard copy / paste
 *   - Clipboard flash feedback ("Copied!")
 *   - Display-safe string truncation
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace MenuUtils {

// ============================================================================
// Public IP Lookup (async background fetch)
// ============================================================================

enum class IPFetchState : uint8_t {
    Idle = 0,   // Not attempted
    Fetching,   // Background thread running
    Done,       // IP available
    Failed,     // Lookup failed
};

/// Kick off an async public IP fetch. Safe to call repeatedly (no-ops if
/// already fetching or already have a result).
void BeginPublicIPFetch();

/// Current fetch state.
IPFetchState GetPublicIPState();

/// The fetched public IP string, or "" if not available.
const char* GetPublicIP();

// ============================================================================
// "Your Address" Formatting
// ============================================================================

/// Format "externalIP:listenPort" into the internal buffer and return it.
/// Returns "?:port" if no public IP is available yet.
const char* UpdateYourAddress(uint16_t listenPort);

/// Get the last-formatted address string without updating.
const char* GetYourAddress();

// ============================================================================
// Clipboard
// ============================================================================

/// Copy text to the Windows clipboard. Returns true on success.
bool CopyToClipboard(const char* text);

/// Paste text from the Windows clipboard into dst. Returns true on success.
bool PasteFromClipboard(char* dst, size_t cap);

// ============================================================================
// Clipboard Flash Feedback
// ============================================================================

/// Set a brief flash message (e.g. "Copied!") that auto-expires after ~2s.
void FlashClipboardMessage(const char* msg);

/// Returns true if a flash message is currently visible.
bool HasClipboardFlash();

/// Get the current flash message text (empty string if expired).
const char* GetClipboardFlash();

// ============================================================================
// Display String Helpers
// ============================================================================

/// Copy src to dst, truncating with "..." if longer than maxChars.
void CopyDisplayText(char* dst, size_t cap, const char* src, size_t maxChars);

// ============================================================================
// Lifecycle
// ============================================================================

/// Clean up resources (IP fetch thread handle, etc). Call at mod shutdown.
void Cleanup();

} // namespace MenuUtils

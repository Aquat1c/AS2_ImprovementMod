/**
 * Alice Senki 2 - Rematch Boundary Cleanup
 *
 * Resets match-scoped rollback / input / sync leftovers when the current
 * match flow is about to hand control back to CharSel or the connected
 * session menu. This intentionally avoids tearing down the live session.
 */

#pragma once

namespace Rollback {

/// Clears match-scoped leftovers before starting the next pregame flow.
/// The session itself remains alive.
void RematchCleanup_PrepareForNextMatch(const char* reason);

/// Clears match-scoped leftovers before launching a fresh netplay CharSel flow.
/// This covers the "played offline/practice first, then start netplay" path.
void RematchCleanup_PrepareForNetplayLaunch(const char* reason);

} // namespace Rollback

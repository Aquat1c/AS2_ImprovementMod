/**
 * Alice Senki 2 - Input Timeline (re0.7 M4)
 *
 * The resurrected canonical input ring is header-only (`InputRing` in
 * include/rollback/input_timeline.h) so the pure engine2 core and the
 * socket-free test harness can instantiate it without linking mod TUs.
 * This TU exists only to compile the header standalone (include hygiene).
 */

#include "rollback/input_timeline.h"

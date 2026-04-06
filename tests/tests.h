/**
 * Alice Senki 2 Mod - Edge Case Tests
 *
 * Lightweight test harness that can be invoked from the ImGui debug menu.
 * Each test module exposes a RunAll() function.
 */

#pragma once

#include <stdint.h>

namespace PacketCodecTests {
void RunAll();
}

namespace SavestateTests {

struct ProbeResult {
	bool     valid;
	bool     passed;
	uint32_t frame_before;
	uint32_t frame_after;
	uint32_t checksum_before;
	uint32_t checksum_after;
	uint16_t quick_before;
	uint16_t quick_after;
	uint32_t rng_before;
	uint32_t rng_after;
	char     status[160];
};

void RunAll();
bool RunRoundTripProbe(ProbeResult* out);
bool GetLastProbeResult(ProbeResult* out);
}

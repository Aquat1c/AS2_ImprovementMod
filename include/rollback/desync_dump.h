/**
 * Alice Senki 2 - Desync State Dump
 *
 * Comprehensive game state snapshot writer for desync diagnosis.
 * Dumps entity state, combat fields, input buffers, per-region CRC
 * breakdowns, and hex dumps of critical memory regions.
 *
 * Reusable: call DesyncDump_WriteFullDump() from anywhere that has a
 * file handle and a frame number.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>

namespace Rollback {

/// Parameters for a desync dump.
struct DesyncDumpParams {
    int32_t  frame;
    uint32_t local_crc;
    uint32_t remote_crc;
    int      dump_number;     // Sequential dump count
};

/// Write a comprehensive state dump to the given file handle.
/// The caller is responsible for opening/closing the FILE*.
void DesyncDump_WriteFullDump(FILE* f, const DesyncDumpParams& params);

/// Write a single entity's full field dump.
/// `label` is "P1" or "P2", `base` is the entity base address.
void DesyncDump_WriteEntityDetail(FILE* f, const char* label, uintptr_t base);

/// Write hex dump of a labeled memory region with CRC.
void DesyncDump_HexDumpRegion(FILE* f, const char* label, uintptr_t addr, size_t size);

/// Top-level convenience: checks cooldown, opens file, writes dump, closes.
/// Returns true if a dump was actually written.
bool DesyncDump_TryDump(int32_t frame, uint32_t local_crc, uint32_t remote_crc);

/// Feed a per-frame checksum into the dump module's local ring buffer.
/// Call from rollback_debug each frame so dumps can show nearby checksums.
void DesyncDump_StoreChecksum(int32_t frame, uint32_t crc);

/// Reset dump state (call on session start).
void DesyncDump_Reset();

} // namespace Rollback

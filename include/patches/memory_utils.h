/**
 * Alice Senki 2 - Memory Utility Functions
 * 
 * SEH-protected memory read/write helpers used throughout the mod.
 * Extracted from as2_rollback.cpp (non-rollback utility code).
 */

#pragma once

#include <windows.h>
#include <stdint.h>
#include <string.h>

// SEH-safe memory read. Returns T() on access violation.
template<typename T>
T ReadMemory(uintptr_t address);

// SEH-safe memory write with VirtualProtect. Returns false on failure.
template<typename T>
bool WriteMemory(uintptr_t address, T value);

// SEH-safe memcpy.
bool CopyMemorySafe(void* dst, const void* src, size_t size);

// VirtualProtect + SEH-safe block write.
bool WriteMemoryBlockSafe(void* dst, const void* src, size_t size);

// CRC32 checksum (standard polynomial 0xEDB88320).
uint32_t CalcCRC32(const void* data, size_t size);

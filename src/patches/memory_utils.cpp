/**
 * Alice Senki 2 - Memory Utility Functions
 * 
 * SEH-protected memory read/write helpers.
 * Extracted from as2_rollback.cpp (non-rollback utility code).
 */

#include "memory_utils.h"

// ============================================================================
// Template Implementations
// ============================================================================

template<typename T>
T ReadMemory(uintptr_t address) {
    __try {
        return *reinterpret_cast<T*>(address);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return T();
    }
}

template<typename T>
bool WriteMemory(uintptr_t address, T value) {
    __try {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        *reinterpret_cast<T*>(address) = value;
        VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), oldProtect, &oldProtect);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Explicit template instantiations for ReadMemory
template uint8_t ReadMemory<uint8_t>(uintptr_t address);
template int8_t ReadMemory<int8_t>(uintptr_t address);
template uint16_t ReadMemory<uint16_t>(uintptr_t address);
template int16_t ReadMemory<int16_t>(uintptr_t address);
template uint32_t ReadMemory<uint32_t>(uintptr_t address);
template int32_t ReadMemory<int32_t>(uintptr_t address);

// Explicit template instantiations for WriteMemory
template bool WriteMemory<uint8_t>(uintptr_t address, uint8_t value);
template bool WriteMemory<int8_t>(uintptr_t address, int8_t value);
template bool WriteMemory<uint16_t>(uintptr_t address, uint16_t value);
template bool WriteMemory<int16_t>(uintptr_t address, int16_t value);
template bool WriteMemory<uint32_t>(uintptr_t address, uint32_t value);
template bool WriteMemory<int32_t>(uintptr_t address, int32_t value);

// ============================================================================
// Non-template functions
// ============================================================================

bool CopyMemorySafe(void* dst, const void* src, size_t size) {
    __try {
        memcpy(dst, src, size);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMemoryBlockSafe(void* dst, const void* src, size_t size) {
    __try {
        DWORD oldProtect;
        if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        memcpy(dst, src, size);
        VirtualProtect(dst, size, oldProtect, &oldProtect);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t CalcCRC32(const void* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* ptr = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) {
        crc ^= ptr[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

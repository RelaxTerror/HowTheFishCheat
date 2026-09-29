#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

namespace mem {

// --- Adres yardimcilari ---
inline uintptr_t ModuleBase(const wchar_t* modName = nullptr) {
    return (uintptr_t)GetModuleHandleW(modName);
}

// Sigorta: VirtualProtect ile yazilabilir yapip geri yukler
inline bool Patch(void* dst, const void* src, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(dst, src, size);
    VirtualProtect(dst, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    return true;
}

inline bool Nop(void* dst, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memset(dst, 0x90, size);
    VirtualProtect(dst, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    return true;
}

// JIT fonksiyonu RET (0xC3) ile baslat = fonksiyonu etkisizlestir.
// Orjinal ilk N byte'i out parametresine kaydeder (geri almak icin).
struct JitPatch {
    void* addr = nullptr;
    std::vector<uint8_t> original;
    size_t patchSize = 1; // default: 1 byte RET

    bool apply(void* codeAddr, size_t n = 1) {
        if (!codeAddr || addr) return false;
        addr = codeAddr;
        patchSize = n;
        original.resize(n);
        memcpy(original.data(), codeAddr, n);
        uint8_t ret = 0xC3;
        if (n > 1) {
            // RET + NOP doldur
            std::vector<uint8_t> buf(n, 0x90);
            buf[0] = 0xC3;
            return Patch(addr, buf.data(), n);
        }
        return Patch(addr, &ret, 1);
    }
    bool restore() {
        if (!addr || original.empty()) return false;
        bool ok = Patch(addr, original.data(), original.size());
        addr = nullptr;
        original.clear();
        return ok;
    }
    bool active() const { return addr != nullptr; }
};

// --- Pattern scan (IDA-style "48 8B ? ? ? ? 90") ---
inline bool CheckMask(const uint8_t* data, const uint8_t* pat, const char* mask) {
    for (; *mask; ++mask, ++data, ++pat)
        if (*mask == 'x' && *data != *pat) return false;
    return true;
}

inline uintptr_t PatternScan(uintptr_t base, size_t size, const char* signature) {
    // signature -> bytes + mask
    static auto parse = [](const char* sig, std::vector<uint8_t>& bytes, std::string& mask) {
        bytes.clear(); mask.clear();
        const char* p = sig;
        while (*p) {
            if (*p == ' ') { ++p; continue; }
            if (*p == '?') { bytes.push_back(0); mask += '?'; p += (*p && *(p+1)=='?') ? 2 : 1; }
            else { bytes.push_back((uint8_t)strtoul(p, nullptr, 16)); mask += 'x'; p += 2; }
        }
    };
    std::vector<uint8_t> pat; std::string mask;
    parse(signature, pat, mask);
    size_t n = pat.size();
    if (n == 0 || size < n) return 0;
    const uint8_t* mem = (const uint8_t*)base;
    for (size_t i = 0; i + n <= size; ++i)
        if (CheckMask(mem + i, pat.data(), mask.c_str()))
            return base + i;
    return 0;
}

inline uintptr_t PatternScanMod(const wchar_t* mod, const char* sig) {
    uintptr_t base = ModuleBase(mod);
    if (!base) return 0;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;
    return PatternScan(base, size, sig);
}

// Pointer chain coz: base + off0 -> deref + off1 -> ...
template <typename T = uintptr_t>
inline T ReadChain(uintptr_t base, const std::vector<uint64_t>& offsets) {
    uintptr_t addr = base;
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (IsBadReadPtr((void*)addr, sizeof(uintptr_t))) return T{};
        addr = *(uintptr_t*)addr + offsets[i];
    }
    if constexpr (std::is_same_v<T, uintptr_t>) return (T)addr;
    if (IsBadReadPtr((void*)addr, sizeof(T))) return T{};
    return *(T*)addr;
}

} // namespace mem

// disables pak / IoStore signature checks.

//   "Pak is invalid" - pak-validity gate
//   "Missing signature" - IoStore container "Signed" flag check
//   "Failed to decrypt TOC signature" - TOC/block signature decrypt+compare function

#include <Windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uintptr_t kIdaImageBase = 0x140000000; // image base IDA used
FILE*   g_log    = nullptr;
HMODULE g_module = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
}

std::string LogPath()
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string p(path);
    size_t pos = p.find_last_of("\\/");
    return (pos == std::string::npos ? "" : p.substr(0, pos + 1)) + "ASI_HookLog.txt";
}

std::string Hex(const BYTE* data, size_t len)
{
    std::string s;
    char buf[4];
    for (size_t i = 0; i < len; ++i) {
        snprintf(buf, sizeof(buf), "%02X ", data[i]);
        s += buf;
    }
    return s;
}


// Pattern scanning ("48 8D 05 ?? ?? ?? ??")

using Pattern = std::vector<int>;

Pattern ParsePattern(const char* text)
{
    Pattern out;
    for (const char* p = text; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') {
            out.push_back(-1);
            while (*p == '?') ++p;
        } else {
            out.push_back(static_cast<int>(strtoul(p, nullptr, 16)));
            p += 2;
        }
    }
    return out;
}

bool MatchesAt(uintptr_t addr, const Pattern& pat)
{
    const BYTE* mem = reinterpret_cast<const BYTE*>(addr);
    for (size_t i = 0; i < pat.size(); ++i)
        if (pat[i] != -1 && mem[i] != pat[i]) return false;
    return true;
}

std::vector<uintptr_t> ScanAll(uintptr_t base, const Pattern& pat, size_t maxHits = 64)
{
    std::vector<uintptr_t> hits;

    size_t firstIdx = 0;
    while (firstIdx < pat.size() && pat[firstIdx] == -1) ++firstIdx;
    if (firstIdx == pat.size()) return hits;
    const BYTE firstByte = static_cast<BYTE>(pat[firstIdx]);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

        uintptr_t start = base + sec->VirtualAddress;
        size_t    size  = sec->Misc.VirtualSize;
        if (size < pat.size()) continue;


        const BYTE* p   = reinterpret_cast<const BYTE*>(start) + firstIdx;
        const BYTE* end = reinterpret_cast<const BYTE*>(start) + (size - pat.size()) + firstIdx;  // inclusive
        while (p <= end) {
            p = static_cast<const BYTE*>(memchr(p, firstByte, static_cast<size_t>(end - p) + 1));
            if (!p) break;
            uintptr_t candidate = reinterpret_cast<uintptr_t>(p) - firstIdx;
            if (MatchesAt(candidate, pat)) {
                hits.push_back(candidate);
                if (hits.size() >= maxHits) return hits;
            }
            ++p;
        }
    }
    return hits;
}

bool InImage(uintptr_t base, uintptr_t addr, size_t len)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return addr >= base && addr + len <= base + nt->OptionalHeader.SizeOfImage;
}

uintptr_t FunctionStart(uintptr_t addr)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(addr, &imageBase, nullptr);
    return rf ? static_cast<uintptr_t>(imageBase + rf->BeginAddress) : 0;
}


// Patch description
struct Variant {
    const char*       expected; // bytes that must be at the target
    std::vector<BYTE> replacement; // bytes written over the target
};

struct PatchDef {
    const char*           name;
    uintptr_t             fallbackVA; // IDA VA for the current build (used if scan fails)
    const char*           pattern;  // anchor pattern, or nullptr to use fallback only
    size_t                patchOffset; // target offset from the start of the pattern match
    bool                  toFunctionStart; // if true, target = start of the function containing the match
    std::vector<Variant>  variants; // first variant whose `expected` matches is applied
    bool                (*validate)(uintptr_t target) = nullptr; // optional extra check per candidate
};

// pak gate check
// jz jumps to the "Pak is invalid" error block

// cmp byte ptr [rip+x], 3
// jb short / mov r8, r14
// lea rdx, [rip+x]

bool JzTargetsPakErrorBlock(uintptr_t jz)
{
    if (!MatchesAt(jz, ParsePattern("0F 84"))) return false;
    int32_t disp = 0;
    memcpy(&disp, reinterpret_cast<const void*>(jz + 2), sizeof(disp));
    uintptr_t dest = jz + 6 + disp;
    return MatchesAt(dest, ParsePattern("80 3D ?? ?? ?? ?? 03 72 ?? 4D 8B C6 48 8D 15"));
}

const std::vector<PatchDef> kPatches = {
    // final pak-validity gate
    // cmp byte ptr [reg+2F1h], 0
    // jz <Pak is invalid>
    {
        "Pak valid gate",
        0x1424B6C3C,
        "80 ?? F1 02 00 00 00 0F 84 ?? ?? ?? ??",
        7,
        false,
        {
            { "0F 84 ?? ?? ?? ??", { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },  // near jz
            { "74 ??",             { 0x90, 0x90 } },                          // short jz
        },
        JzTargetsPakErrorBlock,
    },

    // ioStore "Signed" flag gate
    // test bl, 4
    // jnz short <ok>
    {
        "IoStore signed-flag gate",
        0x14127ACE4,
        "FF 50 30 84 C0 74 ?? F6 C3 04 75 ?? 48 8D 05",
        10,
        false,
        {
            { "75", { 0xEB } },
        },
    },

    // TOC/block signature decrypt+compare
    // failures write 0x0C into the error struct at [rcx]
    // mov dword ptr [rcx], 0
    // ret

    // anchor
    // cmp eax, 14h
    // jz
    // lea rax, "Failed to decrypt TOC signature"`
    {
        "TOC signature function",
        0x141285AE0,
        "83 F8 14 74 ?? 48 8D 05",
        0,
        true,
        {
            { "40 55", { 0xC7, 0x01, 0x00, 0x00, 0x00, 0x00, 0xC3, 0x90 } },
        },
    },
};

uintptr_t ResolveTarget(const PatchDef& def, uintptr_t base)
{
    const uintptr_t fallback = base + (def.fallbackVA - kIdaImageBase);

    if (def.pattern && !def.toFunctionStart && InImage(base, fallback - def.patchOffset, 32)) {
        if (MatchesAt(fallback - def.patchOffset, ParsePattern(def.pattern)) &&
            (!def.validate || def.validate(fallback))) {
            Log("  [fast] anchor matches at the known address 0x%llx\n", (unsigned long long)fallback);
            return fallback;
        }
    }

    if (def.pattern) {
        std::vector<uintptr_t> targets;
        for (uintptr_t hit : ScanAll(base, ParsePattern(def.pattern))) {
            uintptr_t t = hit + def.patchOffset;
            if (def.toFunctionStart) t = FunctionStart(t);
            if (!t) continue;
            if (def.validate && !def.validate(t)) continue;
            if (std::find(targets.begin(), targets.end(), t) == targets.end())
                targets.push_back(t);
        }

        Log("  [scan] %zu candidate target(s)\n", targets.size());
        for (uintptr_t t : targets)
            Log("    0x%llx (RVA 0x%llx)\n", (unsigned long long)t, (unsigned long long)(t - base));

        if (targets.size() == 1) return targets[0];
        Log("  [scan] %s, using fallback address\n", targets.empty() ? "no match" : "ambiguous");
    }

    Log("  [fallback] target 0x%llx\n", (unsigned long long)fallback);
    if (!InImage(base, fallback, 8)) {
        Log("  fallback address is outside the image, giving up\n");
        return 0;
    }
    return fallback;
}

bool ApplyPatch(const PatchDef& def, uintptr_t base)
{
    Log("Patch '%s'\n", def.name);

    uintptr_t target = ResolveTarget(def, base);
    if (!target) return false;
    const BYTE* mem  = reinterpret_cast<const BYTE*>(target);
    Log("  before: %s\n", Hex(mem, 8).c_str());

    const Variant* chosen = nullptr;
    for (const Variant& v : def.variants) {
        if (MatchesAt(target, ParsePattern(v.expected))) { chosen = &v; break; }
    }
    if (!chosen) {
        for (const Variant& v : def.variants) {
            if (memcmp(mem, v.replacement.data(), v.replacement.size()) == 0) {
                Log("  already patched (bytes match the replacement), nothing to do\n");
                return true;
            }
        }
        Log("  REFUSED: bytes at target do not match any expected variant\n");
        return false;
    }

    const size_t len = chosen->replacement.size();
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("  VirtualProtect failed: %lu\n", GetLastError());
        return false;
    }
    memcpy(reinterpret_cast<void*>(target), chosen->replacement.data(), len);
    VirtualProtect(reinterpret_cast<void*>(target), len, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), len);

    Log("  after:  %s(patched %zu bytes)\n", Hex(mem, 8).c_str(), len);
    return true;
}

DWORD WINAPI MainThread(LPVOID)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    Log("===MainThread started, module base 0x%llx===\n", (unsigned long long)base);

    for (const PatchDef& def : kPatches)
        Log("  -> %s\n", ApplyPatch(def, base) ? "SUCCESS" : "FAILED");

    Log("===Patch setup complete===\n");
    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        fopen_s(&g_log, LogPath().c_str(), "w");
        Log("===DLL_PROCESS_ATTACH===\n");
        MainThread(nullptr);
    }
    return TRUE;
}
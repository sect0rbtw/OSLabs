#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cstdint>

struct Allocation {
    void*  base = nullptr;
    SIZE_T size = 0;
    DWORD  allocType = 0;
};

static std::vector<Allocation> g_allocs;

static std::string hexPtr(const void* p) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << (uintptr_t)p;
    return oss.str();
}

static bool readLine(std::string& out) {
    std::getline(std::cin, out);
    return static_cast<bool>(std::cin);
}

static bool parseUInt64(const std::string& s, uint64_t& v) {
    std::string t = s;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();

    size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    if (i >= t.size()) return false;

    int base = 10;
    if (t.size() - i > 2 && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }

    std::string num = t.substr(i);
    if (num.empty()) return false;

    char* endp = nullptr;
    errno = 0;
    unsigned long long tmp = std::strtoull(num.c_str(), &endp, base);
    if (errno != 0 || endp == num.c_str() || *endp != '\0') return false;

    v = static_cast<uint64_t>(tmp);
    return true;
}

static const char* stateToStr(DWORD st) {
    switch (st) {
        case MEM_COMMIT:  return "MEM_COMMIT";
        case MEM_FREE:    return "MEM_FREE";
        case MEM_RESERVE: return "MEM_RESERVE";
        default:          return "UNKNOWN";
    }
}

static std::string protectToStr(DWORD p) {
    switch (p & 0xFF) {
        case PAGE_NOACCESS:          return "PAGE_NOACCESS";
        case PAGE_READONLY:          return "PAGE_READONLY";
        case PAGE_READWRITE:         return "PAGE_READWRITE";
        case PAGE_WRITECOPY:         return "PAGE_WRITECOPY";
        case PAGE_EXECUTE:           return "PAGE_EXECUTE";
        case PAGE_EXECUTE_READ:      return "PAGE_EXECUTE_READ";
        case PAGE_EXECUTE_READWRITE: return "PAGE_EXECUTE_READWRITE";
        case PAGE_EXECUTE_WRITECOPY: return "PAGE_EXECUTE_WRITECOPY";
        default:                     return "PAGE_?(unknown)";
    }
}

static bool canReadProtect(DWORD p) {
    DWORD x = p & 0xFF;
    return x == PAGE_READONLY ||
           x == PAGE_READWRITE ||
           x == PAGE_WRITECOPY ||
           x == PAGE_EXECUTE_READ ||
           x == PAGE_EXECUTE_READWRITE ||
           x == PAGE_EXECUTE_WRITECOPY;
}

static bool canWriteProtect(DWORD p) {
    DWORD x = p & 0xFF;
    return x == PAGE_READWRITE ||
           x == PAGE_WRITECOPY ||
           x == PAGE_EXECUTE_READWRITE ||
           x == PAGE_EXECUTE_WRITECOPY;
}

static bool queryRegion(void* addr, MEMORY_BASIC_INFORMATION& mbi) {
    std::memset(&mbi, 0, sizeof(mbi));
    return VirtualQuery(addr, &mbi, sizeof(mbi)) != 0;
}

static bool checkAddressRange(void* addr, SIZE_T size, bool wantWrite) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!queryRegion(addr, mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect == PAGE_NOACCESS) return false;

    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t regionEnd = regionStart + mbi.RegionSize;
    uintptr_t end = start + size;

    if (end > regionEnd) return false;
    return wantWrite ? canWriteProtect(mbi.Protect) : canReadProtect(mbi.Protect);
}

static void printSystemInfo() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    std::cout << "\n=== GetSystemInfo ===\n";
    std::cout << "Processor architecture: " << si.wProcessorArchitecture << "\n";
    std::cout << "Page size: " << si.dwPageSize << "\n";
    std::cout << "Min application address: " << hexPtr(si.lpMinimumApplicationAddress) << "\n";
    std::cout << "Max application address: " << hexPtr(si.lpMaximumApplicationAddress) << "\n";
    std::cout << "Active processor mask: 0x" << std::hex << std::uppercase
              << static_cast<uintptr_t>(si.dwActiveProcessorMask) << std::dec << "\n";
    std::cout << "Number of processors: " << si.dwNumberOfProcessors << "\n";
    std::cout << "Allocation granularity: " << si.dwAllocationGranularity << "\n";
    std::cout << "Processor type: " << si.dwProcessorType << "\n";
}

static void printMemoryStatus() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);

    if (!GlobalMemoryStatusEx(&ms)) {
        std::cout << "GlobalMemoryStatusEx failed: " << GetLastError() << "\n";
        return;
    }

    auto toGiB = [](DWORDLONG x) {
        return static_cast<double>(x) / (1024.0 * 1024.0 * 1024.0);
    };

    std::cout << "\n=== GlobalMemoryStatusEx ===\n";
    std::cout << "Memory load: " << ms.dwMemoryLoad << "%\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total physical: " << toGiB(ms.ullTotalPhys) << " GiB\n";
    std::cout << "Avail physical: " << toGiB(ms.ullAvailPhys) << " GiB\n";
    std::cout << "Total pagefile: " << toGiB(ms.ullTotalPageFile) << " GiB\n";
    std::cout << "Avail pagefile: " << toGiB(ms.ullAvailPageFile) << " GiB\n";
    std::cout << "Total virtual: " << toGiB(ms.ullTotalVirtual) << " GiB\n";
    std::cout << "Avail virtual: " << toGiB(ms.ullAvailVirtual) << " GiB\n";
    std::cout.unsetf(std::ios::floatfield);
}

static void doVirtualQuery() {
    std::cout << "\nEnter address (dec or 0x...): ";
    std::string s;
    if (!readLine(s)) return;

    uint64_t addr64 = 0;
    if (!parseUInt64(s, addr64)) {
        std::cout << "Invalid address.\n";
        return;
    }

    void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(addr64));
    MEMORY_BASIC_INFORMATION mbi{};
    if (!queryRegion(addr, mbi)) {
        std::cout << "VirtualQuery failed: " << GetLastError() << "\n";
        return;
    }

    std::cout << "\n=== VirtualQuery ===\n";
    std::cout << "BaseAddress: " << hexPtr(mbi.BaseAddress) << "\n";
    std::cout << "AllocationBase: " << hexPtr(mbi.AllocationBase) << "\n";
    std::cout << "AllocationProtect: " << protectToStr(mbi.AllocationProtect) << "\n";
    std::cout << "RegionSize: " << static_cast<unsigned long long>(mbi.RegionSize) << " bytes\n";
    std::cout << "State: " << stateToStr(mbi.State) << "\n";
    std::cout << "Protect: " << protectToStr(mbi.Protect) << "\n";
    std::cout << "Type: ";
    if (mbi.Type == MEM_IMAGE) std::cout << "MEM_IMAGE\n";
    else if (mbi.Type == MEM_MAPPED) std::cout << "MEM_MAPPED\n";
    else if (mbi.Type == MEM_PRIVATE) std::cout << "MEM_PRIVATE\n";
    else std::cout << "UNKNOWN\n";
}

static SIZE_T askSizeBytes() {
    std::cout << "Enter region size in bytes (dec): ";
    std::string s;
    if (!readLine(s)) return 0;

    uint64_t v = 0;
    if (!parseUInt64(s, v) || v == 0) return 0;
    return static_cast<SIZE_T>(v);
}

static void* askBaseAddressOrNull() {
    std::cout << "Enter base address (0 = auto, otherwise 0x.../dec): ";
    std::string s;
    if (!readLine(s)) return nullptr;

    uint64_t v = 0;
    if (!parseUInt64(s, v)) return nullptr;
    if (v == 0) return nullptr;

    return reinterpret_cast<void*>(static_cast<uintptr_t>(v));
}

static void listAllocations() {
    std::cout << "\n=== Current allocations (inside this program) ===\n";
    if (g_allocs.empty()) {
        std::cout << "(empty)\n";
        return;
    }

    for (size_t i = 0; i < g_allocs.size(); ++i) {
        std::cout << "[" << i << "] base=" << hexPtr(g_allocs[i].base)
                  << " size=" << static_cast<unsigned long long>(g_allocs[i].size)
                  << " allocType=0x" << std::hex << std::uppercase << g_allocs[i].allocType
                  << std::dec << "\n";
    }
}

static void reserveRegionAuto() {
    SIZE_T size = askSizeBytes();
    if (!size) {
        std::cout << "Invalid size.\n";
        return;
    }

    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
        std::cout << "VirtualAlloc(MEM_RESERVE) failed: " << GetLastError() << "\n";
        return;
    }

    g_allocs.push_back({p, size, MEM_RESERVE});
    std::cout << "RESERVE OK: base=" << hexPtr(p)
              << " size=" << static_cast<unsigned long long>(size) << "\n";
}

static void reserveRegionManual() {
    SIZE_T size = askSizeBytes();
    if (!size) {
        std::cout << "Invalid size.\n";
        return;
    }

    void* base = askBaseAddressOrNull();
    void* p = VirtualAlloc(base, size, MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
        std::cout << "VirtualAlloc(MEM_RESERVE) failed: " << GetLastError() << "\n";
        return;
    }

    g_allocs.push_back({p, size, MEM_RESERVE});
    std::cout << "RESERVE OK: base=" << hexPtr(p)
              << " size=" << static_cast<unsigned long long>(size) << "\n";
}

static void commitRegionByIndex() {
    listAllocations();
    std::cout << "   : ";
    std::string s;
    if (!readLine(s)) return;

    uint64_t idx = 0;
    if (!parseUInt64(s, idx) || idx >= g_allocs.size()) {
        std::cout << "Invalid index.\n";
        return;
    }

    void* base = g_allocs[static_cast<size_t>(idx)].base;
    SIZE_T size = g_allocs[static_cast<size_t>(idx)].size;

    void* p = VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        std::cout << "VirtualAlloc(MEM_COMMIT) failed: " << GetLastError() << "\n";
        return;
    }

    std::cout << "COMMIT OK: base=" << hexPtr(p)
              << " size=" << static_cast<unsigned long long>(size) << "\n";
}

static void commitRegionByAddress() {
    void* base = askBaseAddressOrNull();
    if (!base) {
        std::cout << "Address is not set.\n";
        return;
    }

    SIZE_T size = askSizeBytes();
    if (!size) {
        std::cout << "Invalid size.\n";
        return;
    }

    void* p = VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        std::cout << "VirtualAlloc(MEM_COMMIT) failed: " << GetLastError() << "\n";
        return;
    }

    std::cout << "COMMIT OK: base=" << hexPtr(p)
              << " size=" << static_cast<unsigned long long>(size) << "\n";
}

static void reserveAndCommitAuto() {
    SIZE_T size = askSizeBytes();
    if (!size) {
        std::cout << "Invalid size.\n";
        return;
    }

    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        std::cout << "VirtualAlloc(RESERVE|COMMIT) failed: " << GetLastError() << "\n";
        return;
    }

    g_allocs.push_back({p, size, MEM_RESERVE | MEM_COMMIT});
    std::cout << "RESERVE|COMMIT OK: base=" << hexPtr(p)
              << " size=" << static_cast<unsigned long long>(size) << "\n";
}

static void reserveAndCommitManual() {
    SIZE_T size = askSizeBytes();
    if (!size) {
        std::cout << "Invalid size.\n";
        return;
    }

    void* base = askBaseAddressOrNull();
    void* p = VirtualAlloc(base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        std::cout << "VirtualAlloc(RESERVE|COMMIT) failed: " << GetLastError() << "\n";
        return;
    }

    g_allocs.push_back({p, size, MEM_RESERVE | MEM_COMMIT});
    std::cout << "RESERVE|COMMIT OK: base=" << hexPtr(p)
              << " size=" << static_cast<unsigned long long>(size) << "\n";
}

static void freeRegionByIndex() {
    listAllocations();
    std::cout << "  : ";
    std::string s;
    if (!readLine(s)) return;

    uint64_t idx = 0;
    if (!parseUInt64(s, idx) || idx >= g_allocs.size()) {
        std::cout << "Invalid index.\n";
        return;
    }

    void* base = g_allocs[static_cast<size_t>(idx)].base;
    if (!VirtualFree(base, 0, MEM_RELEASE)) {
        std::cout << "VirtualFree(MEM_RELEASE) failed: " << GetLastError() << "\n";
        return;
    }

    std::cout << "RELEASE OK: base=" << hexPtr(base) << "\n";
    g_allocs.erase(g_allocs.begin() + static_cast<std::ptrdiff_t>(idx));
}

static void freeRegionByAddress() {
    void* base = askBaseAddressOrNull();
    if (!base) {
        std::cout << "Address is not set.\n";
        return;
    }

    if (!VirtualFree(base, 0, MEM_RELEASE)) {
        std::cout << "VirtualFree(MEM_RELEASE) failed: " << GetLastError() << "\n";
        return;
    }

    std::cout << "RELEASE OK: base=" << hexPtr(base) << "\n";
    for (size_t i = 0; i < g_allocs.size(); ++i) {
        if (g_allocs[i].base == base) {
            g_allocs.erase(g_allocs.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

static void writeUint32ByAddress() {
    std::cout << "\n    uint32 (0x.../dec): ";
    std::string s;
    if (!readLine(s)) return;

    uint64_t addr64 = 0;
    if (!parseUInt64(s, addr64)) {
        std::cout << "Invalid address.\n";
        return;
    }

    auto* p = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(addr64));
    if (!checkAddressRange(p, sizeof(uint32_t), true)) {
        std::cout << "Write failed: address is not in a writable committed region.\n";
        return;
    }

    std::cout << "Enter uint32 value (dec or 0x...): ";
    if (!readLine(s)) return;

    uint64_t val64 = 0;
    if (!parseUInt64(s, val64) || val64 > 0xFFFFFFFFull) {
        std::cout << "Invalid value.\n";
        return;
    }

    *p = static_cast<uint32_t>(val64);
    std::cout << "OK: written " << static_cast<uint32_t>(val64)
              << " to address " << hexPtr(p) << "\n";
}

static void writeStringByAddress() {
    std::cout << "\n     (0x.../dec): ";
    std::string s;
    if (!readLine(s)) return;

    uint64_t addr64 = 0;
    if (!parseUInt64(s, addr64)) {
        std::cout << "Invalid address.\n";
        return;
    }

    auto* p = reinterpret_cast<char*>(static_cast<uintptr_t>(addr64));

    std::cout << "Enter string: ";
    std::string msg;
    if (!readLine(msg)) return;

    SIZE_T bytes = static_cast<SIZE_T>(msg.size() + 1);
    if (!checkAddressRange(p, bytes, true)) {
        std::cout << "Write failed: address/range is not writable.\n";
        return;
    }

    std::memcpy(p, msg.c_str(), bytes);
    std::cout << "OK:   to address " << hexPtr(p) << "\n";
}

static DWORD chooseProtection() {
    std::cout << "\n :\n"
              << "1) PAGE_NOACCESS\n"
              << "2) PAGE_READONLY\n"
              << "3) PAGE_READWRITE\n"
              << "4) PAGE_EXECUTE_READ\n"
              << "5) PAGE_EXECUTE_READWRITE\n"
              << "Choice: ";

    std::string s;
    if (!readLine(s)) return 0;

    uint64_t c = 0;
    if (!parseUInt64(s, c)) return 0;

    switch (c) {
        case 1: return PAGE_NOACCESS;
        case 2: return PAGE_READONLY;
        case 3: return PAGE_READWRITE;
        case 4: return PAGE_EXECUTE_READ;
        case 5: return PAGE_EXECUTE_READWRITE;
        default: return 0;
    }
}

static void changeProtection() {
    std::cout << "\n    (0x.../dec): ";
    std::string s;
    if (!readLine(s)) return;

    uint64_t addr64 = 0;
    if (!parseUInt64(s, addr64)) {
        std::cout << "Invalid address.\n";
        return;
    }
    void* base = reinterpret_cast<void*>(static_cast<uintptr_t>(addr64));

    SIZE_T size = askSizeBytes();
    if (!size) {
        std::cout << "Invalid size.\n";
        return;
    }

    DWORD newProt = chooseProtection();
    if (!newProt) {
        std::cout << "Invalid protection.\n";
        return;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(base, size, newProt, &oldProt)) {
        std::cout << "VirtualProtect failed: " << GetLastError() << "\n";
        return;
    }

    std::cout << "VirtualProtect OK. Old=" << protectToStr(oldProt)
              << ", New=" << protectToStr(newProt) << "\n";
}

static void checkRegionAccess() {
    std::cout << "\n     (0x.../dec): ";
    std::string s;
    if (!readLine(s)) return;

    uint64_t addr64 = 0;
    if (!parseUInt64(s, addr64)) {
        std::cout << "Invalid address.\n";
        return;
    }
    void* base = reinterpret_cast<void*>(static_cast<uintptr_t>(addr64));

    bool canRead = checkAddressRange(base, 1, false);
    bool canWrite = checkAddressRange(base, 1, true);

    std::cout << "TEST READ: " << (canRead ? "OK (  VirtualQuery)" : "ACCESS DENIED") << "\n";
    std::cout << "TEST WRITE: " << (canWrite ? "OK (  VirtualQuery)" : "ACCESS DENIED") << "\n";
}

static void printMenu() {
    std::cout << "\n========== VM Win32 Menu ==========\n"
              << "1) GetSystemInfo\n"
              << "2) GlobalMemoryStatusEx\n"
              << "3) VirtualQuery (by input address)\n"
              << "4) VirtualAlloc: RESERVE, automatic base address\n"
              << "5) VirtualAlloc: RESERVE, manual base address\n"
              << "6) VirtualAlloc: COMMIT reserved region (by index)\n"
              << "7) VirtualAlloc: COMMIT reserved region (by address)\n"
              << "8) VirtualAlloc: RESERVE | COMMIT, automatic base address\n"
              << "9) VirtualAlloc: RESERVE | COMMIT, manual base address\n"
              << "10) VirtualFree: MEM_RELEASE (by index)\n"
              << "11) VirtualFree: MEM_RELEASE (by address)\n"
              << "12) Write uint32 by address\n"
              << "13) Write string by address\n"
              << "14) VirtualProtect: change region protection\n"
              << "15) Check read/write availability of region\n"
              << "16) Show allocation list\n"
              << "0) Exit\n"
              << "Choice: ";
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::ios::sync_with_stdio(false);

    while (true) {
        printMenu();
        std::string s;
        if (!readLine(s)) break;

        uint64_t cmd = 0;
        if (!parseUInt64(s, cmd)) continue;

        switch (cmd) {
            case 1:  printSystemInfo(); break;
            case 2:  printMemoryStatus(); break;
            case 3:  doVirtualQuery(); break;
            case 4:  reserveRegionAuto(); break;
            case 5:  reserveRegionManual(); break;
            case 6:  commitRegionByIndex(); break;
            case 7:  commitRegionByAddress(); break;
            case 8:  reserveAndCommitAuto(); break;
            case 9:  reserveAndCommitManual(); break;
            case 10: freeRegionByIndex(); break;
            case 11: freeRegionByAddress(); break;
            case 12: writeUint32ByAddress(); break;
            case 13: writeStringByAddress(); break;
            case 14: changeProtection(); break;
            case 15: checkRegionAccess(); break;
            case 16: listAllocations(); break;
            case 0:  return 0;
            default: std::cout << "Unknown command.\n"; break;
        }
    }

    return 0;
}

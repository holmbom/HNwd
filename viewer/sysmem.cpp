#include "sysmem.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#include "path_utf8.h"   // på Windows den enda som drar in windows.h

#if defined(_WIN32)
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2   // GetProcessMemoryInfo ur kernel32, utan psapi.lib
#endif
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif

namespace nwdview {

namespace {

constexpr uint64_t kMiB = uint64_t(1) << 20;

size_t to_size(uint64_t v) {
    return static_cast<size_t>(std::min<uint64_t>(v, std::numeric_limits<size_t>::max()));
}

#if defined(__linux__)

// Värdet i byte för `key` ("MemTotal") i /proc/meminfo; false om raden saknas.
bool meminfo_value(const std::string& text, const char* key, uint64_t& out) {
    const std::string needle = std::string(key) + ":";
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find('\n', at), text.size());
        if (text.compare(at, needle.size(), needle) == 0) {
            std::istringstream line(text.substr(at + needle.size(), end - at - needle.size()));
            uint64_t kib = 0;
            std::string unit;
            if (!(line >> kib)) return false;
            line >> unit;
            if (!unit.empty() && unit != "kB") return false;
            out = kib * 1024;
            return true;
        }
        at = end + 1;
    }
    return false;
}

bool read_file(const char* path, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::string("kan inte öppna ") + path;
        return false;
    }
    std::ostringstream all;
    all << in.rdbuf();
    text = all.str();
    return true;
}

#endif

} // namespace

bool system_memory(SystemMemory& out, std::string& error) {
    SystemMemory got;
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    std::memset(&status, 0, sizeof status);
    status.dwLength = sizeof status;
    if (!GlobalMemoryStatusEx(&status)) {
        error = "GlobalMemoryStatusEx misslyckades (fel " + std::to_string(GetLastError()) + ")";
        return false;
    }
    got.physical = status.ullTotalPhys;
    got.available = status.ullAvailPhys;
    PROCESS_MEMORY_COUNTERS counters;
    std::memset(&counters, 0, sizeof counters);
    counters.cb = sizeof counters;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        error = "GetProcessMemoryInfo misslyckades (fel " + std::to_string(GetLastError()) + ")";
        return false;
    }
    got.process = counters.WorkingSetSize;
#elif defined(__APPLE__)
    uint64_t memsize = 0;
    size_t length = sizeof memsize;
    if (sysctlbyname("hw.memsize", &memsize, &length, nullptr, 0) != 0 || length != sizeof memsize) {
        error = std::string("sysctl hw.memsize misslyckades: ") + std::strerror(errno);
        return false;
    }
    got.physical = memsize;
    // Ledigt och inaktivt minne går att ta i bruk utan att något skrivs till disk.
    vm_statistics64_data_t vm;
    std::memset(&vm, 0, sizeof vm);
    mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm), &vm_count) != KERN_SUCCESS) {
        error = "host_statistics64 misslyckades";
        return false;
    }
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        error = "sysconf(_SC_PAGESIZE) misslyckades";
        return false;
    }
    got.available = (static_cast<uint64_t>(vm.free_count) + vm.inactive_count) *
                    static_cast<uint64_t>(page);
    mach_task_basic_info_data_t task;
    std::memset(&task, 0, sizeof task);
    mach_msg_type_number_t task_count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task),
                  &task_count) != KERN_SUCCESS) {
        error = "task_info misslyckades";
        return false;
    }
    got.process = task.resident_size;
#elif defined(__linux__)
    std::string meminfo;
    if (!read_file("/proc/meminfo", meminfo, error)) return false;
    if (!meminfo_value(meminfo, "MemTotal", got.physical)) {
        error = "/proc/meminfo saknar MemTotal";
        return false;
    }
    if (!meminfo_value(meminfo, "MemAvailable", got.available)) {
        error = "/proc/meminfo saknar MemAvailable";
        return false;
    }
    // /proc/self/statm: storlek, resident, delat, ... i sidor.
    std::string statm;
    if (!read_file("/proc/self/statm", statm, error)) return false;
    unsigned long long total_pages = 0, resident_pages = 0;
    if (std::sscanf(statm.c_str(), "%llu %llu", &total_pages, &resident_pages) != 2) {
        error = "/proc/self/statm går inte att läsa";
        return false;
    }
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        error = "sysconf(_SC_PAGESIZE) misslyckades";
        return false;
    }
    got.process = static_cast<uint64_t>(resident_pages) * static_cast<uint64_t>(page);
#else
    error = "systemets minne går inte att läsa på den här plattformen";
    return false;
#endif
    if (got.physical == 0 || got.available == 0 || got.process == 0) {
        error = "systemet gav noll byte för " +
                std::string(got.physical == 0 ? "det fysiska minnet"
                            : got.available == 0 ? "det tillgängliga minnet"
                                                 : "processens minne");
        return false;
    }
    out = got;
    return true;
}

Budgets default_budgets(const SystemMemory& sys, uint64_t gpu_memory, bool gpu_shared,
                        size_t document_bytes) {
    constexpr uint64_t kMinRam = 512 * kMiB;
    const uint64_t half = sys.physical / 2;
    const uint64_t ram = std::max<uint64_t>(half > document_bytes ? half - document_bytes : 0, kMinRam);
    Budgets b;
    b.budget.ram = to_size(ram);
    if (gpu_shared || gpu_memory == 0) {
        b.shared = true;
        b.budget.gpu = b.budget.ram;
    } else {
        b.budget.gpu = to_size(gpu_memory / 10 * 6 + gpu_memory % 10 * 6 / 10);
    }
    return b;
}

} // namespace nwdview

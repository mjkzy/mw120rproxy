#include "load_prefetch.h"

#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace loadprefetch {
namespace {
constexpr uint64_t kBatchBytes = 256ull << 20;  // elevator window of the read-ahead
constexpr uint64_t kMergeGap = 512ull << 10;    // a shorter gap costs less than a seek
constexpr uint64_t kBudgetBytes = 12ull << 30;  // upper limit of one read-ahead run
constexpr DWORD kChunkBytes = 4u << 20;
constexpr size_t kMaxReads = 1u << 21;

struct FileStats {
    std::wstring relativePath;
    DWORD flags = 0;
    uint64_t bytes = 0;
    uint64_t reads = 0;
    uint64_t pendingReads = 0;
    uint64_t syncTicks = 0;
};

struct ReadRange {
    uint32_t file = 0;
    uint32_t ms = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
using ReadFileEx_t =
    BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);

CreateFileW_t g_createFileW = nullptr;
CreateFileA_t g_createFileA = nullptr;
ReadFileEx_t g_readFileEx = nullptr;
ReadFile_t g_readFile = nullptr;
CloseHandle_t g_closeHandle = nullptr;

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<HANDLE, uint32_t> g_handles;
std::unordered_map<std::wstring, uint32_t> g_fileIds;
std::vector<FileStats> g_files;
std::vector<ReadRange> g_reads;
std::atomic<size_t> g_trackedHandles{0};
std::atomic<bool> g_recording{false};

std::wstring g_gameRoot; // lower case, with trailing backslash
std::wstring g_tracePath;
ULONGLONG g_startTick = 0;
LARGE_INTEGER g_frequency{};

std::wstring LowerCase(std::wstring text) {
    for (auto& c : text)
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c + 32);
    return text;
}

bool RelativeGamePath(LPCWSTR path, std::wstring& relative) {
    if (!path)
        return false;
    wchar_t full[1024]{};
    const DWORD length = GetFullPathNameW(path, _countof(full), full, nullptr);
    if (!length || length >= _countof(full))
        return false;
    std::wstring lower = LowerCase(full);
    if (lower.rfind(L"\\\\?\\", 0) == 0)
        lower.erase(0, 4);
    if (lower.size() <= g_gameRoot.size() || lower.compare(0, g_gameRoot.size(), g_gameRoot) != 0)
        return false;
    relative = lower.substr(g_gameRoot.size());
    return relative.rfind(L"mw120rproxy.", 0) != 0;
}

void TrackOpen(HANDLE handle, LPCWSTR name, DWORD access, DWORD flags) {
    if (handle == INVALID_HANDLE_VALUE || !g_recording.load(std::memory_order_relaxed) ||
        !(access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)))
        return;
    const DWORD error = GetLastError();
    std::wstring relative;
    if (RelativeGamePath(name, relative)) {
        AcquireSRWLockExclusive(&g_lock);
        auto [entry, added] = g_fileIds.try_emplace(relative, static_cast<uint32_t>(g_files.size()));
        if (added) {
            g_files.emplace_back();
            g_files.back().relativePath = relative;
        }
        g_files[entry->second].flags |= flags;
        if (g_handles.insert_or_assign(handle, entry->second).second)
            g_trackedHandles.fetch_add(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_lock);
    }
    SetLastError(error);
}

HANDLE WINAPI CreateFileWHook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE templateFile) {
    const HANDLE handle =
        g_createFileW(name, access, share, security, disposition, flags, templateFile);
    TrackOpen(handle, name, access, flags);
    return handle;
}

HANDLE WINAPI CreateFileAHook(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE templateFile) {
    const HANDLE handle =
        g_createFileA(name, access, share, security, disposition, flags, templateFile);
    const DWORD error = GetLastError();
    wchar_t wide[1024]{};
    if (name && MultiByteToWideChar(CP_ACP, 0, name, -1, wide, _countof(wide) - 1))
        TrackOpen(handle, wide, access, flags);
    SetLastError(error);
    return handle;
}

bool TrackedFile(HANDLE handle, uint32_t& file) {
    if (!g_trackedHandles.load(std::memory_order_relaxed))
        return false;
    AcquireSRWLockShared(&g_lock);
    const auto entry = g_handles.find(handle);
    const bool found = entry != g_handles.end();
    if (found)
        file = entry->second;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

BOOL WINAPI ReadFileHook(HANDLE handle, LPVOID buffer, DWORD size, LPDWORD read,
                         LPOVERLAPPED overlapped) {
    uint32_t file = 0;
    if (!g_recording.load(std::memory_order_relaxed) || !TrackedFile(handle, file))
        return g_readFile(handle, buffer, size, read, overlapped);

    LARGE_INTEGER position{};
    bool known = true;
    if (overlapped) {
        position.LowPart = overlapped->Offset;
        position.HighPart = static_cast<LONG>(overlapped->OffsetHigh);
    } else {
        known = SetFilePointerEx(handle, LARGE_INTEGER{}, &position, FILE_CURRENT) != FALSE;
    }

    LARGE_INTEGER begin{}, end{};
    QueryPerformanceCounter(&begin);
    const BOOL result = g_readFile(handle, buffer, size, read, overlapped);
    const DWORD error = GetLastError();
    QueryPerformanceCounter(&end);
    const bool pending = !result && error == ERROR_IO_PENDING;

    if (known && (result || pending)) {
        AcquireSRWLockExclusive(&g_lock);
        auto& stats = g_files[file];
        stats.bytes += size;
        ++stats.reads;
        if (pending)
            ++stats.pendingReads;
        else
            stats.syncTicks += static_cast<uint64_t>(end.QuadPart - begin.QuadPart);
        const uint64_t offset = static_cast<uint64_t>(position.QuadPart);
        if (!g_reads.empty() && g_reads.back().file == file &&
            g_reads.back().offset + g_reads.back().length == offset)
            g_reads.back().length += size;
        else if (g_reads.size() < kMaxReads)
            g_reads.push_back({file, static_cast<uint32_t>(GetTickCount64() - g_startTick), offset, size});
        ReleaseSRWLockExclusive(&g_lock);
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI ReadFileExHook(HANDLE handle, LPVOID buffer, DWORD size, LPOVERLAPPED overlapped,
                           LPOVERLAPPED_COMPLETION_ROUTINE completion) {
    uint32_t file = 0;
    if (overlapped && g_recording.load(std::memory_order_relaxed) && TrackedFile(handle, file)) {
        const uint64_t offset = (static_cast<uint64_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset;
        AcquireSRWLockExclusive(&g_lock);
        auto& stats = g_files[file];
        stats.bytes += size;
        ++stats.reads;
        ++stats.pendingReads;
        if (!g_reads.empty() && g_reads.back().file == file &&
            g_reads.back().offset + g_reads.back().length == offset)
            g_reads.back().length += size;
        else if (g_reads.size() < kMaxReads)
            g_reads.push_back({file, static_cast<uint32_t>(GetTickCount64() - g_startTick), offset, size});
        ReleaseSRWLockExclusive(&g_lock);
    }
    return g_readFileEx(handle, buffer, size, overlapped, completion);
}

BOOL WINAPI CloseHandleHook(HANDLE handle) {
    if (g_trackedHandles.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_handles.erase(handle))
            g_trackedHandles.fetch_sub(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return g_closeHandle(handle);
}

bool PatchImport(uintptr_t imageBase, const char* function, void* detour, void** original) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return false;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* resolved = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, function)) : nullptr;
    if (!resolved)
        return false;

    bool patched = false;
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(imageBase + directory.VirtualAddress);
         import->Name; ++import) {
        if (!import->OriginalFirstThunk)
            continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(imageBase + import->OriginalFirstThunk);
        auto* slots = reinterpret_cast<void**>(imageBase + import->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal))
                continue;
            const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                imageBase + names[i].u1.AddressOfData);
            // An unresolved slot does not hold the kernel32 export: leave it for the retry.
            if (std::strcmp(name->Name, function) != 0 || slots[i] != resolved)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots[i], sizeof(void*), PAGE_READWRITE, &old))
                continue;
            *original = slots[i];
            slots[i] = detour;
            VirtualProtect(&slots[i], sizeof(void*), old, &old);
            patched = true;
        }
    }
    return patched;
}

bool InstallRecorder(uintptr_t imageBase) {
    // Order matters: a hook runs only after its original pointer is set by PatchImport.
    PatchImport(imageBase, "ReadFileEx", &ReadFileExHook, reinterpret_cast<void**>(&g_readFileEx));
    PatchImport(imageBase, "CreateFileA", &CreateFileAHook, reinterpret_cast<void**>(&g_createFileA));
    return PatchImport(imageBase, "CloseHandle", &CloseHandleHook,
                       reinterpret_cast<void**>(&g_closeHandle)) &&
           PatchImport(imageBase, "ReadFile", &ReadFileHook, reinterpret_cast<void**>(&g_readFile)) &&
           PatchImport(imageBase, "CreateFileW", &CreateFileWHook,
                       reinterpret_cast<void**>(&g_createFileW));
}

std::string Utf8(const std::wstring& text) {
    std::string result(text.size() * 3 + 1, '\0');
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           result.data(), static_cast<int>(result.size()), nullptr,
                                           nullptr);
    result.resize(length > 0 ? static_cast<size_t>(length) : 0);
    return result;
}

std::wstring Wide(const char* text) {
    wchar_t buffer[1024]{};
    MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, _countof(buffer) - 1);
    return buffer;
}

void WriteTrace(DWORD seconds) {
    AcquireSRWLockShared(&g_lock);
    const std::vector<FileStats> files = g_files;
    const std::vector<ReadRange> reads = g_reads;
    ReleaseSRWLockShared(&g_lock);
    if (reads.empty()) {
        LOG_WARN("Prefetch", "recorder saw no game-folder reads after %lus", seconds);
        return;
    }

    const std::wstring temporary = g_tracePath + L".tmp";
    FILE* out = nullptr;
    if (_wfopen_s(&out, temporary.c_str(), L"wb") || !out) {
        LOG_ERR("Prefetch", "cannot write the read trace");
        return;
    }
    for (size_t i = 0; i < files.size(); ++i)
        std::fprintf(out, "F %zu %lX %s\n", i, files[i].flags, Utf8(files[i].relativePath).c_str());
    uint64_t total = 0;
    for (const auto& read : reads) {
        std::fprintf(out, "R %u %llu %llu %u\n", read.file, read.offset, read.length, read.ms);
        total += read.length;
    }
    std::fclose(out);
    MoveFileExW(temporary.c_str(), g_tracePath.c_str(), MOVEFILE_REPLACE_EXISTING);

    LOG_INFO("Prefetch", "read trace at %lus: %zu files, %zu ranges, %llu MB", seconds, files.size(),
             reads.size(), total >> 20);
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return files[a].bytes > files[b].bytes; });
    for (size_t i = 0; i < order.size() && i < 12; ++i) {
        const auto& file = files[order[i]];
        const double syncMs = 1000.0 * static_cast<double>(file.syncTicks) /
                              static_cast<double>(g_frequency.QuadPart);
        LOG_INFO("Prefetch", "  %s: flags=0x%lX %llu MB in %llu reads, %llu pending, sync %.0f ms",
                 Utf8(file.relativePath).c_str(), file.flags, file.bytes >> 20, file.reads,
                 file.pendingReads, syncMs);
    }
}

DWORD WINAPI RecorderThread(LPVOID) {
    DWORD previous = 0;
    for (const DWORD seconds : {DWORD{45}, DWORD{90}, DWORD{150}, DWORD{220}, DWORD{300}}) {
        Sleep((seconds - previous) * 1000);
        previous = seconds;
        WriteTrace(seconds);
    }
    g_recording.store(false, std::memory_order_relaxed);
    return 0;
}

struct ReplayFile {
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
};

uint64_t ReadBatch(std::vector<ReplayFile>& files, std::vector<ReadRange>& batch, void* buffer) {
    std::sort(batch.begin(), batch.end(), [](const ReadRange& a, const ReadRange& b) {
        return a.file != b.file ? a.file < b.file : a.offset < b.offset;
    });
    uint64_t done = 0;
    for (size_t i = 0; i < batch.size();) {
        const uint32_t fileIndex = batch[i].file;
        uint64_t begin = batch[i].offset;
        uint64_t end = begin + batch[i].length;
        for (++i; i < batch.size() && batch[i].file == fileIndex && batch[i].offset <= end + kMergeGap; ++i)
            end = std::max(end, batch[i].offset + batch[i].length);

        auto& file = files[fileIndex];
        if (file.handle == INVALID_HANDLE_VALUE) {
            if (file.path.empty())
                continue;
            file.handle = CreateFileW(file.path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            LARGE_INTEGER size{};
            if (file.handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.handle, &size)) {
                file.path.clear();
                continue;
            }
            file.size = static_cast<uint64_t>(size.QuadPart);
        }
        begin &= ~0xFFFFull;
        end = std::min(end, file.size);
        while (begin < end) {
            OVERLAPPED position{};
            position.Offset = static_cast<DWORD>(begin);
            position.OffsetHigh = static_cast<DWORD>(begin >> 32);
            DWORD read = 0;
            const DWORD wanted = static_cast<DWORD>(std::min<uint64_t>(kChunkBytes, end - begin));
            if (!ReadFile(file.handle, buffer, wanted, &read, &position) || !read)
                break;
            begin += read;
            done += read;
        }
    }
    return done;
}

DWORD WINAPI ReplayThread(LPVOID) {
    const ULONGLONG start = GetTickCount64();
    FILE* in = nullptr;
    if (_wfopen_s(&in, g_tracePath.c_str(), L"rb") || !in) {
        Sleep(3000);
        LOG_INFO("Prefetch", "no read trace yet; this run records one");
        return 0;
    }
    std::vector<ReplayFile> files;
    std::vector<ReadRange> reads;
    char line[2048];
    while (std::fgets(line, sizeof(line), in)) {
        unsigned index = 0, flags = 0, ms = 0;
        unsigned long long offset = 0, length = 0;
        int consumed = 0;
        if (sscanf_s(line, "F %u %X %n", &index, &flags, &consumed) == 2 && consumed) {
            line[std::strcspn(line, "\r\n")] = 0;
            if (index >= files.size())
                files.resize(index + 1);
            // Unbuffered game reads bypass the system cache: a read-ahead cannot help them.
            if (!(flags & FILE_FLAG_NO_BUFFERING))
                files[index].path = g_gameRoot + Wide(line + consumed);
        } else if (sscanf_s(line, "R %u %llu %llu %u", &index, &offset, &length, &ms) == 4 &&
                   length) {
            reads.push_back({index, ms, offset, length});
        }
    }
    std::fclose(in);

    void* buffer = VirtualAlloc(nullptr, kChunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint64_t done = 0;
    if (buffer) {
        std::vector<ReadRange> batch;
        uint64_t batchBytes = 0;
        for (size_t i = 0; i <= reads.size() && done < kBudgetBytes; ++i) {
            if (i < reads.size() && reads[i].file < files.size()) {
                batch.push_back(reads[i]);
                batchBytes += reads[i].length;
            }
            if (batchBytes < kBatchBytes && i < reads.size())
                continue;
            MEMORYSTATUSEX memory{sizeof(memory)};
            if (GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys < (2ull << 30))
                break;
            done += ReadBatch(files, batch, buffer);
            batch.clear();
            batchBytes = 0;
        }
        VirtualFree(buffer, 0, MEM_RELEASE);
    }
    for (auto& file : files)
        if (file.handle != INVALID_HANDLE_VALUE)
            CloseHandle(file.handle);

    const ULONGLONG elapsed = GetTickCount64() - start;
    if (elapsed < 3000)
        Sleep(static_cast<DWORD>(3000 - elapsed));
    LOG_INFO("Prefetch", "read-ahead done: %llu MB in %llu ms (%zu ranges of the last run)",
             done >> 20, elapsed, reads.size());
    return 0;
}

void StartThread(LPTHREAD_START_ROUTINE routine) {
    if (HANDLE thread = CreateThread(nullptr, 0, routine, nullptr, 0, nullptr))
        CloseHandle(thread);
}
}

void Start(HMODULE self, uintptr_t imageBase, const Settings& settings) {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash)
        return;
    slash[1] = 0;
    g_gameRoot = LowerCase(path);
    g_tracePath = std::wstring(path) + L"mw120rproxy.prefetch.txt";
    g_startTick = GetTickCount64();
    QueryPerformanceFrequency(&g_frequency);
    (void)self;

    if (settings.replay)
        StartThread(&ReplayThread);
    if (settings.record && InstallRecorder(imageBase)) {
        g_recording.store(true, std::memory_order_relaxed);
        StartThread(&RecorderThread);
    }
}
}

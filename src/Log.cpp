#include "Log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nc::log
{
namespace
{
std::mutex g_mutex;
FILE* g_file = nullptr;
Sink g_sink;

// Ring of the last lines. Static: the crash handler reads it without the lock and without the heap (it may be corrupt).
// 16 MB: room for five minutes of the densest log.
constexpr size_t kRingBytes = size_t(16) << 20;
/// Second marks: the ring byte where each second that had lines begins.
constexpr size_t kMarks = 4096;
struct Mark
{
    int64_t second = 0;
    uint64_t total = 0;
};
char g_ring[kRingBytes];
Mark g_marks[kMarks];
std::atomic<uint64_t> g_ringTotal{0}; ///< bytes ever written into the ring
std::atomic<uint64_t> g_markCount{0};
int64_t g_lastMarkSecond = INT64_MIN; ///< under g_mutex
std::string g_recentDir;              ///< under g_mutex
/// Snapshot header (build, protocol): the crash handler reads it too, hence an array rather than a string.
char g_header[512] = {};

void LocalTime(std::tm& aOut, int* aMs = nullptr)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    if (aMs)
        *aMs = static_cast<int>((duration_cast<milliseconds>(now.time_since_epoch()) % 1000).count());
    const auto t = system_clock::to_time_t(now);
#ifdef _WIN32
    localtime_s(&aOut, &t);
#else
    localtime_r(&t, &aOut);
#endif
}

std::string Timestamp()
{
    std::tm local{};
    int ms = 0;
    LocalTime(local, &ms);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, ms);
    return buf;
}

int64_t NowSecond()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

/// Under g_mutex: the line goes into the ring, with a second mark if the second is new.
void RingAppend(const char* aData, size_t aSize)
{
    const int64_t second = NowSecond();
    const uint64_t total = g_ringTotal.load(std::memory_order_relaxed);
    if (second != g_lastMarkSecond)
    {
        g_lastMarkSecond = second;
        const uint64_t count = g_markCount.load(std::memory_order_relaxed);
        g_marks[count % kMarks] = {second, total};
        g_markCount.store(count + 1, std::memory_order_release);
    }
    if (aSize > kRingBytes)
    {
        aData += aSize - kRingBytes;
        aSize = kRingBytes;
    }
    const size_t pos = static_cast<size_t>(total % kRingBytes);
    const size_t head = std::min(aSize, kRingBytes - pos);
    std::memcpy(g_ring + pos, aData, head);
    if (head < aSize)
        std::memcpy(g_ring, aData + head, aSize - head);
    g_ringTotal.store(total + aSize, std::memory_order_release);
}

std::filesystem::path Utf8Path(const std::string& aUtf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(aUtf8.data()), aUtf8.size()));
}

std::string ToUtf8(const std::filesystem::path& aPath)
{
    const std::u8string text = aPath.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string Sanitize(const std::string& aText)
{
    std::string out;
    for (const char c : aText)
        out += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ? c
                                                                                                                 : '_';
    return out.empty() ? std::string("snapshot") : out.substr(0, 32);
}

/// At most aKeep files <aPrefix>*.log in the directory: the oldest (names sort by time) go.
void Prune(const std::filesystem::path& aDir, const char* aPrefix = "nightcoop-last-", size_t aKeep = kRecentKeep)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> snapshots;
    for (fs::directory_iterator it(aDir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = ToUtf8(it->path().filename());
        if (name.rfind(aPrefix, 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".log")
            snapshots.push_back(it->path());
    }
    if (snapshots.size() <= aKeep)
        return;
    std::sort(snapshots.begin(), snapshots.end());
    for (size_t i = 0; i + aKeep < snapshots.size(); ++i)
        fs::remove(snapshots[i], ec);
}

/// The previous run's log moves to logs/nightcoop-session-<time of its last write>.log, to be read after a restart.
/// Under g_mutex, before the new one opens.
void KeepPreviousSession(const std::string& aDirectory)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path old = Utf8Path(aDirectory) / "nightcoop.log";
    if (!fs::exists(old, ec) || fs::file_size(old, ec) <= 3)
        return;
    const auto written = fs::last_write_time(old, ec);
    if (ec)
        return;
    // File clock -> system clock without clock_cast (not available on every platform the core builds on)
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char name[64];
    std::snprintf(name, sizeof(name), "nightcoop-session-%04d-%02d-%02d-%02d-%02d-%02d.log", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    const fs::path dir = Utf8Path(aDirectory) / "logs";
    fs::create_directories(dir, ec);
    fs::rename(old, dir / name, ec);
    if (!ec)
        Prune(dir, "nightcoop-session-", kSessionKeep);
}

// Lines-per-second limit (a flood from one place) and flushing not on every line: under g_mutex
int64_t g_floodSecond = 0;
uint32_t g_floodLines = 0;
uint32_t g_floodDropped = 0;
std::chrono::steady_clock::time_point g_lastFlush{};
} // namespace

const char* LevelName(Level aLevel)
{
    switch (aLevel)
    {
    case Level::Trace: return "trace";
    case Level::Info:  return "info ";
    case Level::Warn:  return "warn ";
    case Level::Error: return "error";
    }
    return "?????";
}

void Init(const std::string& aDirectory)
{
    std::lock_guard lock(g_mutex);
    if (g_file)
        return;

    const std::string path = aDirectory + "/nightcoop.log";
    g_recentDir = aDirectory + "/logs";
    KeepPreviousSession(aDirectory);

    // Strictly binary mode: the tempting "w, ccs=UTF-8" makes an MSVC stream wide-oriented, and a narrow fprintf into it is
    // undefined behaviour — the CRT goes to __fastfail and the process dies with 0xC0000409 without a word. The sources are
    // UTF-8 already, so the BOM written by hand is enough.
    g_file = std::fopen(path.c_str(), "wb");
    if (g_file)
        std::fwrite("\xEF\xBB\xBF", 1, 3, g_file);
}

void Shutdown()
{
    std::lock_guard lock(g_mutex);
    if (g_file)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_sink = nullptr;
}

void SetSink(Sink aSink)
{
    std::lock_guard lock(g_mutex);
    g_sink = std::move(aSink);
}

namespace
{
/// The log lock for no longer than aWaitMs: a hang watchdog must not get stuck on a lock held by a stuck thread.
bool LockFor(std::unique_lock<std::mutex>& aLock, uint32_t aWaitMs)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(aWaitMs);
    while (!aLock.try_lock())
    {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void WriteLocked(Level aLevel, std::string_view aMessage);

std::atomic<bool> g_hideAddresses{false}; ///< 23.10 (log_hide_ips): IPv4 addresses as <ip>

bool Digit(char aChar)
{
    return aChar >= '0' && aChar <= '9';
}

/// 23.10 (log_hide_ips): a dotted IPv4 address — four parts 0-255, not inside a longer run of digits and dots (a version like
/// 3.0.88.46235 or 1.2.3.4.5 is not one) — becomes "<ip>"; 127.x.x.x and 0.0.0.0 stay. A port after it stays too.
void HideAddressesIn(std::string& aText)
{
    if (aText.find('.') == std::string::npos)
        return;
    std::string out;
    size_t last = 0;
    bool changed = false;
    const size_t n = aText.size();
    size_t i = 0;
    while (i < n)
    {
        if (!Digit(aText[i]) || (i > 0 && (Digit(aText[i - 1]) || aText[i - 1] == '.')))
        {
            ++i;
            continue;
        }
        size_t j = i;
        int parts = 0;
        int first = -1;
        bool zero = true;
        bool ok = true;
        while (parts < 4)
        {
            int value = 0;
            int digits = 0;
            while (j < n && Digit(aText[j]) && digits <= 3)
            {
                value = value * 10 + (aText[j] - '0');
                ++j;
                ++digits;
            }
            if (digits == 0 || digits > 3 || value > 255)
            {
                ok = false;
                break;
            }
            if (parts == 0)
                first = value;
            zero = zero && value == 0;
            ++parts;
            if (parts == 4)
                break;
            if (j < n && aText[j] == '.')
                ++j;
            else
            {
                ok = false;
                break;
            }
        }
        const bool longer = j < n && (Digit(aText[j]) || (aText[j] == '.' && j + 1 < n && Digit(aText[j + 1])));
        if (!ok || longer)
        {
            i = std::max(j, i + 1);
            continue;
        }
        if (first != 127 && !zero)
        {
            out.append(aText, last, i - last);
            out += "<ip>";
            last = j;
            changed = true;
        }
        i = j;
    }
    if (!changed)
        return;
    out.append(aText, last, std::string::npos);
    aText.swap(out);
}
} // namespace

void SetHideAddresses(bool aOn)
{
    g_hideAddresses.store(aOn, std::memory_order_relaxed);
}

bool HidingAddresses()
{
    return g_hideAddresses.load(std::memory_order_relaxed);
}

void Write(Level aLevel, std::string_view aMessage)
{
    std::lock_guard lock(g_mutex);
    WriteLocked(aLevel, aMessage);
}

bool TryWrite(Level aLevel, std::string_view aMessage, uint32_t aWaitMs)
{
    std::unique_lock lock(g_mutex, std::defer_lock);
    if (!LockFor(lock, aWaitMs))
        return false;
    WriteLocked(aLevel, aMessage);
    return true;
}

namespace
{
/// Under g_mutex.
void WriteLocked(Level aLevel, std::string_view aMessage)
{
    // Flood: past kMaxLinesPerSecond in a second lines are not written (except errors); the next second starts with how
    // many were dropped
    const int64_t second = NowSecond();
    if (second != g_floodSecond)
    {
        if (g_floodDropped)
        {
            const std::string note = std::format("[{}] [warn ] log: {} line(s) dropped in the last second (more than {} a "
                                                 "second)\n",
                                                 Timestamp(), g_floodDropped, kMaxLinesPerSecond);
            RingAppend(note.data(), note.size());
            if (g_file)
                std::fwrite(note.data(), 1, note.size(), g_file);
        }
        g_floodSecond = second;
        g_floodLines = 0;
        g_floodDropped = 0;
    }
    if (++g_floodLines > kMaxLinesPerSecond && aLevel != Level::Error && !PastFloodLimit(aMessage))
    {
        ++g_floodDropped;
        return;
    }

    // 23.10 (log_hide_ips): players' addresses do not reach the file, the ring or the sink
    std::string message(aMessage);
    if (g_hideAddresses.load(std::memory_order_relaxed))
        HideAddressesIn(message);

    std::string line = std::format("[{}] [{}] ", Timestamp(), LevelName(aLevel));
    line.append(message);
    line += '\n';
    RingAppend(line.data(), line.size());

    if (g_file)
    {
        std::fwrite(line.data(), 1, line.size(), g_file);
        // Warnings and errors go to disk at once, the rest at most every 100 ms (a crash snapshot comes from the ring)
        const auto now = std::chrono::steady_clock::now();
        if (aLevel >= Level::Warn || now - g_lastFlush >= std::chrono::milliseconds(100))
        {
            std::fflush(g_file);
            g_lastFlush = now;
        }
    }

    if (g_sink)
        g_sink(aLevel, message.c_str()); // owned: string_view does not guarantee a terminating zero, and sinks expect one
}
} // namespace

namespace
{
std::mutex g_dueMutex;
std::unordered_map<uint64_t, uint64_t> g_due; ///< line key -> when it was last written (steady clock, ms)
} // namespace

bool Due(uint64_t aKey, uint64_t aEveryMs)
{
    using namespace std::chrono;
    const auto now = static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    std::lock_guard lock(g_dueMutex);
    if (g_due.size() > 65536)
        g_due.clear();
    const auto [it, fresh] = g_due.emplace(aKey, now);
    if (fresh)
        return true;
    if (now - it->second < aEveryMs)
        return false;
    it->second = now;
    return true;
}

// ---------------------------------------------------------------- the last minutes

RecentSpan RecentUnlocked(uint32_t aSeconds)
{
    RecentSpan out;
    const uint64_t total = g_ringTotal.load(std::memory_order_acquire);
    const uint64_t count = g_markCount.load(std::memory_order_acquire);
    const int64_t from = NowSecond() - static_cast<int64_t>(aSeconds);
    uint64_t start = total;
    const uint64_t have = std::min<uint64_t>(count, kMarks);
    for (uint64_t i = count - have; i < count; ++i)
    {
        const Mark mark = g_marks[i % kMarks];
        if (mark.second >= from)
        {
            start = mark.total;
            break;
        }
    }
    if (start > total)
        start = total; // a mark was added while we were reading
    if (total - start > kRingBytes)
    {
        // The ring has already overwritten the start of the window: from the first whole line of what is left
        start = total - kRingBytes;
        for (uint64_t at = start; at < total; ++at)
            if (g_ring[at % kRingBytes] == '\n')
            {
                start = at + 1;
                break;
            }
    }
    const size_t size = static_cast<size_t>(total - start);
    const size_t pos = static_cast<size_t>(start % kRingBytes);
    out.first = g_ring + pos;
    out.firstSize = std::min(size, kRingBytes - pos);
    if (out.firstSize < size)
    {
        out.second = g_ring;
        out.secondSize = size - out.firstSize;
    }
    return out;
}

void SetRecentHeader(const std::string& aText)
{
    std::lock_guard lock(g_mutex);
    const size_t size = std::min(aText.size(), sizeof(g_header) - 1);
    std::memcpy(g_header, aText.data(), size);
    g_header[size] = '\0';
}

const char* RecentHeaderUnlocked()
{
    return g_header;
}

std::string RecentDirectory()
{
    std::lock_guard lock(g_mutex);
    return g_recentDir;
}

namespace
{
/// The ring for the last aSeconds, the snapshot directory and the header, copied into own strings. Under g_mutex (or
/// without it — SaveRecentNoWait).
void CopyRecent(uint32_t aSeconds, std::string& aBody, std::string& aDirectory, std::string& aHeader)
{
    const RecentSpan span = RecentUnlocked(aSeconds);
    aBody.reserve(span.firstSize + span.secondSize);
    aBody.append(span.first, span.firstSize);
    if (span.secondSize)
        aBody.append(span.second, span.secondSize);
    aDirectory = g_recentDir;
    aHeader = g_header;
}

std::string WriteRecentFile(const std::string& aBody, const std::string& aDirectory, const std::string& aHeader,
                            const std::string& aReason, const std::string& aNote, uint32_t aSeconds,
                            std::string* aError);
} // namespace

std::string SaveRecent(const std::string& aReason, const std::string& aNote, uint32_t aSeconds, std::string* aError)
{
    std::string body;
    std::string directory;
    std::string header;
    {
        std::lock_guard lock(g_mutex);
        CopyRecent(aSeconds, body, directory, header);
    }
    return WriteRecentFile(body, directory, header, aReason, aNote, aSeconds, aError);
}

std::string SaveRecentNoWait(const std::string& aReason, const std::string& aNote, uint32_t aWaitMs, uint32_t aSeconds,
                             std::string* aError)
{
    std::string body;
    std::string directory;
    std::string header;
    {
        std::unique_lock lock(g_mutex, std::defer_lock);
        // The lock was not taken within aWaitMs (a stuck thread holds it): copy the ring without it, as the crash handler does
        // (the line at the edge may be incomplete); the directory and the header are set at start and never change
        static_cast<void>(LockFor(lock, aWaitMs));
        CopyRecent(aSeconds, body, directory, header);
    }
    return WriteRecentFile(body, directory, header, aReason, aNote, aSeconds, aError);
}

namespace
{
std::string WriteRecentFile(const std::string& aBody, const std::string& aDirectory, const std::string& aHeader,
                            const std::string& aReason, const std::string& aNote, uint32_t aSeconds,
                            std::string* aError)
{
    namespace fs = std::filesystem;
    const std::string& body = aBody;
    const std::string& directory = aDirectory;
    const std::string& header = aHeader;
    if (directory.empty())
    {
        if (aError)
            *aError = "the log is not open";
        return {};
    }
    std::error_code ec;
    const fs::path dir = Utf8Path(directory);
    fs::create_directories(dir, ec);
    std::tm local{};
    LocalTime(local);
    char stamp[64]; // room to spare: g++ counts int fields as up to 11 characters wide
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d-%02d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    const std::string base = std::string("nightcoop-last-") + stamp + "-" + Sanitize(aReason);
    fs::path path = dir / Utf8Path(base + ".log");
    // Two snapshots in one second: the second goes next to the first, not over it
    for (int n = 2; fs::exists(path, ec) && n < 100; ++n)
        path = dir / Utf8Path(std::format("{}-{}.log", base, n));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        if (aError)
            *aError = "cannot create " + ToUtf8(path);
        return {};
    }
    char when[64];
    std::snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    const std::string top = std::format("\xEF\xBB\xBFNightCoop: the last {} s of the log, saved {} ({}){}{}{}\n",
                                        aSeconds, when, aReason, header.empty() ? "" : ", ", header,
                                        aNote.empty() ? std::string() : " — " + aNote);
    file.write(top.data(), static_cast<std::streamsize>(top.size()));
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    file.close();
    if (!file)
    {
        if (aError)
            *aError = "cannot write " + ToUtf8(path);
        return {};
    }
    Prune(dir);
    return ToUtf8(path);
}
} // namespace
} // namespace nc::log

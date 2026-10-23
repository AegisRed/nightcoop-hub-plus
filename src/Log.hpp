#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// Logging for the mod and the hub. The module knows nothing of RED4ext or Windows: the same core builds into the console
// tools and the hub. An external sink (the RED4ext log, stderr) plugs in through SetSink.
namespace nc::log
{
enum class Level
{
    Trace,
    Info,
    Warn,
    Error
};

const char* LevelName(Level aLevel);

/// "No limit" line budget of a module: module line counters start from it and never run out. A flood from one place is
/// held back by the common kMaxLinesPerSecond in Write.
inline constexpr int32_t kNoLineLimit = 1 << 30;
/// At most this many lines a second: the rest are dropped, and the next second starts with a line saying how many.
inline constexpr uint32_t kMaxLinesPerSecond = 2000;

/// Lines that show whether the process is alive pass the flood limit: a flood from one place must not push them out
/// right when they matter.
inline bool PastFloodLimit(std::string_view aMessage)
{
    for (const std::string_view start : {"frame cost", "slow frames", "netstats", "hang watch"})
        if (aMessage.substr(0, start.size()) == start)
            return true;
    return false;
}
/// Logs of previous runs kept (nightcoop.log of the last run -> logs/nightcoop-session-<time>.log).
inline constexpr size_t kSessionKeep = 20;

/// Opens the log file (UTF-8 path). The previous run's log is not overwritten: it moves to
/// logs/nightcoop-session-<its time>.log.
void Init(const std::string& aDirectory);
void Shutdown();

/// Extra message sink. Called under the same mutex as the file write, so it need not care about races.
using Sink = std::function<void(Level, const char*)>;
void SetSink(Sink aSink);

/// 23.10 (log_hide_ips): dotted IPv4 addresses (players' public and local ones) are written as <ip> into the file, the ring
/// and the sink — players share their logs, the addresses are personal data. 127.x.x.x and 0.0.0.0 stay. Off by default:
/// the plugin turns it on, the hub and the tools keep addresses. Any thread.
void SetHideAddresses(bool aOn);
bool HidingAddresses();

void Write(Level aLevel, std::string_view aMessage);

/// Same, but waits for the log lock no longer than aWaitMs: false — not written (someone else holds the lock; a hang
/// watchdog must not get stuck on a lock held by a stuck thread). Any thread.
bool TryWrite(Level aLevel, std::string_view aMessage, uint32_t aWaitMs);

/// A line of one kind at most once per aEveryMs (key: place and subject, e.g. a caller address and an NPC number): a
/// stream of identical lines from hooks and ticks does not drown the log. Any thread.
bool Due(uint64_t aKey, uint64_t aEveryMs);

// ---------------------------------------------------------------- the last minutes

/// The last minutes of the log, kept in memory as a ring. A snapshot of them is a separate file in logs/ next to the log:
/// on request (NightCoopLogSave), on exit and on a crash.
inline constexpr uint32_t kRecentSeconds = 300;
/// Snapshots kept in logs/; older ones are deleted.
inline constexpr size_t kRecentKeep = 40;

/// Pieces of the ring for the last aSeconds. No lock and no allocation — for the crash handler: the line at the edge may
/// be incomplete.
struct RecentSpan
{
    const char* first = nullptr;
    size_t firstSize = 0;
    const char* second = nullptr;
    size_t secondSize = 0;
};
RecentSpan RecentUnlocked(uint32_t aSeconds);

/// Snapshot directory (UTF-8): logs/ next to the log, set by Init.
std::string RecentDirectory();

/// Snapshot header (build, protocol), set at start (up to 511 bytes).
void SetRecentHeader(const std::string& aText);
/// The same header without the lock, for the crash handler.
const char* RecentHeaderUnlocked();

/// Snapshot of the last aSeconds into logs/nightcoop-last-<date>-<time>-<aReason>.log. The ring is copied under the log
/// lock and the file is written without it. Returns the path (UTF-8), or empty with the reason in aError.
std::string SaveRecent(const std::string& aReason, const std::string& aNote, uint32_t aSeconds = kRecentSeconds,
                       std::string* aError = nullptr);

/// Same for the hang watchdog: waits for the log lock no longer than aWaitMs, otherwise copies the ring without it (as the
/// crash handler does: the line at the edge may be incomplete). Any thread.
std::string SaveRecentNoWait(const std::string& aReason, const std::string& aNote, uint32_t aWaitMs,
                             uint32_t aSeconds = kRecentSeconds, std::string* aError = nullptr);

template<class... Args>
void Trace(std::format_string<Args...> aFmt, Args&&... aArgs)
{
    Write(Level::Trace, std::format(aFmt, std::forward<Args>(aArgs)...));
}

template<class... Args>
void Info(std::format_string<Args...> aFmt, Args&&... aArgs)
{
    Write(Level::Info, std::format(aFmt, std::forward<Args>(aArgs)...));
}

template<class... Args>
void Warn(std::format_string<Args...> aFmt, Args&&... aArgs)
{
    Write(Level::Warn, std::format(aFmt, std::forward<Args>(aArgs)...));
}

template<class... Args>
void Error(std::format_string<Args...> aFmt, Args&&... aArgs)
{
    Write(Level::Error, std::format(aFmt, std::forward<Args>(aArgs)...));
}
} // namespace nc::log

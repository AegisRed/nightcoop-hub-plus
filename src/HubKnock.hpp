#pragma once

#include <atomic>
#include <cstdint>

/// The client's "knock" on a hub (see HubCookie.hpp), without ENet types so that it can sit as a field in transport headers.
namespace nc::hubcookie
{
/// Switch hub_cookie. Off: the client does not ask the hub for a pass (a hub that requires one will not let it in).
inline std::atomic<bool> g_ask{true};

struct Knock
{
    uint32_t hubIp = 0;   ///< hub address as in ENetAddress.host; the answer is taken from it only
    uint16_t hubPort = 0;
    uint32_t askedAt = 0; ///< enet_time_get() of the last ask
    uint32_t waitMs = 0;  ///< until the next ask
    uint32_t asks = 0;    ///< 0: not knocking (off, or not started yet)
    bool told = false;    ///< the hub answered
    const char* who = ""; ///< for the log: "relay", "hybrid"
};
} // namespace nc::hubcookie

#pragma once

#include <atomic>

/// Switch of the pre-ENet guard in the game (EnetGuard.hpp), kept apart so that code that only flips it needs no enet.h.
namespace nc::enetguard
{
/// Off: datagrams go straight into ENet. The hub has its own switch (--guard off, HubServer::Limits::guard).
inline std::atomic<bool> g_on{true};
} // namespace nc::enetguard

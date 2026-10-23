#pragma once

#include "HubKnock.hpp"
#include "Log.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

/// A pass for connecting to the hub.
///
/// Why: ENet hands out a connection slot on every CONNECT from a new address and holds it for 15–30 s until the answer
/// times out. UDP source addresses can be forged, so a hundred CONNECTs a second from made-up addresses would take all the
/// hub's slots and honest players would get "the hub does not answer"; a per-address limit does not help, every address is
/// different.
///
/// How: the hub issues a pass — 32 bits from the asker's address and port, the hub's secret and the current time bucket —
/// and remembers nothing. The client puts the pass into the data word of its CONNECT (ENet has exactly one, unused by the
/// hub). The hub looks at each datagram before ENet does (ENetHost::intercept): a CONNECT without a valid pass gets no
/// slot. The pass is sent to the real address only, so whoever forges an address never learns it.
///
/// Compatibility: the client starts its ENet connection at once (a CONNECT with word 0) and asks for the pass from the
/// same socket; the hub's answer is written into the CONNECT not yet acknowledged, and ENet's resend (after ~0.5 s)
/// carries it. An older hub drops the ask (not its packet) and lets the client in on the first CONNECT. A client without
/// a pass is let in while the pass is not required (HubServer::Limits::cookie).
///
/// The ask is not shorter than the answer: answering a forged address, the hub does not amplify anyone's traffic.
namespace nc::hubcookie
{
/// The first two bytes are the ENet header "peer 0xFFE, no flags": an ENet host without our intercept (an older hub, a
/// client) drops such a datagram itself — it has no slot with that number, or the address does not match.
inline constexpr uint8_t kAskMagic[8] = {0x0F, 0xFE, 'N', 'C', 'c', 'k', '?', 1};
inline constexpr uint8_t kTellMagic[8] = {0x0F, 0xFE, 'N', 'C', 'c', 'k', '!', 1};
inline constexpr size_t kAskSize = 24;
inline constexpr size_t kTellSize = 12;
/// Time bucket of a pass. A pass of this or the previous bucket is valid: 32–64 s, longer than the client's ENet keeps
/// resending CONNECT before the first answer (a few seconds are needed).
inline constexpr uint64_t kBucketMs = 32000;
/// Pause between the client's asks: 250 ms, then doubling, at least every 4 s — while ENet waits for the CONNECT answer.
inline constexpr uint32_t kAskFirstMs = 250;
inline constexpr uint32_t kAskMaxMs = 4000;

using Key = std::array<uint8_t, 16>;

/// SipHash-2-4: a short keyed hash, the issued passes do not reveal the secret.
inline uint64_t SipHash24(const Key& aKey, const uint8_t* aData, size_t aSize)
{
    auto load = [](const uint8_t* aAt)
    {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i)
            v = (v << 8) | aAt[i];
        return v;
    };
    auto rotl = [](uint64_t aValue, int aBits) { return (aValue << aBits) | (aValue >> (64 - aBits)); };
    const uint64_t k0 = load(aKey.data());
    const uint64_t k1 = load(aKey.data() + 8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ull, v1 = k1 ^ 0x646f72616e646f6dull;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ull, v3 = k1 ^ 0x7465646279746573ull;
    auto round = [&]
    {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    size_t at = 0;
    for (; at + 8 <= aSize; at += 8)
    {
        const uint64_t m = load(aData + at);
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    uint64_t last = static_cast<uint64_t>(aSize & 0xFF) << 56;
    for (size_t i = 0; at + i < aSize; ++i)
        last |= static_cast<uint64_t>(aData[at + i]) << (8 * i);
    v3 ^= last;
    round();
    round();
    v0 ^= last;
    v2 ^= 0xFF;
    for (int i = 0; i < 4; ++i)
        round();
    return v0 ^ v1 ^ v2 ^ v3;
}

/// A pass for an address and port in a time bucket. Never zero: zero in CONNECT means "no pass".
inline uint32_t Make(const Key& aKey, uint32_t aIp, uint16_t aPort, uint64_t aBucket)
{
    uint8_t text[14];
    std::memcpy(text, &aIp, 4);
    std::memcpy(text + 4, &aPort, 2);
    std::memcpy(text + 6, &aBucket, 8);
    const uint64_t hash = SipHash24(aKey, text, sizeof(text));
    const uint32_t cookie = static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(hash >> 32);
    return cookie ? cookie : 1;
}

inline bool Good(const Key& aKey, uint32_t aIp, uint16_t aPort, uint64_t aNowMs, uint32_t aCookie)
{
    const uint64_t bucket = aNowMs / kBucketMs;
    return aCookie != 0 && (aCookie == Make(aKey, aIp, aPort, bucket) || aCookie == Make(aKey, aIp, aPort, bucket - 1));
}

inline bool IsAsk(const uint8_t* aData, size_t aSize)
{
    return aSize >= kAskSize && std::memcmp(aData, kAskMagic, sizeof(kAskMagic)) == 0;
}

inline bool ReadTell(const uint8_t* aData, size_t aSize, uint32_t& aCookie)
{
    if (aSize != kTellSize || std::memcmp(aData, kTellMagic, sizeof(kTellMagic)) != 0)
        return false;
    std::memcpy(&aCookie, aData + sizeof(kTellMagic), 4);
    return true;
}

inline void WriteTell(uint8_t aOut[kTellSize], uint32_t aCookie)
{
    std::memcpy(aOut, kTellMagic, sizeof(kTellMagic));
    std::memcpy(aOut + sizeof(kTellMagic), &aCookie, 4);
}

/// An ENet datagram "from someone without a connection yet" (peer 0xFFF) with CONNECT as its first command — the only
/// one ENet gives a slot for (protocol.c: without a connection it takes only CONNECT, and only as the first command).
/// aWord is its data word. Everything else ENet drops without a slot itself.
inline bool ReadConnect(const uint8_t* aData, size_t aSize, uint32_t& aWord)
{
    if (aSize < 2)
        return false;
    const uint16_t word = static_cast<uint16_t>((aData[0] << 8) | aData[1]);
    if ((word & 0x0FFF) != ENET_PROTOCOL_MAXIMUM_PEER_ID || (word & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED))
        return false;
    const size_t header = (word & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? 4 : 2;
    if (aSize < header + sizeof(ENetProtocolConnect) ||
        (aData[header] & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_CONNECT)
        return false;
    ENetProtocolConnect connect;
    std::memcpy(&connect, aData + header, sizeof(connect));
    aWord = ENET_NET_TO_HOST_32(connect.data);
    return true;
}

// ---------------------------------------------------------------- client

inline void Ask(Knock& aKnock, ENetHost* aHost)
{
    uint8_t out[kAskSize] = {};
    std::memcpy(out, kAskMagic, sizeof(kAskMagic));
    ENetAddress hub{};
    hub.host = aKnock.hubIp;
    hub.port = aKnock.hubPort;
    ENetBuffer buffer;
    buffer.data = out;
    buffer.dataLength = sizeof(out);
    enet_socket_send(aHost->socket, &hub, &buffer, 1);
    aKnock.askedAt = enet_time_get();
    aKnock.waitMs = std::min(kAskFirstMs << std::min<uint32_t>(aKnock.asks, 4), kAskMaxMs);
    ++aKnock.asks;
}

/// Right after enet_host_connect to the hub: the first ask, from the same socket.
inline void Begin(Knock& aKnock, ENetHost* aHost, const ENetAddress& aHub, const char* aWho)
{
    aKnock = Knock{};
    aKnock.who = aWho;
    if (!aHost || !g_ask.load(std::memory_order_relaxed))
        return;
    aKnock.hubIp = aHub.host;
    aKnock.hubPort = aHub.port;
    Ask(aKnock, aHost);
}

/// Every turn: the ask repeats until the hub answers with a pass or to the CONNECT (ENet is still waiting).
inline void Tick(Knock& aKnock, ENetHost* aHost, const ENetPeer* aHubPeer)
{
    if (aKnock.asks == 0 || aKnock.told || !aHost || !aHubPeer || aHubPeer->state != ENET_PEER_STATE_CONNECTING)
        return;
    if (enet_time_get() - aKnock.askedAt >= aKnock.waitMs)
        Ask(aKnock, aHost);
}

/// The pass goes into the CONNECT that ENet does not consider delivered yet (waiting to be sent or acknowledged).
inline size_t PutIntoConnect(ENetPeer* aPeer, uint32_t aCookie)
{
    size_t put = 0;
    for (ENetList* list : {&aPeer->sentReliableCommands, &aPeer->outgoingSendReliableCommands, &aPeer->outgoingCommands})
    {
        for (ENetListIterator it = enet_list_begin(list); it != enet_list_end(list); it = enet_list_next(it))
        {
            auto* command = reinterpret_cast<ENetOutgoingCommand*>(it);
            if ((command->command.header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_CONNECT)
                continue;
            command->command.connect.data = ENET_HOST_TO_NET_32(aCookie);
            ++put;
        }
    }
    return put;
}

/// From the client's intercept (ENetHost::intercept): true — the datagram was the hub's answer to our ask and is handled.
inline bool OnDatagram(Knock& aKnock, ENetHost* aHost, ENetPeer* aHubPeer)
{
    if (aKnock.asks == 0 || aHost->receivedAddress.host != aKnock.hubIp || aHost->receivedAddress.port != aKnock.hubPort)
        return false;
    uint32_t cookie = 0;
    if (!ReadTell(aHost->receivedData, aHost->receivedDataLength, cookie))
        return false;
    if (aKnock.told)
        return true; // an answer to a repeated ask: the pass is already in
    aKnock.told = true;
    const bool waiting = aHubPeer && aHubPeer->state == ENET_PEER_STATE_CONNECTING;
    const size_t put = waiting ? PutIntoConnect(aHubPeer, cookie) : 0;
    log::Info("{}: the hub gave a pass for our address (asked {} time(s)){}", aKnock.who, aKnock.asks,
              put ? " — the CONNECT still on its way carries it from the next resend"
                  : " — the hub has let us in without it already");
    return true;
}

/// Intercept for an ENet host with a single job, the connection to the hub (RelayTransport, NetCheck). An ENet intercept
/// carries no pointer to its owner and is called only from enet_host_service on its own thread, so the owner is
/// remembered for that call. aPeer points at the owner's field: the hub connection changes when moving to a thread's port.
struct Serving
{
    Knock* knock = nullptr;
    ENetPeer* const* peer = nullptr;
};
inline thread_local Serving t_serving{};

inline int ENET_CALLBACK ClientIntercept(ENetHost* aHost, ENetEvent*)
{
    const Serving serving = t_serving;
    return serving.knock && OnDatagram(*serving.knock, aHost, serving.peer ? *serving.peer : nullptr) ? 1 : 0;
}

struct Scope
{
    Scope(Knock& aKnock, ENetPeer* const* aPeer) { t_serving = Serving{&aKnock, aPeer}; }
    ~Scope() { t_serving = Serving{}; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace nc::hubcookie

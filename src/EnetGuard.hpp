#pragma once

#include "EnetGuardSwitch.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

/// A limit on ENet's receive-side memory, enforced before ENet allocates it.
///
/// Why: ENet keeps in a connection's queues everything that has arrived but is not delivered yet: the reassembly of a
/// fragmented packet (its whole buffer is allocated on the first fragment, up to maximumPacketSize), reliable commands
/// out of order and unreliable ones "for a future sequence number". ENet has one limit, maximumWaitingData per
/// connection, and it is weaker than it looks (vendor/enet/peer.c, enet_peer_queue_incoming_command):
///  * it checks what is already waiting, without the new packet: with 8 MiB waiting, one more packet up to
///    maximumPacketSize still gets in;
///  * it counts data bytes only: a command without data (length 0 is legal) costs ENet nothing but costs the heap a
///    command and a packet record (~190 B). ENet keeps any number of unreliable commands "for a future number", so a
///    stream of them from any connection grows memory without bound, unseen by maximumWaitingData;
///  * there is no per-process limit: there are many connections (hundreds on a hub), each with its own.
/// Anyone on the internet has a connection to the hub (before any session), and anyone who reaches the port has one to
/// a host in direct mode.
///
/// How: the datagram is parsed in ENet's intercept (ENetHost::intercept) the same way ENet will parse it next, only
/// without writing anything: which of its commands are delivered at once (the next reliable number, an unreliable one of
/// the current number, a fragment of a started reassembly) and which stay waiting (out of order, for a future number, a
/// new reassembly). What waits costs its bytes plus a record (kEntryCost). A datagram after which a connection's waiting
/// data would exceed its limit or the host's shared pool is dropped whole before ENet: it is not acknowledged, and an
/// honest sender resends it later when the queue drains. What is delivered at once is never limited: a command that
/// closes a gap in the queue always passes, so an honest connection never stalls because of someone else's flood.
///
/// Limits (Budget): perPeer — all waiting data of one connection (strictly, with the new datagram); guaranteed — each
/// connection's own share, outside the pool; pool — the host's shared pool above the shares; preAuth — for a connection
/// not in a session yet (hub): no fragments at all and no more than preAuth waiting. Worst case for a host:
/// connections × guaranteed + pool; for a connection: perPeer.
///
/// Unlike ENet: a datagram with one command too many is dropped whole (with the acknowledgements in it) — ENet resends
/// them itself. The parse changes no byte and decides nothing for ENet: anything unusual (not our connection,
/// compression, a wrong length) goes to ENet as it is, and ENet drops it itself.
namespace nc::enetguard
{
/// Cost of one waiting command beyond its data: ENetIncomingCommand (~96 B on x64) and ENetPacket (~48 B) with heap
/// headers; 192 as an upper estimate.
inline constexpr size_t kEntryCost = 192;

/// The largest message any receiver accepts (the biggest kind, an animation stream message, stays under 7.8 MiB with all
/// its wrappers). No sender builds a bigger packet.
inline constexpr size_t kMaxMessage = 8u * 1024u * 1024u;

struct Budget
{
    size_t perPeer = 0;    ///< 0: no check
    size_t guaranteed = 0; ///< each connection's own share, outside the pool
    size_t pool = 0;       ///< the host's shared pool above the shares; 0: no pool (perPeer only)
    size_t preAuth = 0;    ///< a connection outside a session: no more waiting than this, no fragments
    size_t maxMessage = 0; ///< a bigger new reassembly is refused; 0: ENet's limit (maximumPacketSize)
};

/// Limits of the mod's own ENet hosts: few connections (up to 24 slots), while the one to the hub carries all of a
/// host's guests in one queue. An honest connection waits for what arrived out of order after a lost datagram (hundreds
/// of KiB) plus one big packet being reassembled. Worst case for a host: 24 × 1 MiB + 64 MiB.
inline Budget GameBudget()
{
    Budget budget;
    budget.perPeer = 16u * 1024u * 1024u;
    budget.guaranteed = 1024u * 1024u;
    budget.pool = 64u * 1024u * 1024u;
    budget.maxMessage = kMaxMessage;
    return budget;
}

/// What a datagram brings to ENet (parsed without writing)
struct Bring
{
    ENetPeer* peer = nullptr; ///< the connection it is for; nullptr: not our business (ENet decides)
    size_t held = 0;          ///< cost of what stays waiting: data + kEntryCost per command (+ the fragment map)
    size_t heldCommands = 0;
    size_t biggest = 0;       ///< the biggest new fragment reassembly, bytes
    bool fragments = false;   ///< has fragment commands (new or continuing)
};

enum class Verdict : uint8_t
{
    Pass,     ///< hand to ENet
    PreAuth,  ///< a connection outside a session: fragments, or waiting data above preAuth
    PeerFull, ///< the connection's waiting data would exceed perPeer
    PoolFull, ///< the host's shared pool is full
    TooBig,   ///< a new reassembly bigger than maxMessage
};

inline const char* VerdictName(Verdict aVerdict)
{
    switch (aVerdict)
    {
    case Verdict::PreAuth:
        return "a connection outside a session sends fragments or holds too much";
    case Verdict::PeerFull:
        return "the connection holds its limit of undelivered data";
    case Verdict::PoolFull:
        return "the shared pool of undelivered data is full";
    case Verdict::TooBig:
        return "a fragmented packet bigger than any message of the game";
    default:
        return "pass";
    }
}

namespace detail
{
inline uint16_t Be16(const uint8_t* aAt)
{
    return static_cast<uint16_t>((aAt[0] << 8) | aAt[1]);
}

inline uint32_t Be32(const uint8_t* aAt)
{
    return (static_cast<uint32_t>(aAt[0]) << 24) | (static_cast<uint32_t>(aAt[1]) << 16) |
           (static_cast<uint32_t>(aAt[2]) << 8) | aAt[3];
}

/// A number inside the channel's window of reliable numbers, as in ENet (peer.c, enet_peer_queue_incoming_command): a
/// command outside the window is dropped and takes no memory.
inline bool InWindow(uint16_t aSeq, uint16_t aIncoming)
{
    uint16_t window = aSeq / ENET_PEER_RELIABLE_WINDOW_SIZE;
    const uint16_t current = aIncoming / ENET_PEER_RELIABLE_WINDOW_SIZE;
    if (aSeq < aIncoming)
        window += ENET_PEER_RELIABLE_WINDOWS;
    return !(window < current || window >= current + ENET_PEER_FREE_RELIABLE_WINDOWS - 1);
}

inline size_t FragmentMapBytes(uint32_t aCount)
{
    return (static_cast<size_t>(aCount) + 31) / 32 * sizeof(uint32_t);
}

/// A started reassembly of a reliable packet with this start number, searched as ENet does (protocol.c,
/// enet_protocol_handle_send_fragment): from the end of the queue, with the same comparisons. 1: found, 0: none,
/// -1: ENet will refuse (under this number there is no fragment, or one of a different length).
inline int FindReliableStart(const ENetChannel& aChannel, uint16_t aStart, uint32_t aTotal, uint32_t aCount)
{
    const ENetList* list = &aChannel.incomingReliableCommands;
    for (const ENetListNode* node = list->sentinel.previous; node != &list->sentinel; node = node->previous)
    {
        const auto* incoming = reinterpret_cast<const ENetIncomingCommand*>(node);
        if (aStart >= aChannel.incomingReliableSequenceNumber)
        {
            if (incoming->reliableSequenceNumber < aChannel.incomingReliableSequenceNumber)
                continue;
        }
        else if (incoming->reliableSequenceNumber >= aChannel.incomingReliableSequenceNumber)
            break;
        if (incoming->reliableSequenceNumber <= aStart)
        {
            if (incoming->reliableSequenceNumber < aStart)
                break;
            if ((incoming->command.header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_FRAGMENT ||
                !incoming->packet || aTotal != incoming->packet->dataLength || aCount != incoming->fragmentCount)
                return -1;
            return 1;
        }
    }
    return 0;
}

/// The same for an unreliable reassembly (enet_protocol_handle_send_unreliable_fragment)
inline int FindUnreliableStart(const ENetChannel& aChannel, uint16_t aReliable, uint16_t aStart, uint32_t aTotal,
                               uint32_t aCount)
{
    const ENetList* list = &aChannel.incomingUnreliableCommands;
    for (const ENetListNode* node = list->sentinel.previous; node != &list->sentinel; node = node->previous)
    {
        const auto* incoming = reinterpret_cast<const ENetIncomingCommand*>(node);
        if (aReliable >= aChannel.incomingReliableSequenceNumber)
        {
            if (incoming->reliableSequenceNumber < aChannel.incomingReliableSequenceNumber)
                continue;
        }
        else if (incoming->reliableSequenceNumber >= aChannel.incomingReliableSequenceNumber)
            break;
        if (incoming->reliableSequenceNumber < aReliable)
            break;
        if (incoming->reliableSequenceNumber > aReliable)
            continue;
        if (incoming->unreliableSequenceNumber <= aStart)
        {
            if (incoming->unreliableSequenceNumber < aStart)
                break;
            if ((incoming->command.header.command & ENET_PROTOCOL_COMMAND_MASK) !=
                    ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT ||
                !incoming->packet || aTotal != incoming->packet->dataLength || aCount != incoming->fragmentCount)
                return -1;
            return 1;
        }
    }
    return 0;
}
} // namespace detail

/// Parses datagram aData (as host aHost received it from aFrom) without writing. False: not ours to judge (CONNECT, an
/// unknown or unconnected peer, a foreign address, compression) — ENet decides.
inline bool Inspect(const ENetHost& aHost, const uint8_t* aData, size_t aSize, const ENetAddress& aFrom, Bring& aOut)
{
    aOut = Bring{};
    if (!aData || aSize < 2)
        return false;
    const uint16_t word = detail::Be16(aData);
    const uint16_t flags = word & ENET_PROTOCOL_HEADER_FLAG_MASK;
    const uint8_t session = static_cast<uint8_t>((word & ENET_PROTOCOL_HEADER_SESSION_MASK) >>
                                                 ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    const uint16_t id = word & static_cast<uint16_t>(~(ENET_PROTOCOL_HEADER_FLAG_MASK | ENET_PROTOCOL_HEADER_SESSION_MASK));
    if (id == ENET_PROTOCOL_MAXIMUM_PEER_ID || id >= aHost.peerCount || (flags & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED))
        return false;
    ENetPeer* peer = &aHost.peers[id];
    // ENet takes data only from a connected peer (protocol.c, enet_protocol_handle_send_*) and only from its address. A
    // connection still in its handshake is judged too: an acknowledgement at the start of the datagram makes it connected,
    // and ENet takes the data commands after it in the same datagram — they would bypass the "outside a session" limit.
    // ENet does not listen to a removed or zombie peer.
    if (peer->state == ENET_PEER_STATE_DISCONNECTED || peer->state == ENET_PEER_STATE_ZOMBIE || !peer->channels)
        return false;
    if ((aFrom.host != peer->address.host || aFrom.port != peer->address.port) && peer->address.host != ENET_HOST_BROADCAST)
        return false;
    if (peer->outgoingPeerID < ENET_PROTOCOL_MAXIMUM_PEER_ID && session != peer->incomingSessionID)
        return false;
    size_t pos = (flags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? sizeof(ENetProtocolHeader)
                                                                : offsetof(ENetProtocolHeader, sentTime);
    if (aHost.checksum)
        pos += sizeof(uint32_t);
    aOut.peer = peer;

    // What ENet will deliver during this same datagram: the next reliable number per channel and "the channel's unreliable
    // queue is empty". Our hosts have two channels; beyond kSim the order is not tracked (everything counts as waiting).
    constexpr size_t kSim = 8;
    std::array<uint16_t, kSim> next{};
    std::array<bool, kSim> unreliableEmpty{};
    const size_t simulated = std::min<size_t>(peer->channelCount, kSim);
    for (size_t c = 0; c < simulated; ++c)
    {
        next[c] = peer->channels[c].incomingReliableSequenceNumber;
        unreliableEmpty[c] = enet_list_empty(&peer->channels[c].incomingUnreliableCommands);
    }
    // New reassemblies in this datagram (several fragments of one count once)
    struct Started
    {
        uint8_t channel;
        bool reliable;
        uint16_t reliableSeq;
        uint16_t start;
    };
    std::array<Started, 8> started{};
    size_t startedCount = 0;
    auto hold = [&](size_t aBytes)
    {
        aOut.held += aBytes + kEntryCost;
        ++aOut.heldCommands;
    };

    while (pos + sizeof(ENetProtocolCommandHeader) <= aSize)
    {
        const uint8_t* command = aData + pos;
        const uint8_t number = command[0] & ENET_PROTOCOL_COMMAND_MASK;
        if (number >= ENET_PROTOCOL_COMMAND_COUNT)
            break;
        const size_t commandSize = enet_protocol_command_size(number);
        if (commandSize == 0 || pos + commandSize > aSize)
            break;
        pos += commandSize;
        const uint8_t channel = command[1];
        const uint16_t reliableSeq = detail::Be16(command + 2);
        const bool data = number == ENET_PROTOCOL_COMMAND_SEND_RELIABLE || number == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE ||
                          number == ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED ||
                          number == ENET_PROTOCOL_COMMAND_SEND_FRAGMENT ||
                          number == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT;
        if (!data)
            continue;
        // Data length: the last u16 of the three simple commands, dataLength of fragments
        size_t length = 0;
        switch (number)
        {
        case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
            length = detail::Be16(command + offsetof(ENetProtocolSendReliable, dataLength));
            break;
        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
            length = detail::Be16(command + offsetof(ENetProtocolSendUnreliable, dataLength));
            break;
        case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
            length = detail::Be16(command + offsetof(ENetProtocolSendUnsequenced, dataLength));
            break;
        default:
            length = detail::Be16(command + offsetof(ENetProtocolSendFragment, dataLength));
            break;
        }
        // Here ENet refuses this command and drops the rest of the datagram: stop counting
        if (channel >= peer->channelCount || length > aHost.maximumPacketSize || pos + length > aSize)
            break;
        pos += length;
        const ENetChannel& ch = peer->channels[channel];
        const bool sim = channel < kSim;

        if (number == ENET_PROTOCOL_COMMAND_SEND_RELIABLE)
        {
            const uint16_t incoming = sim ? next[channel] : ch.incomingReliableSequenceNumber;
            if (!detail::InWindow(reliableSeq, incoming) || reliableSeq == incoming)
                continue; // a repeat or outside the window: ENet drops it
            if (sim && reliableSeq == static_cast<uint16_t>(incoming + 1))
            {
                next[channel] = reliableSeq; // the next number: delivered at once
                continue;
            }
            hold(length);
            continue;
        }
        if (number == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE)
        {
            const uint16_t incoming = sim ? next[channel] : ch.incomingReliableSequenceNumber;
            if (!detail::InWindow(reliableSeq, incoming))
                continue;
            if (sim && reliableSeq == incoming)
                continue; // of the current number: delivered at once (or dropped as old)
            hold(length);
            if (sim)
                unreliableEmpty[channel] = false;
            continue;
        }
        if (number == ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
        {
            if (sim && unreliableEmpty[channel])
                continue; // delivered at once into an empty unreliable queue
            hold(length);
            continue;
        }

        // Fragments
        aOut.fragments = true;
        const uint16_t start = detail::Be16(command + offsetof(ENetProtocolSendFragment, startSequenceNumber));
        const uint32_t count = detail::Be32(command + offsetof(ENetProtocolSendFragment, fragmentCount));
        const uint32_t fragment = detail::Be32(command + offsetof(ENetProtocolSendFragment, fragmentNumber));
        const uint32_t total = detail::Be32(command + offsetof(ENetProtocolSendFragment, totalLength));
        const uint32_t offset = detail::Be32(command + offsetof(ENetProtocolSendFragment, fragmentOffset));
        const bool reliable = number == ENET_PROTOCOL_COMMAND_SEND_FRAGMENT;
        // The window as in ENet: a fragment outside it is dropped without an error
        if (!detail::InWindow(reliable ? start : reliableSeq, ch.incomingReliableSequenceNumber))
            continue;
        if (!reliable && reliableSeq == ch.incomingReliableSequenceNumber && start <= ch.incomingUnreliableSequenceNumber)
            continue;
        // A malformed fragment: ENet drops the rest of the datagram
        if (length == 0 || count > ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT || fragment >= count ||
            total > aHost.maximumPacketSize || total < count || offset >= total || length > total - offset)
            break;
        if (reliable && start == ch.incomingReliableSequenceNumber)
            break; // ENet: a reassembly with an already delivered number is an error, the rest of the datagram is dropped
        const int found = reliable ? detail::FindReliableStart(ch, start, total, count)
                                   : detail::FindUnreliableStart(ch, reliableSeq, start, total, count);
        if (found < 0)
            break;
        if (found > 0)
            continue; // continues a started reassembly: its memory is taken already
        bool counted = false;
        for (size_t i = 0; i < startedCount; ++i)
            counted = counted || (started[i].channel == channel && started[i].reliable == reliable &&
                                  started[i].start == start && (reliable || started[i].reliableSeq == reliableSeq));
        if (counted)
            continue;
        if (startedCount < started.size())
            started[startedCount++] = {channel, reliable, reliableSeq, start};
        hold(static_cast<size_t>(total) + detail::FragmentMapBytes(count));
        aOut.biggest = std::max<size_t>(aOut.biggest, total);
        if (!reliable && sim)
            unreliableEmpty[channel] = false;
    }
    return true;
}

/// What waits at the connection now, from its queues, at the same cost Inspect counts
inline size_t HeldCost(const ENetPeer& aPeer)
{
    size_t cost = 0;
    for (size_t c = 0; c < aPeer.channelCount; ++c)
    {
        const ENetChannel& channel = aPeer.channels[c];
        for (const ENetList* list : {&channel.incomingReliableCommands, &channel.incomingUnreliableCommands})
            for (const ENetListNode* node = list->sentinel.next; node != &list->sentinel; node = node->next)
            {
                const auto* incoming = reinterpret_cast<const ENetIncomingCommand*>(node);
                cost += kEntryCost + (incoming->packet ? incoming->packet->dataLength : 0) +
                        (incoming->fragments ? detail::FragmentMapBytes(incoming->fragmentCount) : 0);
            }
    }
    return cost;
}

/// The verdict on a datagram. aHeldNow: what waits at the connection now; aPoolOthers: how much of the pool the other
/// connections use (their waiting data above their shares). A pure function.
inline Verdict Decide(const Budget& aBudget, bool aPreAuth, size_t aHeldNow, const Bring& aBring, size_t aPoolOthers)
{
    if (!aBudget.perPeer || !aBring.peer)
        return Verdict::Pass;
    if (aBudget.maxMessage && aBring.biggest > aBudget.maxMessage)
        return Verdict::TooBig;
    if (aPreAuth && (aBring.fragments || (aBring.held && aHeldNow + aBring.held > aBudget.preAuth)))
        return Verdict::PreAuth;
    if (!aBring.held)
        return Verdict::Pass;
    const size_t after = aHeldNow + aBring.held;
    if (after > aBudget.perPeer)
        return Verdict::PeerFull;
    if (aBudget.pool)
    {
        const size_t aboveBefore = aHeldNow > aBudget.guaranteed ? aHeldNow - aBudget.guaranteed : 0;
        const size_t aboveAfter = after > aBudget.guaranteed ? after - aBudget.guaranteed : 0;
        if (aboveAfter > aboveBefore && aPoolOthers + aboveAfter > aBudget.pool)
            return Verdict::PoolFull;
    }
    return Verdict::Pass;
}

/// Accounting of an ENet host: waiting data per connection (exact, from ENet's queues, at most every kRefreshMs per
/// connection; between recounts an upper bound: what passed since) and the shared pool. Only the thread that services
/// the host.
class Guard
{
public:
    static constexpr uint64_t kRefreshMs = 100;     ///< recount a connection's waiting data from ENet's queues at most this often
    static constexpr uint64_t kPoolRefreshMs = 250; ///< recount the pool over all connections at most this often

    void Configure(const Budget& aBudget)
    {
        m_budget = aBudget;
        m_slots.clear();
        m_poolUsed = 0;
        m_poolExactAtMs = 0;
    }
    const Budget& Limits() const { return m_budget; }

    /// Counts of drops, total and by kind, since the last TakeDropped
    struct Dropped
    {
        uint64_t preAuth = 0;
        uint64_t peerFull = 0;
        uint64_t poolFull = 0;
        uint64_t tooBig = 0;
        uint64_t Total() const { return preAuth + peerFull + poolFull + tooBig; }
    };

    /// Judges the datagram the host holds now (receivedData). aPreAuth tells whether a connection is outside a session.
    /// Pass: hand it to ENet; anything else: drop it (the intercept returns 1).
    template<class PreAuth>
    Verdict Judge(ENetHost& aHost, uint64_t aNowMs, PreAuth&& aPreAuth)
    {
        m_last = Bring{};
        if (!m_budget.perPeer || !g_on.load(std::memory_order_relaxed))
            return Verdict::Pass;
        Bring bring;
        if (!Inspect(aHost, aHost.receivedData, aHost.receivedDataLength, aHost.receivedAddress, bring))
            return Verdict::Pass;
        const bool pre = aPreAuth(bring.peer);
        if (!bring.held && !(pre && bring.fragments))
            return Verdict::Pass; // everything is delivered at once: an honest connection's usual datagram
        Slot& slot = SlotOf(aHost, *bring.peer);
        if (aNowMs - slot.exactAtMs >= kRefreshMs)
            Refresh(*bring.peer, slot, aNowMs);
        Verdict verdict = Decide(m_budget, pre, slot.held, bring, m_poolUsed - Above(slot.held));
        if (verdict == Verdict::PoolFull && aNowMs - m_poolExactAtMs >= kPoolRefreshMs)
        {
            // The pool is counted from above: what has left since the last recount still sits in it. Recount from the queues
            // before refusing
            RefreshAll(aHost, aNowMs);
            verdict = Decide(m_budget, pre, slot.held, bring, m_poolUsed - Above(slot.held));
        }
        if (verdict == Verdict::Pass)
        {
            SetHeld(slot, slot.held + bring.held);
            return verdict;
        }
        m_last = bring;
        m_lastHeld = slot.held;
        switch (verdict)
        {
        case Verdict::PreAuth:
            ++m_dropped.preAuth;
            break;
        case Verdict::PeerFull:
            ++m_dropped.peerFull;
            break;
        case Verdict::TooBig:
            ++m_dropped.tooBig;
            break;
        default:
            ++m_dropped.poolFull;
            break;
        }
        return verdict;
    }

    /// About once a second: the pool from ENet's queues (what has left stops counting)
    void Tick(ENetHost& aHost, uint64_t aNowMs)
    {
        if (m_budget.perPeer && aNowMs - m_poolExactAtMs >= 1000)
            RefreshAll(aHost, aNowMs);
    }

    Dropped TakeDropped()
    {
        const Dropped out = m_dropped;
        m_dropped = {};
        return out;
    }
    /// The last dropped datagram: what it carried and how much its connection held — for the log line
    const Bring& LastDropped() const { return m_last; }
    size_t LastHeld() const { return m_lastHeld; }
    size_t PoolUsed() const { return m_poolUsed; }

private:
    struct Slot
    {
        uint32_t connectId = 0;
        size_t held = 0;
        uint64_t exactAtMs = 0;
    };

    size_t Above(size_t aHeld) const { return aHeld > m_budget.guaranteed ? aHeld - m_budget.guaranteed : 0; }

    void SetHeld(Slot& aSlot, size_t aHeld)
    {
        m_poolUsed -= Above(aSlot.held);
        aSlot.held = aHeld;
        m_poolUsed += Above(aSlot.held);
    }

    Slot& SlotOf(const ENetHost& aHost, const ENetPeer& aPeer)
    {
        const size_t index = static_cast<size_t>(&aPeer - aHost.peers);
        if (m_slots.size() < aHost.peerCount)
            m_slots.resize(aHost.peerCount);
        Slot& slot = m_slots[index];
        if (slot.connectId != aPeer.connectID)
        {
            // A new connection in the ENet slot: the previous one's waiting data left with it
            SetHeld(slot, 0);
            slot.connectId = aPeer.connectID;
            slot.exactAtMs = 0;
        }
        return slot;
    }

    void Refresh(const ENetPeer& aPeer, Slot& aSlot, uint64_t aNowMs)
    {
        SetHeld(aSlot, HeldCost(aPeer));
        aSlot.exactAtMs = aNowMs;
    }

    void RefreshAll(const ENetHost& aHost, uint64_t aNowMs)
    {
        m_poolExactAtMs = aNowMs;
        for (size_t i = 0; i < m_slots.size() && i < aHost.peerCount; ++i)
        {
            Slot& slot = m_slots[i];
            const ENetPeer& peer = aHost.peers[i];
            if (slot.held == 0 && slot.connectId == peer.connectID)
                continue;
            const bool live = peer.state != ENET_PEER_STATE_DISCONNECTED && peer.state != ENET_PEER_STATE_ZOMBIE;
            if (!live || slot.connectId != peer.connectID)
            {
                SetHeld(slot, 0);
                slot.connectId = live ? peer.connectID : 0;
                continue;
            }
            Refresh(peer, slot, aNowMs);
        }
    }

    Budget m_budget;
    std::vector<Slot> m_slots;
    size_t m_poolUsed = 0;
    uint64_t m_poolExactAtMs = 0;
    Dropped m_dropped;
    Bring m_last;
    size_t m_lastHeld = 0;
};
} // namespace nc::enetguard

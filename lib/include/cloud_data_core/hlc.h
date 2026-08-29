#pragma once

#include <cstdint>
#include <string>

namespace cloud_data_core {

/// Hybrid Logical Clock timestamp: a physical-time component (nanoseconds
/// since the Unix epoch) plus a logical counter that breaks ties when
/// multiple events share the same physical reading. Two timestamps are
/// ordered by (physical, logical) lexicographically, and a peer_id is used
/// as the final tiebreaker by callers (see Hlc::happensBefore /
/// LwwRegister in crdt.h) since two distinct peers can otherwise produce an
/// identical (physical, logical) pair.
struct HlcTimestamp {
    uint64_t physical = 0;
    uint32_t logical = 0;

    bool operator==(const HlcTimestamp& other) const {
        return physical == other.physical && logical == other.logical;
    }
    bool operator<(const HlcTimestamp& other) const {
        if (physical != other.physical) return physical < other.physical;
        return logical < other.logical;
    }
    bool operator>(const HlcTimestamp& other) const { return other < *this; }
    bool operator<=(const HlcTimestamp& other) const { return !(other < *this); }
    bool operator>=(const HlcTimestamp& other) const { return !(*this < other); }

    /// Fixed-width, lexicographically-sortable encoding ("<20-digit physical>.<10-digit logical>").
    std::string toString() const;
    static HlcTimestamp fromString(const std::string& s);
};

/// Compares two (HlcTimestamp, peer_id) pairs — the deterministic ordering
/// used to resolve concurrent writes to the same LWW-Register field.
/// Returns true if (a_ts, a_peer) is ordered after (b_ts, b_peer), i.e. `a`
/// should win as the more-recent write.
bool hlcWinsOver(const HlcTimestamp& aTs, const std::string& aPeer,
                  const HlcTimestamp& bTs, const std::string& bPeer);

/// A Hybrid Logical Clock local to one peer. Not thread-safe; callers
/// serialize access (the store's op-append path already holds a lock).
class HybridLogicalClock {
public:
    /// `nowNanos` supplies the physical wall-clock reading in nanoseconds
    /// since the Unix epoch; injectable for tests.
    explicit HybridLogicalClock(uint64_t (*nowNanos)() = nullptr);

    /// Generates a new local timestamp, advancing the clock state.
    HlcTimestamp tick();

    /// Merges in a timestamp observed from a remote peer (e.g. attached to
    /// an incoming op) and advances the local clock so that all subsequent
    /// local ticks happen-after it.
    HlcTimestamp update(const HlcTimestamp& remote);

    HlcTimestamp current() const { return state_; }

private:
    uint64_t physicalNow() const;

    HlcTimestamp state_;
    uint64_t (*nowNanosFn_)();
};

} // namespace cloud_data_core

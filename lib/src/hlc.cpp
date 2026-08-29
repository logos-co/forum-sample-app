#include "cloud_data_core/hlc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace cloud_data_core {

namespace {

uint64_t defaultNowNanos() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

std::string HlcTimestamp::toString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%020llu.%010u",
                  static_cast<unsigned long long>(physical), logical);
    return std::string(buf);
}

HlcTimestamp HlcTimestamp::fromString(const std::string& s) {
    HlcTimestamp ts;
    auto dot = s.find('.');
    if (dot == std::string::npos) return ts;
    ts.physical = std::stoull(s.substr(0, dot));
    ts.logical = static_cast<uint32_t>(std::stoul(s.substr(dot + 1)));
    return ts;
}

bool hlcWinsOver(const HlcTimestamp& aTs, const std::string& aPeer,
                  const HlcTimestamp& bTs, const std::string& bPeer) {
    if (aTs != bTs) return bTs < aTs;
    return bPeer < aPeer;
}

HybridLogicalClock::HybridLogicalClock(uint64_t (*nowNanos)())
    : nowNanosFn_(nowNanos ? nowNanos : defaultNowNanos) {}

uint64_t HybridLogicalClock::physicalNow() const { return nowNanosFn_(); }

HlcTimestamp HybridLogicalClock::tick() {
    const uint64_t phys = physicalNow();
    const uint64_t l = std::max(state_.physical, phys);
    const uint32_t c = (l == state_.physical) ? state_.logical + 1 : 0;
    state_ = HlcTimestamp{l, c};
    return state_;
}

HlcTimestamp HybridLogicalClock::update(const HlcTimestamp& remote) {
    const uint64_t phys = physicalNow();
    const uint64_t l = std::max({state_.physical, remote.physical, phys});
    uint32_t c;
    if (l == state_.physical && l == remote.physical) {
        c = std::max(state_.logical, remote.logical) + 1;
    } else if (l == state_.physical) {
        c = state_.logical + 1;
    } else if (l == remote.physical) {
        c = remote.logical + 1;
    } else {
        c = 0;
    }
    state_ = HlcTimestamp{l, c};
    return state_;
}

} // namespace cloud_data_core

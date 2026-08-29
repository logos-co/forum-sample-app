#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cloud_data_core {

/// Result of Transport::send()/channelSend(): mirrors delivery_module's
/// StdLogosResult, whose `value` on success is a request id string. That id
/// is what a later delivery/failure event reports back, so the engine keys
/// its outbox rows by it (see CloudDataEngine::onOutboxResolved).
struct TransportSendResult {
    bool success = false;
    std::string requestId;
    std::string error;
};

/// Result of Transport::subscribe()/unsubscribe().
struct TransportResult {
    bool success = false;
    std::string error;
};

/// Abstract pub/sub transport CloudDataEngine broadcasts ops and snapshot
/// pointers over, and subscribes to doc/collection topics through. Mirrors
/// delivery_module's own shape (see src/delivery_module_transport.h for the
/// module-side implementation against modules().delivery_module) in plain
/// C++, so a different Logos module embedding this library can supply its
/// own adapter without any change to the engine.
///
/// Implementations are 1:1 forwarders: the *policy* — prefer a reliable
/// channel, fall back to plain send/subscribe once channelCreate() has
/// failed once — lives in the engine (see CloudDataEngine's ensureChannel/
/// publish/joinTopic), because every module embedding this library depends
/// on the same delivery_module and would otherwise reimplement it
/// identically. What stays in the adapter is wire format: how a particular
/// module encodes payloads and envelopes (see fetchHistory below, and
/// BlobStore's equivalent asymmetry).
///
/// Deliberately outbound-only: inbound delivery ("a message arrived") is
/// not modeled as a Transport-owned callback, because delivery_module's
/// messageReceived event is a single node-wide firehose dispatched once per
/// context, not a per-subscribe callback. Instead the embedding module
/// calls CloudDataEngine::handleIncomingMessage() directly from its own
/// messageReceived / channelMessageReceived handlers.
class Transport {
public:
    virtual ~Transport() = default;

    // --- Plain pub/sub: the fallback path -----------------------------------

    virtual TransportSendResult send(const std::string& contentTopic,
                                      const std::vector<uint8_t>& payload) = 0;
    virtual TransportResult subscribe(const std::string& contentTopic) = 0;
    virtual TransportResult unsubscribe(const std::string& contentTopic) = 0;

    // --- Reliable channels: the preferred path ------------------------------
    //
    // The engine passes the content topic itself as `channelId`, so channel
    // ids agree across peers for free and an inbound channel message reports
    // the channelId exactly where a plain message reports the contentTopic.

    /// True if the channel is already open on this node. An unknown channel
    /// id is not an error — it just means "not open here yet", including
    /// after a restart, where persisted channel state survives and
    /// channelCreate() re-opens it rather than starting from scratch.
    virtual bool channelExists(const std::string& channelId) = 0;

    /// Opens (or re-opens) a reliable channel. Returning false is how a
    /// kernel-only node reports that it has no reliable channel manager;
    /// the engine latches that and uses send()/subscribe() from then on.
    virtual bool channelCreate(const std::string& channelId, const std::string& contentTopic,
                                const std::string& peerId) = 0;

    virtual TransportSendResult channelSend(const std::string& channelId,
                                             const std::vector<uint8_t>& payload) = 0;

    // --- History backfill ---------------------------------------------------

    /// Historical op payloads previously published on `contentTopic`,
    /// fetched from `peerAddr`, already decoded out of whatever envelope
    /// and encoding the concrete transport uses — the engine feeds each
    /// returned element straight back through its own op-decode path.
    ///
    /// `requestId` is supplied by the engine (it comes from the local
    /// store's restart-safe peer counter, which the adapter has no access
    /// to). Returns empty on every failure: backfill is best-effort, and
    /// delivery_module's storeQuery in particular is flagged upstream as a
    /// kernel-backed API that may change without a deprecation cycle.
    virtual std::vector<std::vector<uint8_t>> fetchHistory(const std::string& contentTopic,
                                                            const std::string& peerAddr,
                                                            const std::string& requestId,
                                                            int64_t timeoutMs) = 0;
};

} // namespace cloud_data_core

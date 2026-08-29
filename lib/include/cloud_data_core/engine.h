#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cloud_data_core/blob_store.h"
#include "cloud_data_core/crdt.h"
#include "cloud_data_core/hlc.h"
#include "cloud_data_core/store.h"
#include "cloud_data_core/transport.h"

namespace cloud_data_core {

/// Per-embedding-module configuration for CloudDataEngine. Lets a
/// differently-named consumer (e.g. a future forum-storage module) reuse
/// this engine with its own app identity and tuning, rather than the
/// engine hardcoding one module's values.
struct EngineConfig {
    /// LIP-23 content-topic app-name segment (see sync_engine.h). The
    /// original cloud_data_module wrapper supplies "cloud-data" here.
    std::string appName;
    int topicVersion = 1;
    /// Doc-id bucket width for content-topic derivation. 0 means one topic
    /// per collection; see sync_engine::contentTopicForDoc for the trade-off.
    /// Spelled as a literal rather than sync_engine::kDefaultBucketBytes so
    /// this header keeps out of sync_engine.h.
    int bucketBytes = 1;
    /// Number of ops applied to a document (local or remote) before a full
    /// snapshot is pushed to the blob store (see maybeSnapshot / PLAN.md
    /// "Durability / bootstrap bridge").
    int snapshotOpThreshold = 50;
    /// Chunk size passed to BlobStore upload/download calls.
    int64_t storageChunkSize = 65536;
    /// Timeout handed to Transport::fetchHistory() for cold-start backfill.
    /// Bounded deliberately: backfill runs inline in subscribe(), so a peer
    /// that never answers must not hang the caller indefinitely.
    int64_t storeQueryTimeoutMs = 10000;
};

/// Plain-C++ result type structurally identical to the Logos SDK's
/// StdLogosResult, independently defined so this library has no header
/// dependency on the Logos SDK. The module wrapper's public API methods
/// translate this to StdLogosResult with a one-line field copy.
struct EngineResult {
    bool success = false;
    nlohmann::json value;
    std::string error;
};

/// The CRUD/local-first data engine: owns the local CloudDataStore and
/// HybridLogicalClock, and orchestrates sync (via Transport) and
/// durability/bootstrap (via BlobStore) — i.e. everything cloud_data_module
/// does *except* the Logos-framework glue (LogosModuleContext, StdLogosResult,
/// logos_events, node lifecycle), which stays in the module wrapper (see
/// src/cloud_data_module_plugin.h) and talks to this class through plain
/// C++ calls and the DocumentChangedCallback below.
///
/// Thread-safety notes mirror the original plugin: clock_ and the
/// snapshot/session/channel bookkeeping are touched both by this engine's
/// own callers (put/remove, called synchronously) and by
/// handleIncomingMessage/onBlob*()/onOutboxResolved() (invoked from the
/// embedding module's event-delivery thread(s) via the wrapper) — every
/// call site that touches shared state takes the relevant mutex for the
/// whole operation.
class CloudDataEngine {
public:
    CloudDataEngine(std::string dataDir, std::string peerId, EngineConfig config,
                     Transport& transport, BlobStore& blobStore);
    ~CloudDataEngine();

    CloudDataEngine(const CloudDataEngine&) = delete;
    CloudDataEngine& operator=(const CloudDataEngine&) = delete;

    /// Opens the local store under `<dataDir>/cloud_data.sqlite3`. Call once
    /// before any other method. Returns false if the store failed to open.
    bool open();

    EngineResult declareSchema(const std::string& collectionId, const std::string& schemaJson);
    EngineResult put(const std::string& collectionId, const std::string& docId, const std::string& json);
    EngineResult get(const std::string& collectionId, const std::string& docId);
    EngineResult query(const std::string& collectionId, const std::string& filterJson);
    EngineResult remove(const std::string& collectionId, const std::string& docId);
    EngineResult subscribe(const std::string& collectionId, const std::string& docId);

    /// Sets the peer address handed to Transport::fetchHistory() for
    /// cold-start backfill (see subscribe()); an empty string disables
    /// backfill, which is the default. Has to be supplied rather than
    /// discovered — delivery_module exposes no store-peer list.
    EngineResult setStorePeer(const std::string& peerAddr);

    /// Routes an inbound transport message to op-apply or snapshot-pointer
    /// handling by topic shape (sync_engine::isSnapshotPointerTopic). Call
    /// from the embedding module's messageReceived handler, and from its
    /// channelMessageReceived handler with the channel id (which is the
    /// content topic — see ensureChannel).
    void handleIncomingMessage(const std::string& topicOrChannelId, const std::vector<uint8_t>& payload);

    /// Resolves the outbox row for `requestId` once the transport reports
    /// what became of that publish. Call from the embedding module's
    /// message-sent / message-error handlers (on both the channel and the
    /// plain-send path). Until a row is resolved, flushOutbox() keeps
    /// re-issuing it.
    void onOutboxResolved(const std::string& requestId, bool delivered);

    /// Call from the embedding module's storage-adapter event handlers once
    /// they've parsed the underlying module's event payload (session id,
    /// success flag, and — for download progress — the raw chunk bytes,
    /// already decoded from whatever wire encoding the concrete BlobStore
    /// uses).
    void onBlobUploadProgress(bool success, const std::string& sessionId);
    void onBlobDownloadProgress(bool success, const std::string& sessionId, const std::vector<uint8_t>& chunk);
    void onBlobDownloadDone(bool success, const std::string& sessionId);

    using DocumentChangedCallback = std::function<void(const std::string& collectionId,
                                                         const std::string& docId,
                                                         const std::string& json,
                                                         const std::string& origin)>;
    /// Registers the callback invoked whenever a document's materialized
    /// state changes, whether from a local put()/remove() or a merged
    /// remote op (`origin` is "local" or "remote") — the engine-side source
    /// of the module wrapper's documentChanged logos_event.
    void setOnDocumentChanged(DocumentChangedCallback cb);

private:
    std::string dataDir_;
    std::string peerId_;
    EngineConfig config_;
    Transport& transport_;
    BlobStore& blobStore_;

    std::unique_ptr<CloudDataStore> store_;
    bool opened_ = false;

    // clock_ is mutated by put()/remove() (called synchronously by the
    // embedding module's caller) and by handleIncomingOp()/applySnapshotOps()
    // (invoked from the transport/blob-store event-delivery thread(s)).
    // HybridLogicalClock has no internal locking (see hlc.h), so every call
    // site that touches it takes clockMutex_ for the whole store_ call that
    // ticks/updates it.
    HybridLogicalClock clock_;
    std::mutex clockMutex_;

    DocumentChangedCallback onDocumentChanged_;

    std::mutex snapshotOpCountMutex_;
    std::map<std::string, int> snapshotOpCount_; // key: collectionId + "\x1f" + docId

    // Pending blob-store upload/download sessions this engine started,
    // keyed by sessionId, so onBlobUploadProgress/onBlobDownloadDone know
    // which (collectionId, docId) a completion belongs to.
    std::mutex pendingSessionsMutex_;
    std::map<std::string, std::pair<std::string, std::string>> pendingUploads_;
    std::map<std::string, std::pair<std::string, std::string>> pendingDownloads_;
    std::map<std::string, std::vector<uint8_t>> downloadBuffers_; // sessionId -> accumulated raw bytes

    // Reliable-channel bookkeeping. channelId is deliberately the content
    // topic string itself: channel ids then agree across peers for free
    // (they already agree on bucket topics, see sync_engine.h), and an
    // inbound channel message reports the channelId exactly where a plain
    // message reported the contentTopic, so inbound routing stays identical
    // on both paths.
    std::mutex channelsMutex_;
    std::set<std::string> channels_;
    // Latched false the first time channelCreate() fails, after which the
    // engine falls back to plain send()/subscribe(). Not hypothetical: this
    // module tolerates a foreign-owned node, and on a kernel-only node every
    // channel* call fails with "no reliable channel manager".
    bool channelsAvailable_ = true;

    std::mutex storePeerMutex_;
    std::string storePeerAddr_;

    // Creates (or re-opens) the reliable channel for `topic` unless one is
    // already known. Returns false when channels are unavailable, which
    // means the caller should use the send()/subscribe() fallback.
    bool ensureChannel(const std::string& topic);

    // Publishes one already-encoded payload on `topic`, over the topic's
    // channel when available and plain send() otherwise. Returns the request
    // id the transport issued, or "" if the publish call itself failed.
    std::string publish(const std::string& topic, const std::vector<uint8_t>& payload);

    // publish() + outbox bookkeeping. Never fails the caller: a broadcast
    // failure only means the op waits for the next flushOutbox().
    void broadcastOp(const std::string& topic, const std::vector<uint8_t>& payload);

    // Joins `topic` for inbound messages, via a channel when available and
    // subscribe() otherwise.
    bool joinTopic(const std::string& topic);

    // Re-sends every outbox row still pending/failed. Driven opportunistically
    // from put()/remove()/subscribe() rather than by a background thread:
    // this engine has no event loop of its own, and the retry gap does not
    // justify introducing one.
    void flushOutbox();

    // Replays historical ops for (collectionId, docId) from the configured
    // store peer. Best-effort and non-fatal in every failure mode.
    void backfillFromStore(const std::string& collectionId, const std::string& docId,
                            const std::string& topic);

    void handleIncomingOp(const std::vector<uint8_t>& payload);
    void handleSnapshotPointer(const std::vector<uint8_t>& payload);
    void maybeSnapshot(const std::string& collectionId, const std::string& docId);
    void beginSnapshotUpload(const std::string& collectionId, const std::string& docId);
    void applySnapshotOps(const std::string& collectionId, const std::vector<uint8_t>& snapshotBytes);
};

} // namespace cloud_data_core

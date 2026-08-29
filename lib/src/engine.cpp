#include "cloud_data_core/engine.h"

#include "cloud_data_core/sync_engine.h"

using json = nlohmann::json;

namespace cloud_data_core {

CloudDataEngine::CloudDataEngine(std::string dataDir, std::string peerId, EngineConfig config,
                                   Transport& transport, BlobStore& blobStore)
    : dataDir_(std::move(dataDir)),
      peerId_(std::move(peerId)),
      config_(std::move(config)),
      transport_(transport),
      blobStore_(blobStore) {}

CloudDataEngine::~CloudDataEngine() {
    if (store_) store_->close();
}

bool CloudDataEngine::open() {
    if (opened_) return true;
    store_ = std::make_unique<CloudDataStore>(dataDir_);
    if (!store_->open()) return false;
    opened_ = true;
    return true;
}

void CloudDataEngine::setOnDocumentChanged(DocumentChangedCallback cb) {
    onDocumentChanged_ = std::move(cb);
}

// --- Transport plumbing ------------------------------------------------------

bool CloudDataEngine::ensureChannel(const std::string& topic) {
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        if (!channelsAvailable_) return false;
        if (channels_.count(topic) != 0) return true;
    }

    // An unknown channel id is not an error for channelExists(), so "false"
    // just means "not open in this node yet" — including after a restart,
    // where the persisted channel state survives and channelCreate()
    // re-opens it rather than starting from scratch.
    if (!transport_.channelExists(topic)) {
        // channelId == contentTopic: see the channels_ comment in engine.h.
        if (!transport_.channelCreate(topic, topic, peerId_)) {
            std::lock_guard<std::mutex> lock(channelsMutex_);
            channelsAvailable_ = false;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(channelsMutex_);
    channels_.insert(topic);
    return true;
}

std::string CloudDataEngine::publish(const std::string& topic,
                                      const std::vector<uint8_t>& payload) {
    TransportSendResult res = ensureChannel(topic) ? transport_.channelSend(topic, payload)
                                                    : transport_.send(topic, payload);
    if (!res.success) return {};
    return res.requestId;
}

void CloudDataEngine::broadcastOp(const std::string& topic,
                                   const std::vector<uint8_t>& payload) {
    const std::string payloadText(payload.begin(), payload.end());
    std::string requestId = publish(topic, payload);
    if (requestId.empty()) {
        // The publish call itself failed, so there is no request id to key
        // the row by and no event will ever resolve it. Park it under a
        // locally generated id anyway: without a row, a failed broadcast
        // would be lost entirely rather than retried by flushOutbox().
        requestId = "local-" + std::to_string(store_->nextPeerCounter(peerId_));
    }
    store_->enqueueOutbox(requestId, topic, payloadText);
}

bool CloudDataEngine::joinTopic(const std::string& topic) {
    if (ensureChannel(topic)) return true;
    return transport_.subscribe(topic).success;
}

void CloudDataEngine::flushOutbox() {
    if (!store_) return;

    for (const auto& entry : store_->pendingOutbox()) {
        const std::vector<uint8_t> payload(entry.payload.begin(), entry.payload.end());
        const std::string requestId = publish(entry.contentTopic, payload);
        if (requestId.empty() || requestId == entry.requestId) continue;

        // request_id is the primary key, so a re-issued row moves to the new
        // id rather than leaving a duplicate behind under the old one.
        store_->deleteOutbox(entry.requestId);
        store_->enqueueOutbox(requestId, entry.contentTopic, entry.payload);
    }
}

void CloudDataEngine::onOutboxResolved(const std::string& requestId, bool delivered) {
    if (!store_) return;
    store_->markOutboxStatus(requestId, delivered ? "sent" : "failed");
}

EngineResult CloudDataEngine::setStorePeer(const std::string& peerAddr) {
    std::lock_guard<std::mutex> lock(storePeerMutex_);
    storePeerAddr_ = peerAddr;
    return {true, {}, ""};
}

void CloudDataEngine::backfillFromStore(const std::string& collectionId,
                                         const std::string& docId,
                                         const std::string& topic) {
    std::string peerAddr;
    {
        std::lock_guard<std::mutex> lock(storePeerMutex_);
        peerAddr = storePeerAddr_;
    }
    if (peerAddr.empty()) return;

    // Only useful for a cold start: a peer that already has local history
    // will converge through live ops and the snapshot-pointer path.
    if (store_->get(collectionId, docId).has_value()) return;

    const std::string requestId =
        "cloud-data-backfill-" + std::to_string(store_->nextPeerCounter(peerId_));

    // The adapter owns the request/response wire format and returns raw op
    // payloads, or nothing at all on any failure.
    for (const auto& payload :
          transport_.fetchHistory(topic, peerAddr, requestId, config_.storeQueryTimeoutMs)) {
        handleIncomingOp(payload);
    }
}

// --- Public API --------------------------------------------------------------

EngineResult CloudDataEngine::declareSchema(const std::string& collectionId,
                                             const std::string& schemaJson) {
    json obj = json::parse(schemaJson, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) {
        return {false, {}, "declareSchema(): schemaJson must be a flat JSON object"};
    }

    std::vector<std::pair<std::string, CrdtType>> fields;
    for (auto& [field, typeName] : obj.items()) {
        if (!typeName.is_string()) return {false, {}, "declareSchema(): field type must be a string"};
        const std::string t = typeName.get<std::string>();
        CrdtType type;
        if (t == "lww") type = CrdtType::LwwRegister;
        else if (t == "orset") type = CrdtType::OrSet;
        else if (t == "pncounter") type = CrdtType::PnCounter;
        else return {false, {}, "declareSchema(): unknown type '" + t + "'"};
        fields.emplace_back(field, type);
    }

    store_->declareSchema(collectionId, fields);
    return {true, {}, ""};
}

EngineResult CloudDataEngine::put(const std::string& collectionId, const std::string& docId,
                                   const std::string& jsonStr) {
    // Opportunistic retry of anything still undelivered. Cheap in the normal
    // case (idx_outbox_status, usually no rows) and keeps the outbox moving
    // without this engine owning a background thread.
    flushOutbox();

    try {
        json parsed = json::parse(jsonStr, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return {false, {}, "put(): json must be a flat JSON object"};
        }

        std::vector<CrdtOp> ops;
        {
            std::lock_guard<std::mutex> clockLock(clockMutex_);
            ops = store_->put(collectionId, docId, jsonStr, peerId_, clock_);
        }

        auto materialized = store_->get(collectionId, docId);
        if (onDocumentChanged_) {
            onDocumentChanged_(collectionId, docId, materialized.value_or("{}"), "local");
        }

        const std::string topic = sync_engine::contentTopicForDoc(config_.appName, config_.topicVersion,
                                                                    collectionId, docId,
                                                                    config_.bucketBytes);
        for (const auto& op : ops) {
            // A failed broadcast is not fatal to put(): the local write
            // already succeeded, and broadcastOp() parks the op in the outbox
            // either way, so the next flushOutbox() re-issues it.
            broadcastOp(topic, sync_engine::encodeOp(collectionId, op));
        }

        maybeSnapshot(collectionId, docId);
        return {true, {}, ""};
    } catch (const std::exception& e) {
        return {false, {}, std::string("put() failed: ") + e.what()};
    }
}

EngineResult CloudDataEngine::get(const std::string& collectionId, const std::string& docId) {
    auto materialized = store_->get(collectionId, docId);
    if (!materialized) return {false, {}, "document not found"};

    json parsed = json::parse(*materialized, nullptr, false);
    if (parsed.is_discarded()) return {false, {}, "corrupt materialized state"};
    if (parsed.value(CrdtDocument::kDeletedField, false)) {
        return {false, {}, "document was removed"};
    }
    return {true, parsed, ""};
}

EngineResult CloudDataEngine::query(const std::string& collectionId, const std::string& filterJson) {
    try {
        const std::string resultsJson = store_->query(collectionId, filterJson);
        json parsed = json::parse(resultsJson, nullptr, false);
        if (parsed.is_discarded()) return {false, {}, "query(): failed to build result set"};
        return {true, parsed, ""};
    } catch (const std::exception& e) {
        return {false, {}, std::string("query() failed: ") + e.what()};
    }
}

EngineResult CloudDataEngine::remove(const std::string& collectionId, const std::string& docId) {
    // Opportunistic retry of anything still undelivered. Cheap in the normal
    // case (idx_outbox_status, usually no rows) and keeps the outbox moving
    // without this engine owning a background thread.
    flushOutbox();

    try {
        CrdtOp op;
        {
            std::lock_guard<std::mutex> clockLock(clockMutex_);
            op = store_->remove(collectionId, docId, peerId_, clock_);
        }

        auto materialized = store_->get(collectionId, docId);
        if (onDocumentChanged_) {
            onDocumentChanged_(collectionId, docId, materialized.value_or("{}"), "local");
        }

        const std::string topic = sync_engine::contentTopicForDoc(config_.appName, config_.topicVersion,
                                                                    collectionId, docId,
                                                                    config_.bucketBytes);
        broadcastOp(topic, sync_engine::encodeOp(collectionId, op));

        return {true, {}, ""};
    } catch (const std::exception& e) {
        return {false, {}, std::string("remove() failed: ") + e.what()};
    }
}

EngineResult CloudDataEngine::subscribe(const std::string& collectionId, const std::string& docId) {
    const std::string topic = sync_engine::contentTopicForDoc(config_.appName, config_.topicVersion,
                                                                collectionId, docId,
                                                                config_.bucketBytes);
    const std::string ptrTopic = sync_engine::snapshotPointerTopic(config_.appName, config_.topicVersion,
                                                                     collectionId);

    store_->recordSubscription(topic, docId);

    if (!joinTopic(topic)) return {false, {}, "failed to join the document's topic"};
    if (!joinTopic(ptrTopic)) return {false, {}, "failed to join the collection's snapshot-pointer topic"};

    // Cold-start history replay, if a store peer has been configured. Runs
    // after the join so no op published between the two is missed.
    backfillFromStore(collectionId, docId, topic);

    flushOutbox();
    return {true, {}, ""};
}

// --- Inbound sync ------------------------------------------------------------

void CloudDataEngine::handleIncomingMessage(const std::string& topicOrChannelId,
                                             const std::vector<uint8_t>& payload) {
    // Snapshot-pointer messages share the same inbound event as regular ops;
    // route on topic shape (see sync_engine.h).
    if (sync_engine::isSnapshotPointerTopic(topicOrChannelId)) {
        handleSnapshotPointer(payload);
    } else {
        handleIncomingOp(payload);
    }
}

void CloudDataEngine::handleIncomingOp(const std::vector<uint8_t>& payload) {
    std::string collectionId;
    CrdtOp op;
    if (!sync_engine::decodeOp(payload, collectionId, op)) return;

    ApplyResult result;
    {
        std::lock_guard<std::mutex> clockLock(clockMutex_);
        result = store_->applyRemoteOp(collectionId, op, clock_);
    }
    if (!result.changed) return;

    if (onDocumentChanged_) {
        onDocumentChanged_(collectionId, op.docId, result.materializedJson, "remote");
    }
    maybeSnapshot(collectionId, op.docId);
}

void CloudDataEngine::handleSnapshotPointer(const std::vector<uint8_t>& payload) {
    const std::string text(payload.begin(), payload.end());
    json obj = json::parse(text, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) return;
    if (!obj.contains("collectionId") || !obj.contains("docId") || !obj.contains("cid")) return;
    if (!obj["collectionId"].is_string() || !obj["docId"].is_string() || !obj["cid"].is_string()) return;

    const std::string collectionId = obj["collectionId"].get<std::string>();
    const std::string docId = obj["docId"].get<std::string>();
    const std::string cid = obj["cid"].get<std::string>();

    // Skip the fetch if we already have local state for this doc — the
    // pointer is only useful for bootstrapping a peer with no history yet.
    if (store_->get(collectionId, docId).has_value()) return;

    BlobOpResult dl = blobStore_.downloadChunks(cid, false, config_.storageChunkSize);
    if (!dl.success) return;

    std::lock_guard<std::mutex> lock(pendingSessionsMutex_);
    pendingDownloads_[dl.value] = {collectionId, docId};
    downloadBuffers_[dl.value] = {};
}

// --- Durability bridge -------------------------------------------------------

void CloudDataEngine::maybeSnapshot(const std::string& collectionId, const std::string& docId) {
    const std::string key = collectionId + "\x1f" + docId;
    bool due = false;
    {
        std::lock_guard<std::mutex> lock(snapshotOpCountMutex_);
        int& count = snapshotOpCount_[key];
        count++;
        if (count >= config_.snapshotOpThreshold) {
            count = 0;
            due = true;
        }
    }
    if (due) beginSnapshotUpload(collectionId, docId);
}

void CloudDataEngine::beginSnapshotUpload(const std::string& collectionId, const std::string& docId) {
    // The snapshot is the doc's full op history (not just the flattened
    // materialized_json) so a bootstrapping peer replays it through
    // applyRemoteOp() exactly like a normal sync op — preserving CRDT
    // provenance (HLC/peerId/OR-Set tombstones) rather than trusting a
    // flattened view. See PLAN.md "Durability / bootstrap bridge".
    auto ops = store_->opsForDoc(collectionId, docId);
    if (ops.empty()) return;

    json arr = json::array();
    for (const auto& op : ops) {
        auto bytes = sync_engine::encodeOp(collectionId, op);
        arr.push_back(json::parse(std::string(bytes.begin(), bytes.end())));
    }
    const std::string snapshotJson = arr.dump();

    const std::string filename = collectionId + "-" + docId + ".snapshot.json";
    BlobOpResult initRes = blobStore_.uploadInit(filename, config_.storageChunkSize);
    if (!initRes.success) return;
    const std::string sessionId = initRes.value;

    {
        std::lock_guard<std::mutex> lock(pendingSessionsMutex_);
        pendingUploads_[sessionId] = {collectionId, docId};
    }

    // v1 uploads the whole snapshot as a single chunk; a document whose op
    // history grows past what one uploadChunk() call can carry would need
    // real multi-chunk streaming, not implemented yet.
    BlobOpResult chunkRes = blobStore_.uploadChunk(sessionId, snapshotJson);
    if (!chunkRes.success) {
        std::lock_guard<std::mutex> lock(pendingSessionsMutex_);
        pendingUploads_.erase(sessionId);
        return;
    }
    // Completion (and the uploadFinalize() call) happens in onBlobUploadProgress().
}

void CloudDataEngine::onBlobUploadProgress(bool success, const std::string& sessionId) {
    if (sessionId.empty()) return;

    // storage_module v2.1.x reports failures through this callback where
    // earlier versions could stay silent, so a failed upload has to release
    // its pending-session entry rather than leaking one per failure.
    if (!success) {
        std::lock_guard<std::mutex> lock(pendingSessionsMutex_);
        pendingUploads_.erase(sessionId);
        return;
    }

    std::string collectionId, docId;
    {
        std::lock_guard<std::mutex> lock(pendingSessionsMutex_);
        auto it = pendingUploads_.find(sessionId);
        if (it == pendingUploads_.end()) return; // not one of ours
        collectionId = it->second.first;
        docId = it->second.second;
        pendingUploads_.erase(it);
    }

    BlobOpResult finalizeRes = blobStore_.uploadFinalize(sessionId);
    if (!finalizeRes.success) return;

    json ptr;
    ptr["collectionId"] = collectionId;
    ptr["docId"] = docId;
    ptr["cid"] = finalizeRes.value;
    const std::string ptrText = ptr.dump();
    const std::vector<uint8_t> ptrBytes(ptrText.begin(), ptrText.end());

    // Must go out the same way subscribe() joined this topic: a peer that
    // joined the pointer topic as a channel would never see a plain send().
    // Not outbox-tracked — a snapshot pointer is regenerable control traffic,
    // not an op whose loss would cost data.
    const std::string ptrTopic = sync_engine::snapshotPointerTopic(config_.appName, config_.topicVersion,
                                                                     collectionId);
    publish(ptrTopic, ptrBytes);
}

void CloudDataEngine::onBlobDownloadProgress(bool success, const std::string& sessionId,
                                              const std::vector<uint8_t>& chunk) {
    if (!success || sessionId.empty()) return;

    std::lock_guard<std::mutex> lock(pendingSessionsMutex_);
    auto it = downloadBuffers_.find(sessionId);
    if (it == downloadBuffers_.end()) return; // not one of ours (e.g. an app-driven download)
    it->second.insert(it->second.end(), chunk.begin(), chunk.end());
}

void CloudDataEngine::onBlobDownloadDone(bool success, const std::string& sessionId) {
    if (sessionId.empty()) return;

    std::string collectionId;
    std::vector<uint8_t> snapshotBytes;
    {
        std::lock_guard<std::mutex> lock(pendingSessionsMutex_);
        auto pendingIt = pendingDownloads_.find(sessionId);
        auto bufIt = downloadBuffers_.find(sessionId);
        if (pendingIt == pendingDownloads_.end() || bufIt == downloadBuffers_.end()) return;
        collectionId = pendingIt->second.first;
        snapshotBytes = bufIt->second;
        pendingDownloads_.erase(pendingIt);
        downloadBuffers_.erase(bufIt);
    }

    if (!success) return;
    applySnapshotOps(collectionId, snapshotBytes);
}

void CloudDataEngine::applySnapshotOps(const std::string& collectionId,
                                        const std::vector<uint8_t>& snapshotBytes) {
    json arr = json::parse(snapshotBytes.begin(), snapshotBytes.end(), nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return;

    for (const auto& opJson : arr) {
        const std::string dumped = opJson.dump();
        const std::vector<uint8_t> bytes(dumped.begin(), dumped.end());
        std::string decodedCollectionId;
        CrdtOp op;
        if (!sync_engine::decodeOp(bytes, decodedCollectionId, op)) continue;

        ApplyResult result;
        {
            std::lock_guard<std::mutex> clockLock(clockMutex_);
            result = store_->applyRemoteOp(collectionId, op, clock_);
        }
        if (result.changed && onDocumentChanged_) {
            onDocumentChanged_(collectionId, op.docId, result.materializedJson, "remote");
        }
    }
}

} // namespace cloud_data_core

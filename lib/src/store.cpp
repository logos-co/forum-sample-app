#include "cloud_data_core/store.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>
#include <sys/stat.h>

using json = nlohmann::json;

namespace cloud_data_core {

namespace {

constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS oplog (
    op_id TEXT PRIMARY KEY,
    collection TEXT NOT NULL,
    doc_id TEXT NOT NULL,
    field TEXT NOT NULL,
    crdt_type INTEGER NOT NULL,
    hlc_physical INTEGER NOT NULL,
    hlc_logical INTEGER NOT NULL,
    peer_id TEXT NOT NULL,
    value_json TEXT,
    tag_peer_id TEXT,
    tag_counter INTEGER,
    is_remove INTEGER NOT NULL DEFAULT 0,
    delta INTEGER,
    causal_deps TEXT
);
CREATE INDEX IF NOT EXISTS idx_oplog_doc ON oplog(collection, doc_id);

CREATE TABLE IF NOT EXISTS documents (
    collection TEXT NOT NULL,
    doc_id TEXT NOT NULL,
    materialized_json TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 0,
    last_snapshot_cid TEXT,
    PRIMARY KEY (collection, doc_id)
);

CREATE TABLE IF NOT EXISTS outbox (
    request_id TEXT PRIMARY KEY,
    content_topic TEXT NOT NULL,
    payload BLOB NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending'
);

-- pendingOutbox() runs on every put()/remove()/subscribe() to drive retries,
-- and outbox keeps resolved rows, so filtering by status must not degrade
-- into a full scan as the table grows.
CREATE INDEX IF NOT EXISTS idx_outbox_status ON outbox(status);

CREATE TABLE IF NOT EXISTS subscriptions (
    content_topic TEXT NOT NULL,
    doc_id TEXT NOT NULL,
    cursor TEXT,
    PRIMARY KEY (content_topic, doc_id)
);

CREATE TABLE IF NOT EXISTS peer_sequence (
    peer_id TEXT PRIMARY KEY,
    next_counter INTEGER NOT NULL DEFAULT 0
);
)SQL";

std::string makeOpId(const std::string& peerId, uint64_t seq) {
    return peerId + "#" + std::to_string(seq);
}

} // namespace

CloudDataStore::CloudDataStore(std::string dataDir) : dataDir_(std::move(dataDir)) {}

CloudDataStore::~CloudDataStore() { close(); }

std::string CloudDataStore::docKey(const std::string& collectionId, const std::string& docId) {
    return collectionId + "\x1f" + docId;
}

bool CloudDataStore::execSql(const std::string& sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool CloudDataStore::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    mkdir(dataDir_.c_str(), 0755); // best-effort; ignore EEXIST and similar

    const std::string dbPath = dataDir_ + "/cloud_data.sqlite3";
    if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
        return false;
    }
    execSql("PRAGMA journal_mode=WAL;");
    execSql("PRAGMA foreign_keys=ON;");
    return execSql(kSchemaSql);
}

void CloudDataStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void CloudDataStore::declareSchema(const std::string& collectionId,
                                    const std::vector<std::pair<std::string, CrdtType>>& fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    schemas_[collectionId] = fields;
}

CrdtType CloudDataStore::inferFieldType(const std::string& collectionId, const std::string& field,
                                          const std::string& valueJson) const {
    auto schemaIt = schemas_.find(collectionId);
    if (schemaIt != schemas_.end()) {
        for (const auto& [name, type] : schemaIt->second) {
            if (name == field) return type;
        }
    }
    json v = json::parse(valueJson, nullptr, false);
    if (!v.is_discarded() && v.is_array()) return CrdtType::OrSet;
    return CrdtType::LwwRegister;
}

uint64_t CloudDataStore::nextPeerCounter(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return nextPeerCounterLocked(peerId);
}

uint64_t CloudDataStore::nextPeerCounterLocked(const std::string& peerId) {
    // Caller already holds mutex_.
    sqlite3_stmt* stmt = nullptr;
    uint64_t next = 0;

    sqlite3_prepare_v2(db_, "SELECT next_counter FROM peer_sequence WHERE peer_id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        next = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db_,
        "INSERT INTO peer_sequence(peer_id, next_counter) VALUES(?1, ?2) "
        "ON CONFLICT(peer_id) DO UPDATE SET next_counter = ?2;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(next + 1));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return next;
}

void CloudDataStore::persistOp(const CrdtOp& op, const std::string& collectionId) {
    // Caller already holds mutex_.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO oplog(op_id, collection, doc_id, field, crdt_type, "
        "hlc_physical, hlc_logical, peer_id, value_json, tag_peer_id, tag_counter, "
        "is_remove, delta, causal_deps) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, op.opId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, collectionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, op.docId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, op.field.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, static_cast<int>(op.type));
    sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(op.hlc.physical));
    sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(op.hlc.logical));
    sqlite3_bind_text(stmt, 8, op.peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, op.valueJson.c_str(), -1, SQLITE_TRANSIENT);
    if (op.tag.peerId.empty()) {
        sqlite3_bind_null(stmt, 10);
        sqlite3_bind_null(stmt, 11);
    } else {
        sqlite3_bind_text(stmt, 10, op.tag.peerId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 11, static_cast<int64_t>(op.tag.counter));
    }
    sqlite3_bind_int(stmt, 12, op.isRemove ? 1 : 0);
    sqlite3_bind_int64(stmt, 13, op.delta);
    sqlite3_bind_null(stmt, 14); // causal_deps: reserved, unused in v1
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void CloudDataStore::persistMaterialized(const std::string& collectionId, const std::string& docId,
                                           const CrdtDocument& doc) {
    // Caller already holds mutex_.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO documents(collection, doc_id, materialized_json, version) VALUES(?1,?2,?3,1) "
        "ON CONFLICT(collection, doc_id) DO UPDATE SET "
        "materialized_json = ?3, version = version + 1;",
        -1, &stmt, nullptr);
    const std::string materialized = doc.materializeJson();
    sqlite3_bind_text(stmt, 1, collectionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, docId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, materialized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<CrdtOp> CloudDataStore::selectOpsForDoc(const std::string& collectionId, const std::string& docId) {
    // Caller already holds mutex_.
    std::vector<CrdtOp> ops;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT op_id, field, crdt_type, hlc_physical, hlc_logical, peer_id, "
        "value_json, tag_peer_id, tag_counter, is_remove, delta FROM oplog "
        "WHERE collection = ?1 AND doc_id = ?2 ORDER BY hlc_physical, hlc_logical;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, collectionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, docId.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CrdtOp op;
        op.opId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        op.docId = docId;
        op.field = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        op.type = static_cast<CrdtType>(sqlite3_column_int(stmt, 2));
        op.hlc.physical = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        op.hlc.logical = static_cast<uint32_t>(sqlite3_column_int64(stmt, 4));
        op.peerId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const unsigned char* valueJson = sqlite3_column_text(stmt, 6);
        op.valueJson = valueJson ? reinterpret_cast<const char*>(valueJson) : "";
        const unsigned char* tagPeer = sqlite3_column_text(stmt, 7);
        if (tagPeer) {
            op.tag.peerId = reinterpret_cast<const char*>(tagPeer);
            op.tag.counter = static_cast<uint64_t>(sqlite3_column_int64(stmt, 8));
        }
        op.isRemove = sqlite3_column_int(stmt, 9) != 0;
        op.delta = sqlite3_column_int64(stmt, 10);

        ops.push_back(std::move(op));
    }
    sqlite3_finalize(stmt);
    return ops;
}

void CloudDataStore::loadDocFromOplog(const std::string& collectionId, const std::string& docId, CrdtDocument& doc) {
    // Caller already holds mutex_. Replays the authoritative oplog for
    // (collectionId, docId) in HLC order to rebuild in-memory state — used
    // the first time a doc is touched in this process lifetime, since
    // `docs_` (the CrdtDocument cache) starts empty on each open().
    for (const auto& op : selectOpsForDoc(collectionId, docId)) {
        doc.applyOp(op);
    }
}

std::vector<CrdtOp> CloudDataStore::opsForDoc(const std::string& collectionId, const std::string& docId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return selectOpsForDoc(collectionId, docId);
}

CrdtDocument& CloudDataStore::getOrLoadDoc(const std::string& collectionId, const std::string& docId) {
    // Caller already holds mutex_.
    const std::string key = docKey(collectionId, docId);
    auto it = docs_.find(key);
    if (it != docs_.end()) return it->second;

    auto [inserted, _] = docs_.emplace(key, CrdtDocument{});
    loadDocFromOplog(collectionId, docId, inserted->second);
    return inserted->second;
}

std::vector<CrdtOp> CloudDataStore::put(const std::string& collectionId, const std::string& docId,
                                          const std::string& jsonStr, const std::string& peerId,
                                          HybridLogicalClock& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CrdtOp> ops;

    json obj = json::parse(jsonStr, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) return ops;

    CrdtDocument& doc = getOrLoadDoc(collectionId, docId);

    for (auto& [field, value] : obj.items()) {
        if (value.is_object() && value.size() == 1 && value.contains("$counterDelta")) {
            CrdtOp op;
            op.opId = makeOpId(peerId, nextPeerCounterLocked(peerId));
            op.docId = docId;
            op.field = field;
            op.type = CrdtType::PnCounter;
            op.hlc = clock.tick();
            op.peerId = peerId;
            op.delta = value["$counterDelta"].get<int64_t>();
            ops.push_back(op);
            continue;
        }

        const std::string valueJson = value.dump();
        const CrdtType type = inferFieldType(collectionId, field, valueJson);

        if (type == CrdtType::OrSet) {
            if (!value.is_array()) continue; // malformed input for a declared OR-Set field; skip
            for (const auto& tag : doc.currentOrSetTags(field)) {
                CrdtOp removeOp;
                removeOp.opId = makeOpId(peerId, nextPeerCounterLocked(peerId));
                removeOp.docId = docId;
                removeOp.field = field;
                removeOp.type = CrdtType::OrSet;
                removeOp.isRemove = true;
                removeOp.tag = tag;
                removeOp.hlc = clock.tick();
                removeOp.peerId = peerId;
                ops.push_back(removeOp);
            }
            for (const auto& element : value) {
                CrdtOp addOp;
                const uint64_t seq = nextPeerCounterLocked(peerId);
                addOp.opId = makeOpId(peerId, seq);
                addOp.docId = docId;
                addOp.field = field;
                addOp.type = CrdtType::OrSet;
                addOp.isRemove = false;
                addOp.tag = OrSetTag{peerId, seq};
                addOp.valueJson = element.dump();
                addOp.hlc = clock.tick();
                addOp.peerId = peerId;
                ops.push_back(addOp);
            }
        } else {
            CrdtOp op;
            op.opId = makeOpId(peerId, nextPeerCounterLocked(peerId));
            op.docId = docId;
            op.field = field;
            op.type = type;
            op.valueJson = valueJson;
            op.hlc = clock.tick();
            op.peerId = peerId;
            ops.push_back(op);
        }
    }

    for (const auto& op : ops) {
        doc.applyOp(op);
        persistOp(op, collectionId);
    }
    persistMaterialized(collectionId, docId, doc);

    return ops;
}

CrdtOp CloudDataStore::remove(const std::string& collectionId, const std::string& docId,
                                const std::string& peerId, HybridLogicalClock& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    CrdtDocument& doc = getOrLoadDoc(collectionId, docId);

    CrdtOp op;
    op.opId = makeOpId(peerId, nextPeerCounterLocked(peerId));
    op.docId = docId;
    op.field = CrdtDocument::kDeletedField;
    op.type = CrdtType::LwwRegister;
    op.valueJson = "true";
    op.hlc = clock.tick();
    op.peerId = peerId;

    doc.applyOp(op);
    persistOp(op, collectionId);
    persistMaterialized(collectionId, docId, doc);

    return op;
}

ApplyResult CloudDataStore::applyRemoteOp(const std::string& collectionId, const CrdtOp& op, HybridLogicalClock& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    ApplyResult result;

    CrdtDocument& doc = getOrLoadDoc(collectionId, op.docId);
    const bool wasApplied = doc.hasAppliedOp(op.opId);
    if (wasApplied) {
        result.changed = false;
        result.materializedJson = doc.materializeJson();
        result.nowDeleted = doc.isDeleted();
        return result;
    }

    clock.update(op.hlc);
    doc.applyOp(op);
    persistOp(op, collectionId);
    persistMaterialized(collectionId, op.docId, doc);

    result.changed = true;
    result.materializedJson = doc.materializeJson();
    result.nowDeleted = doc.isDeleted();
    return result;
}

std::optional<std::string> CloudDataStore::get(const std::string& collectionId, const std::string& docId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = docKey(collectionId, docId);
    auto it = docs_.find(key);
    if (it != docs_.end()) return it->second.materializeJson();

    // Not cached in memory yet: check the persisted projection first so a
    // cold get() doesn't pay the cost of replaying the whole oplog when the
    // row is already materialized.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT materialized_json FROM documents WHERE collection = ?1 AND doc_id = ?2;",
                        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, collectionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, docId.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> materialized;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        materialized = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    if (!materialized) return std::nullopt;

    // Warm the in-memory cache from the oplog so subsequent applyRemoteOp /
    // put calls have full CRDT state, not just the JSON projection.
    getOrLoadDoc(collectionId, docId);
    return materialized;
}

std::string CloudDataStore::query(const std::string& collectionId, const std::string& filterJson) {
    std::lock_guard<std::mutex> lock(mutex_);

    json filter = json::parse(filterJson, nullptr, false);
    if (filter.is_discarded() || !filter.is_object()) filter = json::object();
    const bool includeDeleted = filter.value("includeDeleted", false);
    filter.erase("includeDeleted");

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT doc_id, materialized_json FROM documents WHERE collection = ?1;",
                        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, collectionId.c_str(), -1, SQLITE_TRANSIENT);

    json results = json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string docId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string materializedStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        json materialized = json::parse(materializedStr, nullptr, false);
        if (materialized.is_discarded()) continue;

        if (!includeDeleted && materialized.value(CrdtDocument::kDeletedField, false)) continue;

        bool matches = true;
        for (auto& [field, expected] : filter.items()) {
            if (!materialized.contains(field) || materialized[field] != expected) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;

        json row = materialized;
        row["docId"] = docId;
        results.push_back(row);
    }
    sqlite3_finalize(stmt);
    return results.dump();
}

void CloudDataStore::enqueueOutbox(const std::string& requestId, const std::string& contentTopic,
                                     const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO outbox(request_id, content_topic, payload, status) VALUES(?1,?2,?3,'pending');",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, contentTopic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void CloudDataStore::markOutboxStatus(const std::string& requestId, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "UPDATE outbox SET status = ?2 WHERE request_id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void CloudDataStore::deleteOutbox(const std::string& requestId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM outbox WHERE request_id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<OutboxEntry> CloudDataStore::pendingOutbox() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OutboxEntry> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT request_id, content_topic, payload, status FROM outbox WHERE status IN ('pending','failed');",
                        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OutboxEntry entry;
        entry.requestId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        entry.contentTopic = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const void* blob = sqlite3_column_blob(stmt, 2);
        const int blobLen = sqlite3_column_bytes(stmt, 2);
        entry.payload.assign(reinterpret_cast<const char*>(blob), static_cast<size_t>(blobLen));
        entry.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        out.push_back(std::move(entry));
    }
    sqlite3_finalize(stmt);
    return out;
}

void CloudDataStore::recordSubscription(const std::string& contentTopic, const std::string& docId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO subscriptions(content_topic, doc_id) VALUES(?1,?2);",
                        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, contentTopic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, docId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<std::string> CloudDataStore::docsForTopic(const std::string& contentTopic) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT doc_id FROM subscriptions WHERE content_topic = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, contentTopic.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return out;
}

} // namespace cloud_data_core

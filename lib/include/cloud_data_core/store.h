#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cloud_data_core/crdt.h"
#include "cloud_data_core/hlc.h"

struct sqlite3;
struct sqlite3_stmt;

namespace cloud_data_core {

/// One row parked in `outbox` — a broadcast CloudDataEngine still needs to
/// (re)issue or is waiting to hear the outcome of. `status` moves to "sent"
/// or "failed" when delivery_module reports the outcome via
/// channelMessageSent / channelMessageError.
struct OutboxEntry {
    std::string requestId;
    std::string contentTopic;
    std::string payload; // raw bytes, binary-safe
    std::string status;  // "pending" | "sent" | "failed"
};

/// Result of applying an op to local state.
struct ApplyResult {
    bool changed = false;     // false if the op was a duplicate (already applied)
    bool nowDeleted = false;  // materialized $deleted flag after applying
    std::string materializedJson;
};

/// Embedded SQLite-backed local store: the authoritative `oplog`, the
/// queryable `documents.materialized_json` projection, and the
/// `outbox`/`subscriptions`/`peer_sequence` bookkeeping tables described in
/// PLAN.md "Local store".
///
/// `peer_sequence` is one addition beyond the plan's four tables: it
/// persists, per local peer_id, the next value of the monotonic counter
/// used both as an OR-Set tag's `counter` and as the uniqueness suffix of
/// every generated op's `opId` — without it, an in-memory counter would
/// collide with tags from a previous process lifetime after a restart.
///
/// Not safe for concurrent use from multiple threads without external
/// synchronization beyond what's documented per-method; `mutex_` only
/// protects the in-memory materialized-document cache and SQLite handle
/// from concurrent access by this process.
class CloudDataStore {
public:
    explicit CloudDataStore(std::string dataDir);
    ~CloudDataStore();

    CloudDataStore(const CloudDataStore&) = delete;
    CloudDataStore& operator=(const CloudDataStore&) = delete;

    /// Opens (creating if needed) `<dataDir>/cloud_data.sqlite3` and applies
    /// the schema. Safe to call once per instance.
    bool open();
    void close();

    /// Declares the CRDT type for each named field of `collectionId`.
    /// Fields not declared before first use are inferred at put() time (see
    /// put()'s doc comment) — explicit declaration lets a caller pin a
    /// numeric field as PnCounter, since inference alone cannot distinguish
    /// "a number" from "a counter".
    void declareSchema(const std::string& collectionId,
                        const std::vector<std::pair<std::string, CrdtType>>& fields);

    /// Builds and applies the op(s) for a local write of `json` (a flat
    /// JSON object) to (collectionId, docId), using `clock` to stamp each
    /// op and `peerId` as this local peer's identity.
    ///
    /// Per-field CRDT type is: whatever declareSchema() set for that field,
    /// else inferred from the JSON value's shape:
    ///   - JSON array           -> OR-Set. The whole array is replaced:
    ///     every element currently present is tombstoned and every element
    ///     of the new array is re-added with a fresh tag. This gives
    ///     put()-based whole-value-replace semantics, not incremental
    ///     add/remove (the v1 public API has no addToSet/removeFromSet).
    ///   - {"$counterDelta": N} -> PN-Counter delta (increments by N,
    ///     N may be negative). There is no public increment() method in v1,
    ///     so this JSON shape is the only way to drive a PN-Counter field.
    ///   - anything else (string/number/bool/null) -> LWW-Register.
    ///
    /// Returns the ops that were generated, for the caller
    /// (CloudDataEngine) to broadcast over its Transport.
    std::vector<CrdtOp> put(const std::string& collectionId, const std::string& docId,
                             const std::string& json, const std::string& peerId,
                             HybridLogicalClock& clock);

    /// Builds and applies the reserved tombstone op (see CrdtDocument's
    /// `$deleted` field). Returns the tombstone op for broadcasting.
    CrdtOp remove(const std::string& collectionId, const std::string& docId,
                  const std::string& peerId, HybridLogicalClock& clock);

    /// Applies a remote op (idempotent by op.opId). `clock` is updated so
    /// local ticks happen-after any remote HLC observed.
    ApplyResult applyRemoteOp(const std::string& collectionId, const CrdtOp& op, HybridLogicalClock& clock);

    /// Returns the materialized JSON for (collectionId, docId), or
    /// std::nullopt if never written or fully unknown locally. A tombstoned
    /// (removed) document still returns its JSON with `"$deleted": true` —
    /// callers (CloudDataEngine::get()) decide whether that counts as "found".
    std::optional<std::string> get(const std::string& collectionId, const std::string& docId);

    /// Runs `filterJson` as a simple field-equality filter (`{"field":
    /// value, ...}`, all clauses AND-ed) against the materialized_json
    /// projection for `collectionId` and returns a JSON array of matches.
    /// Deleted documents are excluded unless filterJson has
    /// `{"includeDeleted": true}`.
    std::string query(const std::string& collectionId, const std::string& filterJson);

    /// Allocates the next per-peer monotonic counter, persisting it so it
    /// survives a restart. Used for OR-Set tags and op ids. Safe to call
    /// directly (acquires mutex_ itself) — see nextPeerCounterLocked for
    /// the internal variant put()/remove() use while already holding it.
    uint64_t nextPeerCounter(const std::string& peerId);

    /// Returns every oplog row for (collectionId, docId) in HLC order — the
    /// full, replayable CRDT state for the document. Used by the durability
    /// bridge to build a storage_module snapshot (see PLAN.md "Durability /
    /// bootstrap bridge"): a downloading peer replays these through
    /// applyRemoteOp() to reconstruct the document exactly, tombstones
    /// included, rather than trusting a flattened JSON view.
    std::vector<CrdtOp> opsForDoc(const std::string& collectionId, const std::string& docId);

    // --- Outbox (broadcast tracking) ---------------------------------------
    void enqueueOutbox(const std::string& requestId, const std::string& contentTopic, const std::string& payload);
    void markOutboxStatus(const std::string& requestId, const std::string& status);
    /// Drops a row outright. Used when a retry re-issues a row under a new
    /// request id, since request_id is the primary key.
    void deleteOutbox(const std::string& requestId);
    /// Every row still awaiting delivery: both "pending" (issued, no outcome
    /// reported yet) and "failed" (delivery_module reported an error). Both
    /// are re-issued by CloudDataEngine's opportunistic outbox flush.
    std::vector<OutboxEntry> pendingOutbox();

    // --- Subscriptions bookkeeping ------------------------------------------
    void recordSubscription(const std::string& contentTopic, const std::string& docId);
    std::vector<std::string> docsForTopic(const std::string& contentTopic);

private:
    std::string dataDir_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;

    std::map<std::string, std::vector<std::pair<std::string, CrdtType>>> schemas_;
    std::map<std::string, CrdtDocument> docs_; // key: collectionId + "\x1f" + docId

    static std::string docKey(const std::string& collectionId, const std::string& docId);

    bool execSql(const std::string& sql);
    // Caller must already hold mutex_ (used by put()/remove(), which do;
    // the public nextPeerCounter() acquires the lock then delegates here).
    uint64_t nextPeerCounterLocked(const std::string& peerId);
    std::vector<CrdtOp> selectOpsForDoc(const std::string& collectionId, const std::string& docId);
    void loadDocFromOplog(const std::string& collectionId, const std::string& docId, CrdtDocument& doc);
    CrdtDocument& getOrLoadDoc(const std::string& collectionId, const std::string& docId);
    CrdtType inferFieldType(const std::string& collectionId, const std::string& field, const std::string& valueJson) const;

    void persistOp(const CrdtOp& op, const std::string& collectionId);
    void persistMaterialized(const std::string& collectionId, const std::string& docId, const CrdtDocument& doc);
};

} // namespace cloud_data_core

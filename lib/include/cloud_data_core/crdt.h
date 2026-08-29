#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cloud_data_core/hlc.h"

namespace cloud_data_core {

/// The three hand-rolled CRDT types a document field can be declared as
/// (see PLAN.md "CRDT decision: Option 3"). Nested documents are modeled as
/// references (separate doc_ids), not nested CRDT structures, so this set
/// only needs to cover scalar/collection/counter fields.
enum class CrdtType {
    LwwRegister,
    OrSet,
    PnCounter,
};

/// Uniquely identifies one OR-Set add so a later remove can reference
/// exactly the add(s) it observed, per the OR-Set (Observed-Remove Set)
/// algorithm: an element is present if it has an add tag with no matching
/// remove tag.
struct OrSetTag {
    std::string peerId;
    uint64_t counter = 0;

    bool operator<(const OrSetTag& other) const {
        if (peerId != other.peerId) return peerId < other.peerId;
        return counter < other.counter;
    }
    bool operator==(const OrSetTag& other) const {
        return peerId == other.peerId && counter == other.counter;
    }

    std::string toString() const;
    static OrSetTag fromString(const std::string& s);
};

/// One entry in the oplog: a single CRDT operation against one field of one
/// document. `opId` is the idempotency key (see CrdtDocument::applyOp) —
/// safe to apply the same op more than once or out of order.
struct CrdtOp {
    std::string opId;
    std::string docId;
    std::string field;
    CrdtType type = CrdtType::LwwRegister;
    HlcTimestamp hlc;
    std::string peerId;

    /// LWW-Register: the new value, JSON-encoded. OR-Set add: the element
    /// value, JSON-encoded (ignored on remove).
    std::string valueJson;

    /// OR-Set only: the tag being added (isRemove == false) or the tag(s)
    /// being removed (isRemove == true, one op per observed tag).
    OrSetTag tag;
    bool isRemove = false;

    /// PN-Counter only: signed delta (positive = increment, negative =
    /// decrement) contributed by `peerId`.
    int64_t delta = 0;
};

/// Merge state for a single LWW-Register field: the value from the write
/// with the highest (hlc, peerId) tiebreak wins deterministically on every
/// peer regardless of arrival order (see hlcWinsOver in hlc.h).
class LwwRegister {
public:
    void apply(const std::string& valueJson, const HlcTimestamp& hlc, const std::string& peerId);
    bool hasValue() const { return hasValue_; }
    const std::string& valueJson() const { return valueJson_; }

private:
    bool hasValue_ = false;
    std::string valueJson_;
    HlcTimestamp hlc_;
    std::string peerId_;
};

/// Merge state for a single OR-Set field. `values()` returns the
/// JSON-encoded elements currently present (added but not yet removed by
/// every tag observed for them).
class OrSet {
public:
    void applyAdd(const OrSetTag& tag, const std::string& valueJson);
    void applyRemove(const OrSetTag& tag);
    std::vector<std::string> values() const;

    /// Tags currently present (added, not yet removed) — used by callers
    /// that need to tombstone every current element (e.g. a put()-based
    /// whole-array replace; see CloudDataStore::put).
    std::vector<OrSetTag> currentTags() const;

    /// Tags whose add has been fully removed — retained so the tombstone
    /// isn't resurrected by a late-arriving duplicate add, until garbage
    /// collection (see PLAN.md "Known cost carried by this choice").
    const std::set<OrSetTag>& tombstones() const { return removes_; }

private:
    std::map<OrSetTag, std::string> adds_;
    std::set<OrSetTag> removes_;
};

/// Merge state for a single PN-Counter field: per-peer increment and
/// decrement totals, summed for the current value. Commutative and
/// idempotent-safe only insofar as each op is applied at most once — hence
/// the opId dedup in CrdtDocument, since re-applying a delta would
/// double-count it (unlike LWW-Register/OR-Set, which are naturally
/// idempotent).
class PnCounter {
public:
    void applyDelta(const std::string& peerId, int64_t delta);
    int64_t value() const;

private:
    std::map<std::string, int64_t> perPeerIncrements_;
    std::map<std::string, int64_t> perPeerDecrements_;
};

/// A document is a flat map of independently-typed CRDT fields plus a
/// reserved `$deleted` LWW-Register<bool> used as the tombstone for
/// remove() (see PLAN.md "Public API (v1)": remove() is a tombstone, not a
/// hard delete).
class CrdtDocument {
public:
    /// Declare (or redeclare, idempotently) the CRDT type for a field.
    /// Must be called before the first op touching that field is applied.
    void declareField(const std::string& field, CrdtType type);

    /// Applies `op` if its opId has not been seen before; no-op otherwise.
    /// Safe to call with ops arriving out of order or more than once. The
    /// reserved `$deleted` tombstone (see kDeletedField) is just an
    /// ordinary LWW-Register field applied through this same path.
    void applyOp(const CrdtOp& op);

    /// Current tags for an OR-Set field (empty if the field doesn't exist
    /// or isn't declared OrSet). See OrSet::currentTags.
    std::vector<OrSetTag> currentOrSetTags(const std::string& field) const;

    bool isDeleted() const;

    /// Renders the current merged state as a JSON object string: one key
    /// per declared field (LWW → scalar, OR-Set → array, PN-Counter →
    /// number), plus `"$deleted": bool`.
    std::string materializeJson() const;

    bool hasAppliedOp(const std::string& opId) const;

    static constexpr const char* kDeletedField = "$deleted";

private:
    std::map<std::string, CrdtType> schema_;
    std::map<std::string, LwwRegister> lww_;
    std::map<std::string, OrSet> orSets_;
    std::map<std::string, PnCounter> pnCounters_;
    std::set<std::string> appliedOpIds_;
};

} // namespace cloud_data_core

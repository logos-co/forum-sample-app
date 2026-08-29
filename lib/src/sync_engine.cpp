#include "cloud_data_core/sync_engine.h"

#include <array>
#include <cstdio>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

/// FNV-1a 64-bit — deterministic across platforms/compilers/std-lib
/// versions, unlike std::hash. See sync_engine.h for why that matters here.
uint64_t fnv1a64(const std::string& s) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string hexBucket(const std::string& docId, int bucketBytes) {
    // No bucketing at all: one topic per collection (see sync_engine.h). Has
    // to be handled before the shift below — at bucketBytes == 0 that shift
    // is 64, and shifting a uint64_t by its own width is undefined.
    if (bucketBytes <= 0) return "";

    uint64_t hash = fnv1a64(docId);
    // Take the top `bucketBytes` bytes of the 64-bit hash.
    const int shift = 64 - bucketBytes * 8;
    uint64_t bucket = (bucketBytes >= 8) ? hash : (hash >> shift);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%0*llx", bucketBytes * 2,
                  static_cast<unsigned long long>(bucket));
    return std::string(buf);
}

} // namespace

namespace cloud_data_core::sync_engine {

std::string contentTopicForDoc(const std::string& appName, int version,
                                const std::string& collectionId, const std::string& docId,
                                int bucketBytes) {
    const std::string bucket = hexBucket(docId, bucketBytes);
    // Drop the separator along with the bucket, so an unbucketed collection
    // reads `/app/1/notes/proto` rather than a trailing-dash `/app/1/notes-/proto`.
    return "/" + appName + "/" + std::to_string(version) + "/" + collectionId +
           (bucket.empty() ? "" : "-" + bucket) + "/proto";
}

std::string snapshotPointerTopic(const std::string& appName, int version, const std::string& collectionId) {
    return "/" + appName + "/" + std::to_string(version) + "/" + collectionId + "/snapshot-ptr/proto";
}

bool isSnapshotPointerTopic(const std::string& contentTopic) {
    return contentTopic.find("/snapshot-ptr/proto") != std::string::npos;
}

std::vector<uint8_t> encodeOp(const std::string& collectionId, const CrdtOp& op) {
    json obj;
    obj["opId"] = op.opId;
    obj["collectionId"] = collectionId;
    obj["docId"] = op.docId;
    obj["field"] = op.field;
    obj["type"] = static_cast<int>(op.type);
    obj["hlcPhysical"] = op.hlc.physical;
    obj["hlcLogical"] = op.hlc.logical;
    obj["peerId"] = op.peerId;
    obj["isRemove"] = op.isRemove;
    obj["delta"] = op.delta;

    if (!op.valueJson.empty()) {
        json parsedValue = json::parse(op.valueJson, nullptr, false);
        obj["value"] = parsedValue.is_discarded() ? json(nullptr) : parsedValue;
    } else {
        obj["value"] = nullptr;
    }

    if (!op.tag.peerId.empty()) {
        obj["tagPeerId"] = op.tag.peerId;
        obj["tagCounter"] = op.tag.counter;
    }

    const std::string dumped = obj.dump();
    return std::vector<uint8_t>(dumped.begin(), dumped.end());
}

bool decodeOp(const std::vector<uint8_t>& bytes, std::string& collectionId, CrdtOp& op) {
    const std::string text(bytes.begin(), bytes.end());
    json obj = json::parse(text, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) return false;

    if (!obj.contains("opId") || !obj["opId"].is_string()) return false;
    if (!obj.contains("collectionId") || !obj["collectionId"].is_string()) return false;
    if (!obj.contains("docId") || !obj["docId"].is_string()) return false;
    if (!obj.contains("field") || !obj["field"].is_string()) return false;
    if (!obj.contains("type") || !obj["type"].is_number_integer()) return false;
    if (!obj.contains("hlcPhysical") || !obj["hlcPhysical"].is_number_unsigned()) return false;
    if (!obj.contains("hlcLogical") || !obj["hlcLogical"].is_number_unsigned()) return false;
    if (!obj.contains("peerId") || !obj["peerId"].is_string()) return false;

    const int typeValue = obj["type"].get<int>();
    if (typeValue < static_cast<int>(CrdtType::LwwRegister) || typeValue > static_cast<int>(CrdtType::PnCounter)) {
        return false;
    }

    collectionId = obj["collectionId"].get<std::string>();
    op.opId = obj["opId"].get<std::string>();
    op.docId = obj["docId"].get<std::string>();
    op.field = obj["field"].get<std::string>();
    op.type = static_cast<CrdtType>(typeValue);
    op.hlc.physical = obj["hlcPhysical"].get<uint64_t>();
    op.hlc.logical = obj["hlcLogical"].get<uint32_t>();
    op.peerId = obj["peerId"].get<std::string>();
    op.isRemove = obj.value("isRemove", false);
    op.delta = obj.value("delta", static_cast<int64_t>(0));

    if (obj.contains("value") && !obj["value"].is_null()) {
        op.valueJson = obj["value"].dump();
    } else {
        op.valueJson.clear();
    }

    if (obj.contains("tagPeerId") && obj["tagPeerId"].is_string()) {
        op.tag.peerId = obj["tagPeerId"].get<std::string>();
        op.tag.counter = obj.value("tagCounter", static_cast<uint64_t>(0));
    } else {
        op.tag = OrSetTag{};
    }

    return true;
}

} // namespace cloud_data_core::sync_engine

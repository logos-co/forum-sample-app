#pragma once

#include <cstdint>
#include <string>

namespace cloud_data_core {

/// Result of a BlobStore session call: `value` carries a session id (for
/// uploadInit/downloadChunks) or a CID (for uploadFinalize), mirroring
/// storage_module's StdLogosResult-returning session API.
struct BlobOpResult {
    bool success = false;
    std::string value;
    std::string error;
};

/// Abstract content-addressed blob store used for the durability/bootstrap
/// bridge (see PLAN.md "Durability / bootstrap bridge"). Mirrors
/// storage_module's manual chunked upload/download session API
/// (uploadInit/uploadChunk/uploadFinalize, downloadChunks) in plain C++ —
/// see src/storage_module_blob_store.h for the module-side implementation
/// against modules().storage_module.
///
/// Every method dispatches a request and returns immediately; real
/// completion/progress arrives later via the three onBlob*() calls the
/// embedding module makes directly on CloudDataEngine (the same
/// outbound-only asymmetry as Transport — BlobStore itself carries no
/// callback-registration surface, since storage_module's progress events
/// are node-wide, not per-call).
class BlobStore {
public:
    virtual ~BlobStore() = default;

    virtual BlobOpResult uploadInit(const std::string& filename, int64_t chunkSize) = 0;
    virtual BlobOpResult uploadChunk(const std::string& sessionId, const std::string& chunk) = 0;
    virtual BlobOpResult uploadFinalize(const std::string& sessionId) = 0;
    virtual BlobOpResult downloadChunks(const std::string& cid, bool local, int64_t chunkSize) = 0;
};

} // namespace cloud_data_core

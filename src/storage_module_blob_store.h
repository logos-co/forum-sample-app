#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cloud_data_core/blob_store.h"

// See delivery_module_transport.h for why this is forward-declared.
struct LogosModules;

/// Implements cloud_data_core::BlobStore by forwarding to
/// modules().storage_module.{uploadInit,uploadChunk,uploadFinalize,
/// downloadChunks}.
///
/// A Qt-typed port of cloud-data-module's src/storage_module_blob_store.*,
/// for the same reason DeliveryModuleTransport is one: that module is
/// `type: core` and std-typed, this app is `type: ui_qml` and Qt-typed.
///
/// Also owns every piece of storage_module wire-format knowledge for the
/// three async progress/completion events — JSON envelope parsing *and*
/// base64 decoding — in one place, via the static parsers below, so
/// cloud_data_core::BlobStore's abstraction stays "raw bytes in, raw bytes
/// out" rather than leaking storage_module's base64-over-JSON encoding.
class StorageModuleBlobStore : public cloud_data_core::BlobStore {
public:
    struct UploadProgressEvent {
        bool success = false;
        std::string sessionId;
    };
    struct DownloadProgressEvent {
        bool success = false;
        std::string sessionId;
        std::vector<uint8_t> chunk;
    };
    struct DownloadDoneEvent {
        bool success = false;
        std::string sessionId;
    };

    explicit StorageModuleBlobStore(LogosModules& modules);

    cloud_data_core::BlobOpResult uploadInit(const std::string& filename, int64_t chunkSize) override;
    cloud_data_core::BlobOpResult uploadChunk(const std::string& sessionId, const std::string& chunk) override;
    cloud_data_core::BlobOpResult uploadFinalize(const std::string& sessionId) override;
    cloud_data_core::BlobOpResult downloadChunks(const std::string& cid, bool local, int64_t chunkSize) override;

    /// Parses a storageUploadProgress event payload
    /// (`{"success":bool,"sessionId":string,...}`).
    static UploadProgressEvent parseUploadProgress(const std::string& payload);

    /// Parses a storageDownloadProgress event payload
    /// (`{"success":true,"sessionId":string,"chunk":base64,...}`),
    /// base64-decoding the chunk. Reports success=false (and leaves chunk
    /// empty) for a progress event that isn't a stream chunk for us — e.g. a
    /// downloadToUrl()-style progress event, which carries "bytes" but no
    /// "chunk" field.
    static DownloadProgressEvent parseDownloadProgress(const std::string& payload);

    /// Parses a storageDownloadDone event payload
    /// (`{"success":bool,"sessionId":string,...}`).
    static DownloadDoneEvent parseDownloadDone(const std::string& payload);

private:
    LogosModules& modules_;
};

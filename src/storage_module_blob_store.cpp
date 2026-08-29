#include "storage_module_blob_store.h"

#include <QString>

#include <nlohmann/json.hpp>

#include "base64.h"
#include "logos_sdk.h"
#include "logos_types.h"

using json = nlohmann::json;
using namespace cloud_data_core;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// Same guard as in delivery_module_transport.cpp: LogosResult's accessors
// throw when read against the wrong outcome.
std::string errorOf(const LogosResult& r) {
    return r.success ? std::string() : r.getError().toStdString();
}

std::string stringOf(const LogosResult& r) {
    if (!r.success) return {};
    return r.getString().toStdString();
}

} // namespace

StorageModuleBlobStore::StorageModuleBlobStore(LogosModules& modules) : modules_(modules) {}

BlobOpResult StorageModuleBlobStore::uploadInit(const std::string& filename, int64_t chunkSize) {
    LogosResult res = modules_.storage_module.uploadInit(qs(filename), static_cast<int>(chunkSize));
    if (!res.success) return {false, "", errorOf(res)};
    return {true, stringOf(res), ""};
}

BlobOpResult StorageModuleBlobStore::uploadChunk(const std::string& sessionId, const std::string& chunk) {
    LogosResult res = modules_.storage_module.uploadChunk(qs(sessionId), qs(chunk));
    return {res.success, "", errorOf(res)};
}

BlobOpResult StorageModuleBlobStore::uploadFinalize(const std::string& sessionId) {
    LogosResult res = modules_.storage_module.uploadFinalize(qs(sessionId));
    if (!res.success) return {false, "", errorOf(res)};
    return {true, stringOf(res), ""};
}

BlobOpResult StorageModuleBlobStore::downloadChunks(const std::string& cid, bool local, int64_t chunkSize) {
    LogosResult res = modules_.storage_module.downloadChunks(qs(cid), local, static_cast<int>(chunkSize));
    if (!res.success) return {false, "", errorOf(res)};
    return {true, stringOf(res), ""};
}

StorageModuleBlobStore::UploadProgressEvent StorageModuleBlobStore::parseUploadProgress(const std::string& payload) {
    UploadProgressEvent ev;
    json obj = json::parse(payload, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) return ev;
    ev.success = obj.value("success", false);
    ev.sessionId = obj.value("sessionId", std::string());
    return ev;
}

StorageModuleBlobStore::DownloadProgressEvent StorageModuleBlobStore::parseDownloadProgress(const std::string& payload) {
    DownloadProgressEvent ev;
    json obj = json::parse(payload, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) return ev;
    if (!obj.value("success", false)) return ev;
    const std::string sessionId = obj.value("sessionId", std::string());
    if (sessionId.empty() || !obj.contains("chunk")) return ev;

    ev.success = true;
    ev.sessionId = sessionId;
    const std::string decoded = base64Decode(obj["chunk"].get<std::string>());
    ev.chunk.assign(decoded.begin(), decoded.end());
    return ev;
}

StorageModuleBlobStore::DownloadDoneEvent StorageModuleBlobStore::parseDownloadDone(const std::string& payload) {
    DownloadDoneEvent ev;
    json obj = json::parse(payload, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) return ev;
    ev.sessionId = obj.value("sessionId", std::string());
    ev.success = obj.value("success", false);
    return ev;
}

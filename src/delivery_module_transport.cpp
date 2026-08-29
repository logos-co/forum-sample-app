#include "delivery_module_transport.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QString>
#include <QVariant>

#include <nlohmann/json.hpp>

#include <utility>

#include "base64.h"
#include "logos_sdk.h"
#include "logos_types.h"

using json = nlohmann::json;
using namespace cloud_data_core;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QByteArray qba(const std::vector<uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

// LogosResult::getError() throws when the result is a success, and
// getString()/getValue() throw when it is a failure — so never reach for
// either without checking .success first. These two keep that discipline in
// one place rather than at every call site below.
std::string errorOf(const LogosResult& r) {
    return r.success ? std::string() : r.getError().toStdString();
}

std::string stringOf(const LogosResult& r) {
    if (!r.success) return {};
    return r.getString().toStdString();
}

} // namespace

DeliveryModuleTransport::DeliveryModuleTransport(LogosModules& modules) : modules_(modules) {}

void DeliveryModuleTransport::setPublishObserver(PublishObserver observer) {
    publishObserver_ = std::move(observer);
}

TransportSendResult DeliveryModuleTransport::send(const std::string& contentTopic,
                                                    const std::vector<uint8_t>& payload) {
    LogosResult res = modules_.delivery_module.send(qs(contentTopic), qba(payload));
    if (!res.success) return {false, "", errorOf(res)};
    const std::string requestId = stringOf(res);
    if (publishObserver_) publishObserver_(requestId, payload);
    return {true, requestId, ""};
}

TransportResult DeliveryModuleTransport::subscribe(const std::string& contentTopic) {
    LogosResult res = modules_.delivery_module.subscribe(qs(contentTopic));
    return {res.success, errorOf(res)};
}

TransportResult DeliveryModuleTransport::unsubscribe(const std::string& contentTopic) {
    LogosResult res = modules_.delivery_module.unsubscribe(qs(contentTopic));
    return {res.success, errorOf(res)};
}

bool DeliveryModuleTransport::channelExists(const std::string& channelId) {
    // channelExists() answers with the verbatim FFI string "true"/"false", and
    // an unknown channel id is not an error — so anything that isn't a literal
    // "true" means "not open on this node yet".
    LogosResult res = modules_.delivery_module.channelExists(qs(channelId));
    if (!res.success) return false;
    return stringOf(res) == "true";
}

bool DeliveryModuleTransport::channelCreate(const std::string& channelId,
                                             const std::string& contentTopic,
                                             const std::string& peerId) {
    return modules_.delivery_module.channelCreate(qs(channelId), qs(contentTopic), qs(peerId)).success;
}

TransportSendResult DeliveryModuleTransport::channelSend(const std::string& channelId,
                                                           const std::vector<uint8_t>& payload) {
    LogosResult res = modules_.delivery_module.channelSend(qs(channelId), qba(payload));
    if (!res.success) return {false, "", errorOf(res)};
    const std::string requestId = stringOf(res);
    if (publishObserver_) publishObserver_(requestId, payload);
    return {true, requestId, ""};
}

std::vector<std::vector<uint8_t>> DeliveryModuleTransport::fetchHistory(const std::string& contentTopic,
                                                                          const std::string& peerAddr,
                                                                          const std::string& requestId,
                                                                          int64_t timeoutMs) {
    std::vector<std::vector<uint8_t>> out;

    json storeQueryReq;
    storeQueryReq["requestId"] = requestId;
    storeQueryReq["includeData"] = true;
    storeQueryReq["paginationForward"] = true;
    storeQueryReq["contentTopics"] = json::array({contentTopic});

    LogosResult res = modules_.delivery_module.storeQuery(qs(storeQueryReq.dump()), qs(peerAddr),
                                                          static_cast<int>(timeoutMs));
    if (!res.success) return out; // best-effort: storeQuery is explicitly unstable upstream

    // StoreQueryResponseHex: { "messages": [ { "message": { "payload": base64 } } ] }.
    // The Qt wrapper hands the FFI payload back as a QVariant, which may carry
    // the response either as a JSON string or as an already-structured
    // map/list — normalise both to JSON text before parsing.
    QString responseText = res.value.toString();
    if (responseText.isEmpty())
        responseText = QString::fromUtf8(QJsonDocument::fromVariant(res.value).toJson(QJsonDocument::Compact));

    json response = json::parse(responseText.toStdString(), nullptr, false);
    if (response.is_discarded() || !response.is_object() || !response.contains("messages")) return out;
    const json& messages = response["messages"];
    if (!messages.is_array()) return out;

    for (const auto& entry : messages) {
        if (!entry.is_object() || !entry.contains("message")) continue;
        const json& message = entry["message"];
        if (!message.is_object() || !message.contains("payload")) continue;
        if (!message["payload"].is_string()) continue;

        const std::string decoded = base64Decode(message["payload"].get<std::string>());
        out.emplace_back(decoded.begin(), decoded.end());
    }
    return out;
}

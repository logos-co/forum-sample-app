#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cloud_data_core/transport.h"

// The per-build aggregate of dependency wrappers behind LogosUiPluginContext's
// modules(). Forward-declared so this header stays free of the generated
// umbrella; the .cpp includes logos_sdk.h to make it complete, per the
// "generated umbrella in .cpp, not header" convention used across this repo.
struct LogosModules;

/// Implements cloud_data_core::Transport by forwarding to
/// modules().delivery_module.*.
///
/// A Qt-typed port of cloud-data-module's own src/delivery_module_transport.*.
/// It could not be reused verbatim: that module is `type: core`, so its
/// generated wrapper is std-typed (StdLogosResult / std::string /
/// std::vector<uint8_t>), whereas this app is `type: ui_qml` and gets a
/// Qt-typed wrapper (LogosResult / QString / QByteArray). Every method is
/// still a 1:1 forwarder; the only additions are the conversions at the
/// boundary.
///
/// The channel-vs-send *policy* lives in CloudDataEngine, not here. What this
/// class does own is delivery_module's wire format: channelExists()'s verbatim
/// "true"/"false" string, and the storeQuery request/response envelope decoded
/// by fetchHistory().
class DeliveryModuleTransport : public cloud_data_core::Transport {
public:
    /// Called after every successful publish with the request id delivery_module
    /// issued and the op payload it carried.
    ///
    /// The engine keys its own outbox by request id but reports nothing back
    /// per-op, and delivery_module's messageSent/messageError events name only
    /// the request id. This app still wants to show the delivery state of *one
    /// post* (see the .rep's messageStateChanged), so it needs the request id →
    /// message id mapping, and this is the only point where both are in hand.
    /// Decoding the op to recover the message id is the embedder's job, not
    /// this adapter's — hence a raw payload rather than a parsed op.
    using PublishObserver = std::function<void(const std::string& requestId,
                                               const std::vector<uint8_t>& payload)>;

    explicit DeliveryModuleTransport(LogosModules& modules);

    void setPublishObserver(PublishObserver observer);

    cloud_data_core::TransportSendResult send(const std::string& contentTopic,
                                               const std::vector<uint8_t>& payload) override;
    cloud_data_core::TransportResult subscribe(const std::string& contentTopic) override;
    cloud_data_core::TransportResult unsubscribe(const std::string& contentTopic) override;

    bool channelExists(const std::string& channelId) override;
    bool channelCreate(const std::string& channelId, const std::string& contentTopic,
                        const std::string& peerId) override;
    cloud_data_core::TransportSendResult channelSend(const std::string& channelId,
                                                       const std::vector<uint8_t>& payload) override;

    std::vector<std::vector<uint8_t>> fetchHistory(const std::string& contentTopic,
                                                    const std::string& peerAddr,
                                                    const std::string& requestId,
                                                    int64_t timeoutMs) override;

private:
    LogosModules& modules_;
    PublishObserver publishObserver_;
};

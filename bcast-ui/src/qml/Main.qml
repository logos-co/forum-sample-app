import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    // One consistently-tagged line per QML lifecycle / callback. console.log
    // is routed to stderr by Qt's default message handler, so these land in
    // the same stream as the C++ backend's std::cerr lines.
    function log(msg) { console.log("[broadcast_app qml] " + msg); }

    // Typed replica — auto-synced properties and callable slots.
    readonly property var backend: logos.module("broadcast_app")
    property bool ready: false

    // PROPs from the .rep file, auto-updated via QtRO.
    readonly property string status:    backend ? backend.status    : ""
    readonly property bool   nodeReady: backend ? backend.nodeReady : false
    readonly property string topic:     backend ? backend.topic     : ""

    onStatusChanged: log("status -> \"" + status + "\"")

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            root.log("onViewModuleReadyChanged(module=\"" + moduleName + "\", ready=" + isReady + ")");
            if (moduleName === "broadcast_app")
                root.ready = isReady && root.backend !== null;
        }
    }

    // Inbound plain-text messages, pushed by the backend from delivery_module's
    // messageReceived event.
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onMessageReceived(text, timestamp) {
            root.log("messageReceived -> \"" + text + "\"");
            root.appendMessage(text, "in", timestamp);
        }
    }

    Component.onCompleted: {
        log("Component.onCompleted — view created");
        root.ready = root.backend !== null && logos.isViewModuleReady("broadcast_app");
    }
    Component.onDestruction: log("Component.onDestruction — view torn down")

    // delivery_module timestamps are nanoseconds since the Unix epoch; ms is
    // plenty for display. A 0/absent timestamp falls back to now.
    function formatTs(ts) {
        var d = ts ? new Date(Math.floor(ts / 1000000)) : new Date();
        return Qt.formatDateTime(d, "hh:mm:ss");
    }

    function appendMessage(body, origin, ts) {
        messagesModel.append({ body: body, origin: origin, ts: root.formatTs(ts) });
        Qt.callLater(messageView.positionViewAtEnd);
    }

    function send() {
        var text = input.text;
        if (!text || !root.nodeReady) return;
        root.log("Send clicked — calling backend.sendMessage(\"" + text + "\")");
        // logos.watch() delivers the pending reply via callbacks. sendMessage
        // returns "" on success, or an error string on failure.
        logos.watch(backend.sendMessage(text), function (err) {
            if (err) {
                root.log("send reply (error) -> " + err);
                root.appendMessage("⚠ " + err, "err", 0);
            } else {
                root.log("send reply (success)");
                // Echo our own message locally — the network won't loop it back.
                root.appendMessage(text, "out", 0);
            }
        }, function (e) {
            root.log("send reply (transport error) -> " + e);
            root.appendMessage("⚠ " + e, "err", 0);
        });
        input.clear();
    }

    ListModel { id: messagesModel }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Text {
            text: "Broadcast App"
            font.pixelSize: 20
            color: "#ffffff"
        }

        // Hard-coded topic this app listens on and broadcasts to.
        Text {
            text: "Topic: " + (root.topic.length > 0 ? root.topic : "—")
            color: "#8b949e"
            font.pixelSize: 12
            font.family: "monospace"
        }

        // Node / connection status.
        Text {
            text: (root.nodeReady ? "● " : "○ ") + (root.status.length > 0 ? root.status : "Connecting to backend…")
            color: root.nodeReady ? "#56d364" : "#f0883e"
            font.pixelSize: 12
        }

        // Message log (received, sent, and errors).
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0d1117"
            border.color: "#30363d"
            radius: 6

            ListView {
                id: messageView
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                spacing: 6
                model: messagesModel

                delegate: RowLayout {
                    width: ListView.view ? ListView.view.width : implicitWidth
                    spacing: 8

                    Text {
                        text: model.ts
                        color: "#6e7681"
                        font.pixelSize: 11
                        font.family: "monospace"
                        Layout.alignment: Qt.AlignTop
                    }
                    Text {
                        Layout.fillWidth: true
                        text: model.body
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        color: model.origin === "out" ? "#58a6ff"
                             : model.origin === "err" ? "#f85149"
                             :                           "#e6edf3"
                        font.pixelSize: 13
                    }
                }
            }
        }

        // Composer — send plain text to the topic.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: input
                Layout.fillWidth: true
                placeholderText: root.nodeReady ? "Type a message…" : "Waiting for node…"
                enabled: root.nodeReady
                onAccepted: root.send()
            }
            Button {
                text: "Send"
                enabled: root.nodeReady && input.text.length > 0
                onClicked: root.send()
            }
        }
    }
}

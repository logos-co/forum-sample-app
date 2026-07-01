import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root

    // One consistently-tagged line per QML lifecycle / callback. console.log is
    // routed to stderr by Qt's default message handler, so these land in the
    // same stream as the C++ backend's std::cerr lines.
    function log(msg) { console.log("[broadcast_app qml] " + msg); }

    // Typed replica — auto-synced properties and callable slots.
    readonly property var backend: logos.module("broadcast_app")
    property bool ready: false

    // Monospace family for code-like values (the topic id). The design system
    // ships no mono token, so centralise the generic family here — Qt maps
    // "monospace" to the platform's fixed-pitch font.
    readonly property string monoFont: "monospace"

    // PROPs from the .rep file, auto-updated via QtRO.
    readonly property string status:    backend ? backend.status    : ""
    readonly property bool   nodeReady: backend ? backend.nodeReady : false
    readonly property string topic:     backend ? backend.topic     : ""

    // Currently opened topic (the thread shown on the right), and a transient
    // error line from the last create/reply attempt.
    property string selectedTopicId: ""
    property string selectedTitle: ""
    property string selectedBody: ""
    property string lastError: ""

    onStatusChanged: log("status -> \"" + status + "\"")

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "broadcast_app")
                root.ready = isReady && root.backend !== null;
        }
    }

    // Inbound forum messages, pushed by the backend from delivery_module (and
    // echoed locally for our own posts).
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onTopicReceived(id, title, body, timestamp) {
            root.log("topicReceived -> " + id);
            root.addTopic(id, title, body, timestamp);
        }
        function onReplyReceived(id, topicId, body, timestamp) {
            root.log("replyReceived -> " + id + " on " + topicId);
            root.addReply(id, topicId, body, timestamp);
        }
    }

    Component.onCompleted: {
        log("Component.onCompleted — view created");
        root.ready = root.backend !== null && logos.isViewModuleReady("broadcast_app");
    }
    Component.onDestruction: log("Component.onDestruction — view torn down")

    // ── Models ────────────────────────────────────────────────────────────────
    // Flat topic + reply stores, plus the reply list for the open topic.
    ListModel { id: topicsModel }   // { tid, title, body, ts, replies }
    ListModel { id: repliesModel }  // { rid, topicId, body, ts }
    ListModel { id: threadModel }   // replies for selectedTopicId (display)

    // delivery_module timestamps are nanoseconds since the Unix epoch; ms is
    // plenty for display. A 0/absent timestamp falls back to now.
    function formatTs(ts) {
        var d = ts ? new Date(Math.floor(ts / 1000000)) : new Date();
        return Qt.formatDateTime(d, "MM-dd hh:mm:ss");
    }

    function findTopicIndex(tid) {
        for (var i = 0; i < topicsModel.count; ++i)
            if (topicsModel.get(i).tid === tid) return i;
        return -1;
    }

    function replyExists(rid) {
        for (var i = 0; i < repliesModel.count; ++i)
            if (repliesModel.get(i).rid === rid) return true;
        return false;
    }

    function countRepliesFor(tid) {
        var n = 0;
        for (var i = 0; i < repliesModel.count; ++i)
            if (repliesModel.get(i).topicId === tid) ++n;
        return n;
    }

    function addTopic(id, title, body, ts) {
        if (root.findTopicIndex(id) >= 0) return;   // de-dupe (self-echo / network echo)
        topicsModel.append({
            tid: id, title: title, body: body,
            ts: root.formatTs(ts),
            replies: root.countRepliesFor(id)       // catch up any orphan replies
        });
    }

    function addReply(id, topicId, body, ts) {
        if (root.replyExists(id)) return;           // de-dupe
        repliesModel.append({ rid: id, topicId: topicId, body: body, ts: root.formatTs(ts) });

        // Bump the parent topic's reply count, if we know the topic yet.
        var ti = root.findTopicIndex(topicId);
        if (ti >= 0)
            topicsModel.setProperty(ti, "replies", topicsModel.get(ti).replies + 1);

        // If this reply belongs to the open thread, show it immediately.
        if (topicId === root.selectedTopicId)
            threadModel.append({ rid: id, body: body, ts: root.formatTs(ts) });
    }

    function openTopic(tid) {
        var i = root.findTopicIndex(tid);
        if (i < 0) return;
        var t = topicsModel.get(i);
        root.selectedTopicId = tid;
        root.selectedTitle = t.title;
        root.selectedBody = t.body;
        threadModel.clear();
        for (var j = 0; j < repliesModel.count; ++j) {
            var r = repliesModel.get(j);
            if (r.topicId === tid)
                threadModel.append({ rid: r.rid, body: r.body, ts: r.ts });
        }
    }

    // ── Actions (drive the backend slots) ──────────────────────────────────────
    function createTopic() {
        if (!root.nodeReady || titleField.text.length === 0) return;
        root.lastError = "";
        var title = titleField.text, body = bodyField.text;
        root.log("createTopic(\"" + title + "\")");
        logos.watch(backend.createTopic(title, body), function (err) {
            if (err) { root.lastError = err; root.log("createTopic error -> " + err); }
            else { titleField.text = ""; bodyField.text = ""; }
        }, function (e) { root.lastError = e; });
    }

    function sendReply() {
        if (!root.nodeReady || root.selectedTopicId.length === 0 || replyField.text.length === 0) return;
        root.lastError = "";
        var body = replyField.text;
        root.log("replyToTopic(" + root.selectedTopicId + ")");
        logos.watch(backend.replyToTopic(root.selectedTopicId, body), function (err) {
            if (err) { root.lastError = err; root.log("reply error -> " + err); }
            else { replyField.text = ""; }
        }, function (e) { root.lastError = e; });
    }

    // ── Layout ──────────────────────────────────────────────────────────────────
    // Fill the view with the theme background — the host window is transparent
    // underneath, so every screen paints its own surface.
    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.large
        spacing: Theme.spacing.small

        // Header
        LogosText {
            text: "Broadcast Forum"
            font.pixelSize: Theme.typography.panelTitleText
            font.weight: Theme.typography.weightBold
            color: Theme.palette.text
        }
        LogosText {
            text: "Topic: " + (root.topic.length > 0 ? root.topic : "—")
            color: Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
            font.family: root.monoFont
        }
        LogosText {
            text: (root.nodeReady ? "● " : "○ ") + (root.status.length > 0 ? root.status : "Connecting to backend…")
            color: root.nodeReady ? Theme.palette.success : Theme.palette.warning
            font.pixelSize: Theme.typography.secondaryText
        }
        LogosText {
            visible: root.lastError.length > 0
            text: "⚠ " + root.lastError
            color: Theme.palette.error
            font.pixelSize: Theme.typography.secondaryText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        // Two-pane forum: topics (left) | thread (right)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.medium

            // ── Left: new-topic composer + topic list ──────────────────────────
            ColumnLayout {
                Layout.preferredWidth: 260
                Layout.minimumWidth: 220
                Layout.fillHeight: true
                spacing: Theme.spacing.small

                LogosText {
                    text: "Topics"
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightBold
                }

                LogosTextField {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: "New topic title…"
                    enabled: root.nodeReady
                }
                // LogosTextField wraps its TextInput, so Enter is handled on the
                // inner input rather than via an onAccepted on the control.
                Connections {
                    target: titleField.textInput
                    function onAccepted() { bodyField.textInput.forceActiveFocus() }
                }
                LogosTextField {
                    id: bodyField
                    Layout.fillWidth: true
                    placeholderText: "Opening message (optional)…"
                    enabled: root.nodeReady
                }
                Connections {
                    target: bodyField.textInput
                    function onAccepted() { root.createTopic() }
                }
                LogosButton {
                    text: "Create topic"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    implicitHeight: 40
                    enabled: root.nodeReady && titleField.text.length > 0
                    onClicked: root.createTopic()
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.palette.backgroundInset
                    border.color: Theme.palette.borderHairline
                    border.width: 1
                    radius: Theme.spacing.radiusMedium

                    ListView {
                        id: topicList
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.tiny
                        clip: true
                        spacing: Theme.spacing.tiny
                        model: topicsModel

                        delegate: Rectangle {
                            width: ListView.view ? ListView.view.width : 0
                            implicitHeight: tcol.implicitHeight + Theme.spacing.medium
                            radius: Theme.spacing.radiusSmall
                            color: model.tid === root.selectedTopicId ? Theme.palette.overlayOrange : Theme.palette.backgroundSecondary
                            border.width: 1
                            border.color: model.tid === root.selectedTopicId ? Theme.palette.primary : Theme.palette.borderHairline

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.openTopic(model.tid)
                            }

                            ColumnLayout {
                                id: tcol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: Theme.spacing.small
                                spacing: 2

                                LogosText {
                                    Layout.fillWidth: true
                                    text: model.title
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightBold
                                    elide: Text.ElideRight
                                }
                                LogosText {
                                    Layout.fillWidth: true
                                    text: model.replies + (model.replies === 1 ? " reply · " : " replies · ") + model.ts
                                    color: Theme.palette.textTertiary
                                    font.pixelSize: Theme.typography.secondaryText
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }
            }

            // ── Right: selected thread + reply composer ────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.spacing.small
                visible: root.selectedTopicId.length > 0

                LogosText {
                    Layout.fillWidth: true
                    text: root.selectedTitle
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightBold
                    wrapMode: Text.WordWrap
                }
                LogosText {
                    Layout.fillWidth: true
                    visible: root.selectedBody.length > 0
                    text: root.selectedBody
                    color: Theme.palette.textSecondary
                    font.pixelSize: Theme.typography.primaryText
                    wrapMode: Text.WordWrap
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.palette.backgroundInset
                    border.color: Theme.palette.borderHairline
                    border.width: 1
                    radius: Theme.spacing.radiusMedium

                    ListView {
                        id: threadView
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.small
                        clip: true
                        spacing: Theme.spacing.small
                        model: threadModel

                        delegate: ColumnLayout {
                            width: ListView.view ? ListView.view.width : 0
                            spacing: 1
                            LogosText {
                                text: model.ts
                                color: Theme.palette.textTertiary
                                font.pixelSize: Theme.typography.secondaryText
                                font.family: root.monoFont
                            }
                            LogosText {
                                Layout.fillWidth: true
                                text: model.body
                                color: Theme.palette.text
                                font.pixelSize: Theme.typography.primaryText
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small
                    LogosTextField {
                        id: replyField
                        Layout.fillWidth: true
                        placeholderText: root.nodeReady ? "Write a reply…" : "Waiting for node…"
                        enabled: root.nodeReady
                    }
                    Connections {
                        target: replyField.textInput
                        function onAccepted() { root.sendReply() }
                    }
                    LogosButton {
                        text: "Reply"
                        Layout.preferredWidth: 88
                        Layout.preferredHeight: 40
                        implicitWidth: 88
                        implicitHeight: 40
                        enabled: root.nodeReady && replyField.text.length > 0
                        onClicked: root.sendReply()
                    }
                }
            }

            // Placeholder when no topic is open.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.selectedTopicId.length === 0
                LogosText {
                    anchors.centerIn: parent
                    text: topicsModel.count > 0 ? "Select a topic to open it" : "No topics yet — create one"
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.primaryText
                }
            }
        }
    }
}

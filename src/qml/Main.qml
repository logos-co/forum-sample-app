import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    // One consistently-tagged line per QML lifecycle / callback. console.log is
    // routed to stderr by Qt's default message handler, so these land in the
    // same stream as the C++ backend's std::cerr lines.
    function log(msg) { console.log("[broadcast_app qml] " + msg); }

    // Typed replica — auto-synced properties and callable slots.
    readonly property var backend: logos.module("broadcast_app")
    property bool ready: false

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
            else { titleField.clear(); bodyField.clear(); }
        }, function (e) { root.lastError = e; });
    }

    function sendReply() {
        if (!root.nodeReady || root.selectedTopicId.length === 0 || replyField.text.length === 0) return;
        root.lastError = "";
        var body = replyField.text;
        root.log("replyToTopic(" + root.selectedTopicId + ")");
        logos.watch(backend.replyToTopic(root.selectedTopicId, body), function (err) {
            if (err) { root.lastError = err; root.log("reply error -> " + err); }
            else { replyField.clear(); }
        }, function (e) { root.lastError = e; });
    }

    // ── Layout ──────────────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        // Header
        Text {
            text: "Broadcast Forum"
            font.pixelSize: 20
            color: "#ffffff"
        }
        Text {
            text: "Topic: " + (root.topic.length > 0 ? root.topic : "—")
            color: "#8b949e"
            font.pixelSize: 11
            font.family: "monospace"
        }
        Text {
            text: (root.nodeReady ? "● " : "○ ") + (root.status.length > 0 ? root.status : "Connecting to backend…")
            color: root.nodeReady ? "#56d364" : "#f0883e"
            font.pixelSize: 12
        }
        Text {
            visible: root.lastError.length > 0
            text: "⚠ " + root.lastError
            color: "#f85149"
            font.pixelSize: 12
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        // Two-pane forum: topics (left) | thread (right)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // ── Left: new-topic composer + topic list ──────────────────────────
            ColumnLayout {
                Layout.preferredWidth: 260
                Layout.minimumWidth: 220
                Layout.fillHeight: true
                spacing: 8

                Text { text: "Topics"; color: "#e6edf3"; font.pixelSize: 14; font.bold: true }

                TextField {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: "New topic title…"
                    enabled: root.nodeReady
                    onAccepted: bodyField.forceActiveFocus()
                }
                TextField {
                    id: bodyField
                    Layout.fillWidth: true
                    placeholderText: "Opening message (optional)…"
                    enabled: root.nodeReady
                    onAccepted: root.createTopic()
                }
                Button {
                    text: "Create topic"
                    Layout.fillWidth: true
                    enabled: root.nodeReady && titleField.text.length > 0
                    onClicked: root.createTopic()
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#0d1117"
                    border.color: "#30363d"
                    radius: 6

                    ListView {
                        id: topicList
                        anchors.fill: parent
                        anchors.margins: 6
                        clip: true
                        spacing: 4
                        model: topicsModel

                        delegate: Rectangle {
                            width: ListView.view ? ListView.view.width : 0
                            implicitHeight: tcol.implicitHeight + 12
                            radius: 5
                            color: model.tid === root.selectedTopicId ? "#1f6feb33" : "#161b22"
                            border.color: model.tid === root.selectedTopicId ? "#1f6feb" : "#30363d"

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
                                anchors.margins: 8
                                spacing: 2

                                Text {
                                    Layout.fillWidth: true
                                    text: model.title
                                    color: "#e6edf3"
                                    font.pixelSize: 13
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: model.replies + (model.replies === 1 ? " reply · " : " replies · ") + model.ts
                                    color: "#8b949e"
                                    font.pixelSize: 10
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
                spacing: 8
                visible: root.selectedTopicId.length > 0

                Text {
                    Layout.fillWidth: true
                    text: root.selectedTitle
                    color: "#ffffff"
                    font.pixelSize: 16
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.selectedBody.length > 0
                    text: root.selectedBody
                    color: "#c9d1d9"
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#0d1117"
                    border.color: "#30363d"
                    radius: 6

                    ListView {
                        id: threadView
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        spacing: 6
                        model: threadModel

                        delegate: ColumnLayout {
                            width: ListView.view ? ListView.view.width : 0
                            spacing: 1
                            Text {
                                text: model.ts
                                color: "#6e7681"
                                font.pixelSize: 10
                                font.family: "monospace"
                            }
                            Text {
                                Layout.fillWidth: true
                                text: model.body
                                color: "#e6edf3"
                                font.pixelSize: 13
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: replyField
                        Layout.fillWidth: true
                        placeholderText: root.nodeReady ? "Write a reply…" : "Waiting for node…"
                        enabled: root.nodeReady
                        onAccepted: root.sendReply()
                    }
                    Button {
                        text: "Reply"
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
                Text {
                    anchors.centerIn: parent
                    text: topicsModel.count > 0 ? "Select a topic to open it" : "No topics yet — create one"
                    color: "#6e7681"
                    font.pixelSize: 13
                }
            }
        }
    }
}

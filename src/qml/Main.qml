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
    function log(msg) { console.log("[example_forum qml] " + msg); }

    // Typed replica — auto-synced properties and callable slots.
    readonly property var backend: logos.module("example_forum")
    property bool ready: false

    // Monospace family for code-like values (the topic id). The design system
    // ships no mono token, so centralise the generic family here — Qt maps
    // "monospace" to the platform's fixed-pitch font.
    readonly property string monoFont: "monospace"

    // PROPs from the .rep file, auto-updated via QtRO.
    readonly property string status:     backend ? backend.status     : ""
    readonly property bool   nodeReady:  backend ? backend.nodeReady  : false
    readonly property string topic:      backend ? backend.topic      : ""
    readonly property string appVersion: backend ? backend.appVersion : ""
    readonly property string myAddress:  backend ? backend.myAddress  : ""

    // Short display form for a signer address: first 6 + last 4 hex chars.
    // `author` is a claimed signer, not a verified one — this app can't check
    // a signature yet (see example_forum.rep's topicReceived doc comment) —
    // so this is a label, not a trust indicator.
    // shortAddress("{\"address\":\"0xdFcd008F17647543e024d2db17dEDf4B23c4dfC3\"") → "0xdfcd...dfc3"
    function shortAddress(addr) {
        if (!addr || addr.length <= 12) return addr || "";
        return addr.substring(12, 18) + "…" + addr.substring(addr.length - 6, addr.length - 2);
    }

    // Currently opened topic (the thread shown on the right), and a transient
    // error line from the last create/reply attempt.
    property string selectedTopicId: ""
    property string selectedTitle: ""
    property string selectedBody: ""
    // True while the open topic is a placeholder backfilled from a reply (its
    // real topic message hasn't arrived) — drives the "restore from title" pane.
    property bool selectedIsPlaceholder: false
    property string lastError: ""

    onStatusChanged: log("status -> \"" + status + "\"")

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "example_forum")
                root.ready = isReady && root.backend !== null;
        }
    }

    // Inbound forum messages, pushed by the backend from delivery_module (and
    // echoed locally for our own posts).
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onTopicReceived(id, title, body, author, timestamp) {
            root.log("topicReceived -> " + id);
            root.addTopic(id, title, body, author, timestamp);
        }
        function onReplyReceived(id, topicId, body, author, timestamp) {
            root.log("replyReceived -> " + id + " on " + topicId);
            root.addReply(id, topicId, body, author, timestamp);
        }
    }

    Component.onCompleted: {
        log("Component.onCompleted — view created");
        root.ready = root.backend !== null && logos.isViewModuleReady("example_forum");
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

    function addTopic(id, title, body, author, ts) {
        var i = root.findTopicIndex(id);
        if (i >= 0) {
            // Already known. If it's a placeholder we backfilled from an early
            // reply, fill in the real title/body now; otherwise it's a
            // self/network echo and we leave the existing row untouched.
            if (topicsModel.get(i).placeholder)
                root.fillTopic(i, title, body, author, ts);
            return;
        }
        topicsModel.append({
            tid: id, title: title, body: body, author: author || "",
            ts: root.formatTs(ts),
            replies: root.countRepliesFor(id),      // catch up any orphan replies
            placeholder: false
        });
    }

    // A reply can outrun the topic it belongs to (out-of-order delivery, or we
    // joined the channel after the topic was posted). Stand up a placeholder
    // topic so the reply is visible and openable; addTopic() promotes it to a
    // real topic in place once the topic message arrives.
    function backfillTopic(topicId, ts) {
        topicsModel.append({
            tid: topicId,
            title: "⏳ " + topicId.substring(0, 8),
            body: "",
            author: "",
            ts: root.formatTs(ts),
            replies: 0,
            placeholder: true
        });
    }

    // Promote the placeholder at row `i` into a real topic, keeping any thread
    // the user has already opened on it in sync.
    function fillTopic(i, title, body, author, ts) {
        topicsModel.setProperty(i, "title", title);
        topicsModel.setProperty(i, "body", body);
        topicsModel.setProperty(i, "author", author || "");
        topicsModel.setProperty(i, "ts", root.formatTs(ts));
        topicsModel.setProperty(i, "placeholder", false);
        if (topicsModel.get(i).tid === root.selectedTopicId) {
            root.selectedTitle = title;
            root.selectedBody = body;
            root.selectedIsPlaceholder = false;
        }
    }

    function addReply(id, topicId, body, author, ts) {
        if (root.replyExists(id)) return;           // de-dupe
        repliesModel.append({ rid: id, topicId: topicId, body: body, author: author || "", ts: root.formatTs(ts) });

        // Bump the parent topic's reply count, backfilling a placeholder topic
        // first if the reply arrived before its topic.
        var ti = root.findTopicIndex(topicId);
        if (ti < 0) {
            root.backfillTopic(topicId, ts);
            ti = root.findTopicIndex(topicId);
        }
        topicsModel.setProperty(ti, "replies", topicsModel.get(ti).replies + 1);

        // If this reply belongs to the open thread, show it immediately.
        if (topicId === root.selectedTopicId)
            threadModel.append({ rid: id, body: body, author: author || "", ts: root.formatTs(ts) });
    }

    function openTopic(tid) {
        var i = root.findTopicIndex(tid);
        if (i < 0) return;
        var t = topicsModel.get(i);
        root.selectedTopicId = tid;
        root.selectedTitle = t.title;
        root.selectedBody = t.body;
        root.selectedIsPlaceholder = t.placeholder === true;
        threadModel.clear();
        for (var j = 0; j < repliesModel.count; ++j) {
            var r = repliesModel.get(j);
            if (r.topicId === tid)
                threadModel.append({ rid: r.rid, body: r.body, author: r.author, ts: r.ts });
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

    // Restore an open placeholder topic from a title pasted by the user. The
    // backend verifies the title hashes to the topic id, then emits topicReceived
    // (routed back through addTopic → fillTopic), so no local state is touched here.
    function restoreTopic() {
        if (!root.ready || restoreField.text.length === 0) return;
        root.lastError = "";
        var title = restoreField.text;
        root.log("reconstructTopic(" + root.selectedTopicId + ")");
        logos.watch(backend.reconstructTopic(root.selectedTopicId, title), function (err) {
            if (err) { root.lastError = err; root.log("reconstruct error -> " + err); }
            else { restoreField.text = ""; }
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

    // LogosButton is pointer-only and sits outside the Tab focus chain. This
    // wrapper turns any button into a proper tab stop: it joins the loop, draws a
    // focus ring so the current stop is visible, and fires clicked() on
    // Enter/Space just like a mouse press.
    component FocusButton: LogosButton {
        id: btn
        activeFocusOnTab: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                    || event.key === Qt.Key_Space) {
                btn.clicked();
                event.accepted = true;
            }
        }
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            radius: btn.radius
            border.width: 2
            border.color: Theme.palette.overlayOrange
            visible: btn.activeFocus
        }
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

        // Header — version is sourced from metadata.json via the backend PROP.
        LogosText {
            text: "Example Forum" + (root.appVersion.length > 0 ? " v" + root.appVersion : "")
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
            // Own signing identity — a fresh keypair minted on first run and
            // persisted locally thereafter (see ensureIdentity() in the backend).
            text: root.myAddress.length > 0
                  ? "Posting as " + root.shortAddress(root.myAddress)
                  : "Preparing identity…"
            color: Theme.palette.textTertiary
            font.pixelSize: Theme.typography.secondaryText
            font.family: root.monoFont
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
                    // Join the Tab focus loop — LogosTextField's inner TextInput has
                    // activeFocusOnTab off by default, so Qt's chain skips it otherwise.
                    Component.onCompleted: textInput.activeFocusOnTab = true
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
                    Component.onCompleted: textInput.activeFocusOnTab = true
                }
                Connections {
                    target: bodyField.textInput
                    function onAccepted() { root.createTopic() }
                }
                FocusButton {
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
                                    color: model.placeholder ? Theme.palette.textSecondary : Theme.palette.text
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightBold
                                    font.italic: model.placeholder === true
                                    elide: Text.ElideRight
                                }
                                LogosText {
                                    Layout.fillWidth: true
                                    text: model.placeholder
                                          ? model.replies + (model.replies === 1 ? " reply · awaiting topic…" : " replies · awaiting topic…")
                                          : model.replies + (model.replies === 1 ? " reply · " : " replies · ") + model.ts
                                              + (model.author ? " · by " + root.shortAddress(model.author) : "")
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

                TextEdit {
                    Layout.fillWidth: true
                    text: root.selectedTitle
                    readOnly: true
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    color: Theme.palette.text
                    selectionColor: Theme.palette.primary
                    font.family: Theme.typography.publicSans
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightBold
                    wrapMode: TextEdit.WordWrap
                }
                TextEdit {
                    Layout.fillWidth: true
                    visible: root.selectedBody.length > 0
                    text: root.selectedBody
                    readOnly: true
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    color: Theme.palette.textSecondary
                    selectionColor: Theme.palette.primary
                    font.family: Theme.typography.publicSans
                    font.pixelSize: Theme.typography.primaryText
                    wrapMode: TextEdit.WordWrap
                }

                // Placeholder recovery: this topic is known only from its replies.
                // Paste the title (shared out-of-band) to restore it — the backend
                // accepts it only if it hashes to this topic's id.
                Rectangle {
                    Layout.fillWidth: true
                    visible: root.selectedIsPlaceholder
                    color: Theme.palette.backgroundInset
                    border.color: Theme.palette.borderHairline
                    border.width: 1
                    radius: Theme.spacing.radiusMedium
                    implicitHeight: restoreCol.implicitHeight + Theme.spacing.medium

                    ColumnLayout {
                        id: restoreCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: Theme.spacing.small
                        spacing: Theme.spacing.tiny

                        LogosText {
                            Layout.fillWidth: true
                            text: "This topic hasn't arrived yet. Paste its title to restore it:"
                            color: Theme.palette.textSecondary
                            font.pixelSize: Theme.typography.secondaryText
                            wrapMode: Text.WordWrap
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small
                            LogosTextField {
                                id: restoreField
                                Layout.fillWidth: true
                                placeholderText: "Topic title…"
                                enabled: root.ready
                                Component.onCompleted: textInput.activeFocusOnTab = true
                            }
                            Connections {
                                target: restoreField.textInput
                                function onAccepted() { root.restoreTopic() }
                            }
                            FocusButton {
                                text: "Restore"
                                Layout.preferredWidth: 88
                                Layout.preferredHeight: 40
                                implicitWidth: 88
                                implicitHeight: 40
                                enabled: root.ready && restoreField.text.length > 0
                                onClicked: root.restoreTopic()
                            }
                        }
                    }
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
                                text: model.ts + (model.author ? " · by " + root.shortAddress(model.author) : "")
                                color: Theme.palette.textTertiary
                                font.pixelSize: Theme.typography.secondaryText
                                font.family: root.monoFont
                            }
                            TextEdit {
                                Layout.fillWidth: true
                                text: model.body
                                readOnly: true
                                selectByMouse: true
                                textFormat: TextEdit.PlainText
                                color: Theme.palette.text
                                selectionColor: Theme.palette.primary
                                font.family: Theme.typography.publicSans
                                font.pixelSize: Theme.typography.primaryText
                                wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
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
                        Component.onCompleted: textInput.activeFocusOnTab = true
                    }
                    Connections {
                        target: replyField.textInput
                        function onAccepted() { root.sendReply() }
                    }
                    FocusButton {
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

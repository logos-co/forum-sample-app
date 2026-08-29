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
    readonly property string myLabel:    backend ? backend.myLabel    : ""
    readonly property string accountsJson: backend ? backend.accountsJson : "[]"

    // Short display form for a signer key id: first 6 + last 4 hex chars.
    // `author` is a claimed signer, not a verified one — this app can't check
    // a signature yet (see example_forum.rep's topicReceived doc comment) —
    // so this is a label, not a trust indicator.
    // shortAddress("a3f0c41d9b27e5106d84b3f27e109c2b") → "a3f0c4…9c2b"
    function shortAddress(addr) {
        if (!addr || addr.length <= 12) return addr || "";
        return addr.substring(0, 6) + "…" + addr.substring(addr.length - 4);
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
    // Errors from the account dialogs. Separate from lastError because those
    // dialogs are modal — an error shown on the main error line would sit
    // behind the very dialog that has to report it.
    property string accountError: ""

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
        function onMessageStateChanged(id, state, detail) {
            root.log("messageStateChanged -> " + id + " " + state
                     + (detail.length > 0 ? " (" + detail + ")" : ""));
            root.setMessageState(id, state, detail);
        }
    }

    // Posts already in the local store, pulled once the replica is up. This
    // cannot be a startup signal from the backend: the backend finishes its
    // bootstrap before this view exists, and QtRO delivers a signal only to
    // replicas connected at the time it is emitted. So the view asks.
    property bool backlogLoaded: false

    function loadBacklog() {
        if (root.backlogLoaded || !root.ready || !root.backend) return;
        root.backlogLoaded = true;
        logos.watch(backend.loadBacklog(), function (json) {
            var list = [];
            try {
                list = JSON.parse(json);
            } catch (e) {
                root.log("could not parse backlog: " + e);
                return;
            }
            for (var i = 0; i < list.length; ++i) {
                var e = list[i];
                var ts = Number(e.ts);
                if (e.kind === "topic")
                    root.addTopic(e.id, e.title, e.body, e.author, ts);
                else if (e.kind === "reply")
                    root.addReply(e.id, e.topicId, e.body, e.author, ts);
            }
            root.log("backlog restored: " + list.length + " post(s)");
        }, function (err) {
            root.log("backlog load failed: " + err);
            // Let a later readiness change try again rather than showing an
            // empty forum for a store we know has posts in it.
            root.backlogLoaded = false;
        });
    }

    onReadyChanged: root.loadBacklog()

    Component.onCompleted: {
        log("Component.onCompleted — view created");
        root.ready = root.backend !== null && logos.isViewModuleReady("example_forum");
        // The replica may already hold accounts by now, in which case
        // onAccountsJsonChanged has come and gone before this view existed.
        root.rebuildAccounts();
        // Same reasoning, for the stored posts — ready may already be true here,
        // in which case onReadyChanged has been and gone too.
        root.loadBacklog();
    }
    Component.onDestruction: log("Component.onDestruction — view torn down")

    // ── Models ────────────────────────────────────────────────────────────────
    // Flat topic + reply stores, plus the reply list for the open topic.
    // `delivery` carries the send state of a post *we* made ("pending" →
    // "propagated" → "sent", or "failed"); it stays "" for everyone else's.
    ListModel { id: topicsModel }   // { tid, title, body, ts, replies, delivery }
    ListModel { id: repliesModel }  // { rid, topicId, body, ts, delivery }
    ListModel { id: threadModel }   // replies for selectedTopicId (display)

    // ── Accounts ──────────────────────────────────────────────────────────────
    // Mirrors the backend's accountsJson PROP, which is re-published whole on
    // every create/select/rename/delete — so this is rebuilt rather than
    // patched. `display` is precomputed because ComboBox's textRole needs a
    // single role to show.
    ListModel { id: accountsModel } // { keyId, label, display }

    function rebuildAccounts() {
        accountsModel.clear();
        var list = [];
        try {
            list = JSON.parse(root.accountsJson);
        } catch (e) {
            root.log("could not parse accountsJson: " + e);
        }
        for (var i = 0; i < list.length; ++i) {
            accountsModel.append({
                keyId: list[i].keyId,
                label: list[i].label,
                display: list[i].label + " (" + root.shortAddress(list[i].keyId) + ")"
            });
        }
        root.syncAccountCombo();
    }

    function accountIndexOf(keyId) {
        for (var i = 0; i < accountsModel.count; ++i)
            if (accountsModel.get(i).keyId === keyId) return i;
        return -1;
    }

    // Point the combo at whichever account the backend says is selected.
    // Assigned imperatively, not bound: activating the combo writes
    // currentIndex itself, which would break a declarative binding for good.
    function syncAccountCombo() {
        var i = root.accountIndexOf(root.myAddress);
        if (accountCombo.currentIndex !== i)
            accountCombo.currentIndex = i;
    }

    onAccountsJsonChanged: root.rebuildAccounts()
    onMyAddressChanged: root.syncAccountCombo()

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
            placeholder: false,
            delivery: ""
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
            placeholder: true,
            delivery: ""
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
        repliesModel.append({ rid: id, topicId: topicId, body: body, author: author || "", ts: root.formatTs(ts), delivery: "" });

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
            threadModel.append({ rid: id, body: body, author: author || "", ts: root.formatTs(ts), delivery: "" });
    }

    // Delivery state for one of our own posts, pushed by the backend. A post is
    // echoed locally the moment the send is accepted, which says nothing about
    // whether it reached anyone — this is what tells the two apart, and the only
    // thing that makes a node publishing into the void visible from the UI.
    function setMessageState(id, state, detail) {
        var i = root.findTopicIndex(id);
        if (i >= 0)
            topicsModel.setProperty(i, "delivery", state);
        for (var j = 0; j < repliesModel.count; ++j)
            if (repliesModel.get(j).rid === id) {
                repliesModel.setProperty(j, "delivery", state);
                break;
            }
        for (var k = 0; k < threadModel.count; ++k)
            if (threadModel.get(k).rid === id) {
                threadModel.setProperty(k, "delivery", state);
                break;
            }
        if (state === "failed")
            root.lastError = detail.length > 0 ? "Not delivered: " + detail
                                               : "Not delivered";
    }

    // Row suffix for a delivery state. "sent" is the expected outcome, so it
    // reads as an unmarked row rather than a badge on every post of your own.
    function deliveryMark(state) {
        if (state === "pending") return " · sending…";
        if (state === "propagated") return " · on the network";
        if (state === "failed") return " · ⚠ not delivered";
        return "";
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
                threadModel.append({ rid: r.rid, body: r.body, author: r.author, ts: r.ts, delivery: r.delivery });
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

    // ── Account actions ───────────────────────────────────────────────────────
    // Each mirrors the create/reply pattern: call the slot through logos.watch
    // and surface any error on the shared lastError line. The backend
    // re-publishes myAddress / myLabel / accountsJson on success, so nothing
    // here mutates local state — the PROP change handlers do it.

    function chooseAccount(index) {
        if (index < 0 || index >= accountsModel.count) return;
        var keyId = accountsModel.get(index).keyId;
        if (keyId === root.myAddress) return;
        root.lastError = "";
        root.log("selectAccount(" + keyId + ")");
        logos.watch(backend.selectAccount(keyId), function (err) {
            // Put the combo back where the backend actually is when the switch
            // didn't take — leaving it on the failed pick would misreport who
            // the next post is signed by.
            if (err) { root.lastError = err; root.syncAccountCombo(); }
        }, function (e) { root.lastError = e; root.syncAccountCombo(); });
    }

    function submitCreateAccount() {
        root.accountError = "";
        var label = newAccountField.text;
        root.log("createAccount(\"" + label + "\")");
        logos.watch(backend.createAccount(label), function (err) {
            if (err) { root.accountError = err; root.log("createAccount error -> " + err); }
            else { newAccountField.text = ""; createAccountDialog.close(); }
        }, function (e) { root.accountError = e; });
    }

    function submitRenameAccount() {
        if (renameAccountField.text.length === 0) return;
        root.accountError = "";
        var keyId = root.myAddress, label = renameAccountField.text;
        root.log("renameAccount(" + keyId + ")");
        logos.watch(backend.renameAccount(keyId, label), function (err) {
            if (err) { root.accountError = err; root.log("renameAccount error -> " + err); }
            else { renameAccountDialog.close(); }
        }, function (e) { root.accountError = e; });
    }

    function submitDeleteAccount() {
        root.accountError = "";
        var keyId = root.myAddress;
        root.log("deleteAccount(" + keyId + ")");
        logos.watch(backend.deleteAccount(keyId), function (err) {
            if (err) { root.accountError = err; root.log("deleteAccount error -> " + err); }
            else { deleteAccountDialog.close(); }
        }, function (e) { root.accountError = e; });
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

    // ── Account menu + dialogs ────────────────────────────────────────────────
    // All three act on the *selected* account — the one named in the combo they
    // hang off. Popups aren't Items, so declaring them here keeps them out of
    // the layouts above.

    LogosMenu {
        id: accountMenu
        LogosMenuItem {
            text: "Rename…"
            onTriggered: {
                renameAccountField.text = root.myLabel;
                renameAccountDialog.open();
            }
        }
        LogosMenuItem {
            text: "Delete…"
            // Deleting the last account would leave nothing to post as, and the
            // backend refuses it — don't offer it.
            enabled: accountsModel.count > 1
            onTriggered: deleteAccountDialog.open()
        }
    }

    LogosDialog {
        id: createAccountDialog
        onOpened: root.accountError = ""
        title: "New account"
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape

        ColumnLayout {
            spacing: Theme.spacing.small
            LogosText {
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                text: "Mints a new signing key and switches to it. Posts already on screen keep the account that signed them."
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }
            LogosTextField {
                id: newAccountField
                Layout.fillWidth: true
                placeholderText: "Name (optional)"
                Component.onCompleted: textInput.activeFocusOnTab = true
            }
            Connections {
                target: newAccountField.textInput
                function onAccepted() { root.submitCreateAccount() }
            }
            LogosText {
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                visible: root.accountError.length > 0
                text: "⚠ " + root.accountError
                color: Theme.palette.error
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }
        }

        rightActions: [
            FocusButton {
                text: "Cancel"
                implicitWidth: 88
                implicitHeight: 36
                onClicked: createAccountDialog.close()
            },
            FocusButton {
                text: "Create"
                implicitWidth: 88
                implicitHeight: 36
                onClicked: root.submitCreateAccount()
            }
        ]
    }

    LogosDialog {
        id: renameAccountDialog
        onOpened: root.accountError = ""
        title: "Rename account"
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape

        ColumnLayout {
            spacing: Theme.spacing.small
            LogosText {
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                text: "A display name only — the signing key is unchanged, so this doesn't affect anything already posted."
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }
            LogosTextField {
                id: renameAccountField
                Layout.fillWidth: true
                placeholderText: "Account name"
                Component.onCompleted: textInput.activeFocusOnTab = true
            }
            Connections {
                target: renameAccountField.textInput
                function onAccepted() { root.submitRenameAccount() }
            }
            LogosText {
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                visible: root.accountError.length > 0
                text: "⚠ " + root.accountError
                color: Theme.palette.error
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }
        }

        rightActions: [
            FocusButton {
                text: "Cancel"
                implicitWidth: 88
                implicitHeight: 36
                onClicked: renameAccountDialog.close()
            },
            FocusButton {
                text: "Rename"
                implicitWidth: 88
                implicitHeight: 36
                enabled: renameAccountField.text.length > 0
                onClicked: root.submitRenameAccount()
            }
        ]
    }

    LogosDialog {
        id: deleteAccountDialog
        onOpened: root.accountError = ""
        title: "Delete account?"
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape

        ColumnLayout {
            spacing: Theme.spacing.small
            LogosText {
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                text: "\u201C" + root.myLabel + "\u201D (" + root.shortAddress(root.myAddress) + ")"
                color: Theme.palette.text
                font.pixelSize: Theme.typography.primaryText
                font.family: root.monoFont
                wrapMode: Text.WordWrap
            }
            LogosText {
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                text: "Its signing key is destroyed permanently — this can't be undone, and nothing can be signed as this account again. Posts it already made stay on other people's screens."
                color: Theme.palette.warning
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }
            LogosText {
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                visible: root.accountError.length > 0
                text: "⚠ " + root.accountError
                color: Theme.palette.error
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }
        }

        rightActions: [
            FocusButton {
                text: "Cancel"
                implicitWidth: 88
                implicitHeight: 36
                onClicked: deleteAccountDialog.close()
            },
            FocusButton {
                text: "Delete"
                implicitWidth: 88
                implicitHeight: 36
                onClicked: root.submitDeleteAccount()
            }
        ]
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
            text: "Topics: " + (root.topic.length > 0 ? root.topic : "—")
            color: Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
            font.family: root.monoFont
        }
        LogosText {
            text: (root.nodeReady ? "● " : "○ ") + (root.status.length > 0 ? root.status : "Connecting to backend…")
            color: root.nodeReady ? Theme.palette.success : Theme.palette.warning
            font.pixelSize: Theme.typography.secondaryText
        }
        // Own signing identity. This install can hold several accounts — each a
        // keystore_signer key minted on demand (see loadAccounts() in the
        // backend) — and exactly one signs from here on. Switching leaves posts
        // already on screen alone: they keep the author they were signed with,
        // which is the honest thing to show.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.small

            LogosText {
                text: root.myAddress.length > 0 ? "Posting as" : "Preparing identity…"
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
            }
            LogosComboBox {
                id: accountCombo
                visible: root.myAddress.length > 0
                enabled: root.ready
                model: accountsModel
                textRole: "display"
                Layout.preferredWidth: 240
                activeFocusOnTab: true
                // onActivated (a user pick) only — currentIndex also moves when
                // syncAccountCombo() follows the backend, and reacting to that
                // would loop a switch back into the backend that made it.
                onActivated: function (index) { root.chooseAccount(index) }
            }
            FocusButton {
                visible: root.myAddress.length > 0
                text: "New"
                Layout.preferredWidth: 72
                Layout.preferredHeight: 32
                implicitWidth: 72
                implicitHeight: 32
                enabled: root.ready
                onClicked: {
                    newAccountField.text = "";
                    createAccountDialog.open();
                }
            }
            FocusButton {
                id: accountMenuButton
                visible: root.myAddress.length > 0
                text: "⋯"
                Layout.preferredWidth: 40
                Layout.preferredHeight: 32
                implicitWidth: 40
                implicitHeight: 32
                enabled: root.ready
                onClicked: accountMenu.popup(accountMenuButton, 0, accountMenuButton.height)
            }
            // Soak up the remaining width so the controls stay left-aligned.
            Item { Layout.fillWidth: true }
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
                                              + root.deliveryMark(model.delivery)
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
                                      + root.deliveryMark(model.delivery)
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
                        placeholderText: root.nodeReady ? "Write a reply…" : "Preparing local store…"
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

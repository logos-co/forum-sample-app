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

    // "status" property from the .rep file, auto-updated via QTRO.
    readonly property string status: backend ? backend.status : ""

    // Property-change callbacks — fire whenever the bound value updates.
    onReadyChanged: log("ready changed -> " + ready)
    onStatusChanged: log("backend status changed -> \"" + status + "\"")

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            root.log("onViewModuleReadyChanged(module=\"" + moduleName + "\", ready=" + isReady + ")");
            if (moduleName === "broadcast_app")
                root.ready = isReady && root.backend !== null;
        }
    }
    Component.onCompleted: {
        log("Component.onCompleted — view created");
        root.ready = root.backend !== null && logos.isViewModuleReady("broadcast_app");
    }
    Component.onDestruction: log("Component.onDestruction — view torn down")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Text {
            text: "Broadcast App (C++ backend)"
            font.pixelSize: 20
            color: "#ffffff"
            Layout.alignment: Qt.AlignHCenter
        }

        // Connection status
        Text {
            text: root.ready ? "Connected" : "Connecting to backend..."
            color: root.ready ? "#56d364" : "#f0883e"
            font.pixelSize: 12
        }

        RowLayout {
            spacing: 12
            Layout.fillWidth: true

            TextField {
                id: inputA
                placeholderText: "a"
                Layout.preferredWidth: 80
                validator: IntValidator {}
            }

            TextField {
                id: inputB
                placeholderText: "b"
                Layout.preferredWidth: 80
                validator: IntValidator {}
            }

            Button {
                text: "Add"
                enabled: root.ready
                onClicked: {
                    var a = parseInt(inputA.text) || 0;
                    var b = parseInt(inputB.text) || 0;
                    root.log("Add clicked — calling backend.add(" + a + ", " + b + ")");
                    // logos.watch() delivers the pending reply via callbacks
                    logos.watch(backend.add(a, b), function (value) {
                        root.log("add reply (success) -> " + value);
                        resultText.text = "Result: " + value;
                    }, function (error) {
                        root.log("add reply (error) -> " + error);
                        resultText.text = "Error: " + error;
                    });
                }
            }
        }

        // Shows the return value from the slot call
        Text {
            id: resultText
            text: "Press Add to call the backend"
            color: "#56d364"
            font.pixelSize: 15
        }

        // Shows the auto-synced "status" property from the backend
        Text {
            text: "Backend status: " + root.status
            color: "#8b949e"
            font.pixelSize: 13
        }

        // Pure-QML elapsed timer — no C++ backend involvement.
        // Ticks once per second from a Timer (only runs while the frontend is
        // active), incrementing the counter each tick.
        Text {
            id: elapsedText
            property int elapsed: 0
            text: "Elapsed since launch: " + elapsed + "s"
            color: "#8b949e"
            font.pixelSize: 13

            Timer {
                interval: 1000
                running: true
                repeat: true
                onTriggered: {
                    elapsedText.elapsed += 1;
                    root.log("QML Timer onTriggered — elapsed=" + elapsedText.elapsed + "s");
                }
            }
        }

        // Backend-driven timer — the C++ backend ticks once per second while
        // it's active (see onContextReady) and pushes the running count as the
        // backendElapsedSeconds PROP, so QML just renders the synced value.
        Text {
            id: backendElapsedText
            property int elapsed: root.backend ? root.backend.backendElapsedSeconds : 0
            text: "Elapsed since backend start: " + elapsed + "s"
            color: "#8b949e"
            font.pixelSize: 13

            // Callback fired each time the backend pushes a new PROP value.
            onElapsedChanged: root.log("backendElapsedSeconds synced from backend -> " + elapsed + "s")
        }

        Item {
            Layout.fillHeight: true
        }
    }
}

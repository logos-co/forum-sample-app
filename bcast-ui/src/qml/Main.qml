import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    // Typed replica — auto-synced properties and callable slots.
    readonly property var backend: logos.module("broadcast_app")
    property bool ready: false

    // "status" property from the .rep file, auto-updated via QTRO.
    readonly property string status: backend ? backend.status : ""

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "broadcast_app")
                root.ready = isReady && root.backend !== null;
        }
    }
    Component.onCompleted: {
        root.ready = root.backend !== null && logos.isViewModuleReady("broadcast_app");
    }

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
                    // logos.watch() delivers the pending reply via callbacks
                    logos.watch(backend.add(parseInt(inputA.text) || 0, parseInt(inputB.text) || 0), function (value) {
                        resultText.text = "Result: " + value;
                    }, function (error) {
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
        // Driven by the render loop (one tick per frame) and derived from
        // wall-clock time, so it never drifts or accumulates dropped ticks.
        Text {
            id: elapsedText
            property double startMs: Date.now()
            property int elapsed: 0
            text: "Elapsed since launch: " + elapsed + "ms"
            color: "#8b949e"
            font.pixelSize: 13

            FrameAnimation {
                running: true
                onTriggered: elapsedText.elapsed = Math.round(Date.now() - elapsedText.startMs)
            }
        }

        // Backend-driven timer — start instant comes from the C++ backend's
        // onContextReady() (no Qt time types there; std::chrono only), synced
        // once as the backendStartedAtMs PROP. QML does the per-frame counting,
        // so there's no IPC per tick. Reads 0 until the PROP syncs.
        Text {
            id: backendElapsedText
            property double startMs: root.backend ? root.backend.backendStartedAtMs : 0
            property int elapsed: 0
            text: startMs > 0 ? "Elapsed since backend start: " + elapsed + "ms"
                              : "Waiting for backend start..."
            color: "#8b949e"
            font.pixelSize: 13

            FrameAnimation {
                running: backendElapsedText.startMs > 0
                onTriggered: backendElapsedText.elapsed = Math.round(Date.now() - backendElapsedText.startMs)
            }
        }

        Item {
            Layout.fillHeight: true
        }
    }
}

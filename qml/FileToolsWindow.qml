import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: toolsWindow
    objectName: "fileToolsWindow"
    property var controller: typeof fileTools !== "undefined" ? fileTools : null
    readonly property bool working: controller ? controller.busy : false
    readonly property int preferredWidth: 440
    readonly property int preferredHeight: options ? options.preferredStandaloneHeight : 430
    width: Math.min(preferredWidth, Screen.desktopAvailableWidth || preferredWidth)
    height: Math.min(preferredHeight, Screen.desktopAvailableHeight || preferredHeight)
    minimumWidth: Math.min(380, Screen.desktopAvailableWidth || 380)
    minimumHeight: Math.min(340, Screen.desktopAvailableHeight || 340)
    visible: false
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal
    color: "transparent"
    title: (controller && controller.action === "metadata" ? "Metadados" : "Comprimir") + " · Maracujá"
    font.family: "Segoe UI"
    font.pixelSize: 14

    function fitToScreen() {
        width = Math.min(preferredWidth, Screen.desktopAvailableWidth || preferredWidth)
        height = Math.min(preferredHeight, Screen.desktopAvailableHeight || preferredHeight)
    }
    onPreferredHeightChanged: if (visible) fitToScreen()
    function openForTool() {
        // Reuse this independent window; never change the main converter's
        // visibility, compact mode or selection when a drag opens a tool.
        fitToScreen()
        if (!visible) show()
        options.open()
        raise()
        requestActivate()
    }
    onClosing: function(close) {
        if (working) close.accepted = false
        else options.close()
    }
    Connections {
        target: toolsWindow.controller
        function onPanelRequested() { toolsWindow.openForTool() }
    }
    // A small transparent margin lets the card keep its rounded silhouette
    // without dimming the user's desktop or creating a full application view.
    Rectangle {
        anchors.fill: parent
        anchors.margins: 9
        anchors.topMargin: 14
        radius: 27
        color: "#2639224d"
    }
    FileToolsPanel {
        id: options
        controller: toolsWindow.controller
        standalone: true
        autoPresent: false
        onClosed: toolsWindow.hide()
        onMoveRequested: {
            if (typeof fileToolsWindowMover !== "undefined") fileToolsWindowMover.beginMove()
        }
    }
}

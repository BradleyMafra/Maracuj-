import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolTip {
    id: tip
    property string heading: ""
    property real maximumWidth: 320
    readonly property var ownerWindow: parent ? parent.Window.window : null
    readonly property real windowLimit: ownerWindow ? Math.max(1, ownerWindow.width - 20) : maximumWidth
    implicitWidth: Math.min(maximumWidth, Math.max(body.implicitWidth, caption.implicitWidth) + leftPadding + rightPadding)
    implicitHeight: contents.implicitHeight + topPadding + bottomPadding
    width: Math.min(implicitWidth, windowLimit)
    x: parent ? (parent.width - width) / 2 : 0
    // ToolTip's popup positioner flips below the anchor when there is no
    // space above. A local offset also follows controls after layout changes.
    y: -height - 8
    margins: 10
    leftPadding: 12; rightPadding: 12
    topPadding: 10; bottomPadding: 10
    delay: 500
    timeout: 9000
    modal: false
    dim: false
    focus: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    font.family: "Segoe UI"
    font.pixelSize: 12
    contentItem: ColumnLayout {
        id: contents
        spacing: 4
        Text {
            id: caption
            visible: tip.heading.length > 0
            Layout.fillWidth: true
            text: tip.heading; textFormat: Text.PlainText
            color: "#63358b"; font.family: tip.font.family
            font.pixelSize: 12; font.weight: Font.DemiBold
            wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight
        }
        Text {
            id: body
            Layout.fillWidth: true
            text: tip.text; textFormat: Text.PlainText
            color: "#5b4a66"; font: tip.font
            wrapMode: Text.Wrap; lineHeight: 1.18
            maximumLineCount: 10; elide: Text.ElideRight
        }
    }
    background: Rectangle {
        radius: 12; color: "#fffdfa"
        border.width: 1; border.color: "#e2d5ed"
        Rectangle {
            z: -1; x: -1; y: 2
            width: parent.width + 2; height: parent.height + 1
            radius: 13; color: "#12412c53"
        }
    }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120; easing.type: Easing.OutCubic } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 80 } }
}

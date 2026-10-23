import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: editor
    objectName: "photoEditorWindow"
    property var controller: typeof photoEditor !== "undefined" ? photoEditor : null
    readonly property int preferredWidth: 620
    readonly property int preferredHeight: 730
    readonly property bool working: controller ? controller.busy : false
    readonly property bool ready: controller ? controller.ready : false
    readonly property bool rendering: controller ? controller.rendering : false
    readonly property bool cancellable: controller && controller.cancellable !== undefined ? controller.cancellable : working
    readonly property bool hasAdjustments: {
        var values = controller ? controller.adjustments : ({})
        return Object.keys(values).some(function(key) { return Math.abs(Number(values[key])) > 0.0001 })
    }
    readonly property color ink: "#412c53"
    readonly property color muted: "#8d7d98"
    readonly property color violet: "#63358b"
    property string selectedGroup: "light"
    property bool closingAccepted: false
    readonly property var groups: [
        { id: "light", label: "Luz", icon: "☀" },
        { id: "color", label: "Cor", icon: "◐" },
        { id: "detail", label: "Detalhes", icon: "✧" }
    ]

    width: Math.min(preferredWidth, Screen.desktopAvailableWidth || preferredWidth)
    height: Math.min(preferredHeight, Screen.desktopAvailableHeight || preferredHeight)
    minimumWidth: Math.min(470, Screen.desktopAvailableWidth || 470)
    minimumHeight: Math.min(540, Screen.desktopAvailableHeight || 540)
    visible: false
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal
    color: "transparent"
    title: "Editar foto · Maracujá"
    font.family: "Segoe UI"
    font.pixelSize: 13

    function fitToScreen() {
        width = Math.min(preferredWidth, Screen.desktopAvailableWidth || preferredWidth)
        height = Math.min(preferredHeight, Screen.desktopAvailableHeight || preferredHeight)
    }
    function openForPhoto() {
        closingAccepted = false
        selectedGroup = "light"
        fitToScreen()
        if (!visible) show()
        raise()
        requestActivate()
    }
    function finishClosing() {
        if (working) return
        closingAccepted = true
        if (controller) controller.closeEditor()
        hide()
        closingAccepted = false
    }
    function requestClose() {
        if (working) return
        if (controller && controller.dirty) discardDialog.open()
        else finishClosing()
    }
    function controlsForGroup() {
        var all = controller ? controller.controls : []
        return all.filter(function(control) {
            var group = String(control.group || "").toLocaleLowerCase()
            if (group === "luz") group = "light"
            else if (group === "cor") group = "color"
            else if (group === "detalhes" || group === "details") group = "detail"
            return group === editor.selectedGroup
        })
    }
    function adjustmentValue(control) {
        var values = controller ? controller.adjustments : ({})
        return values[control.id] === undefined ? 0 : Number(values[control.id])
    }
    function valueText(control, value) {
        var precision = Number(control.step) < 1 ? (Number(control.step) < 0.1 ? 2 : 1) : 0
        var number = Number(value).toFixed(precision)
        if (Number(value) > 0) number = "+" + number
        return number + (control.unit ? " " + control.unit : "")
    }
    onClosing: function(event) {
        event.accepted = closingAccepted
        if (!closingAccepted) requestClose()
    }
    Connections {
        target: editor.controller
        function onPanelRequested() { editor.openForPhoto() }
    }
    Shortcut {
        sequence: "Escape"
        enabled: editor.visible && !discardDialog.opened && !formatCombo.popup.visible
        onActivated: editor.requestClose()
    }

    component SoftButton: Button {
        id: button
        property bool primary: false
        property bool quiet: false
        implicitHeight: 34
        leftPadding: 13; rightPadding: 13
        font.pixelSize: 12; font.weight: Font.DemiBold
        contentItem: Text {
            textFormat: Text.PlainText; text: button.text; font: button.font
            color: !button.enabled ? "#b0a5b9" : button.primary ? "#ffffff" : editor.ink
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 10
            color: !button.enabled ? "#ece6f0" : button.primary
                ? (button.down ? "#512773" : button.hovered ? "#7d49a8" : editor.violet)
                : button.down ? "#e9ddf3" : button.hovered ? "#f1e9f8" : button.quiet ? "transparent" : "#fffdfa"
            border.width: button.primary || button.quiet ? 0 : 1
            border.color: button.activeFocus ? editor.violet : "#e6daee"
        }
    }
    component AdjustmentRow: RowLayout {
        id: row
        required property var control
        readonly property double currentValue: editor.adjustmentValue(control)
        spacing: 8
        implicitHeight: 38
        Text {
            id: controlLabel
            Layout.preferredWidth: editor.width < 540 ? 100 : 115
            Layout.minimumWidth: 75
            textFormat: Text.PlainText; text: row.control.label
            color: editor.ink; font.pixelSize: 12; elide: Text.ElideRight
            CleanToolTip {
                visible: labelHover.containsMouse
                text: controlLabel.text
            }
            MouseArea { id: labelHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
        }
        Slider {
            id: adjustment
            objectName: "photoAdjustment_" + row.control.id
            Layout.fillWidth: true; Layout.minimumWidth: 65
            implicitHeight: 30
            from: Number(row.control.min); to: Number(row.control.max)
            stepSize: Number(row.control.step) || 1
            value: row.currentValue
            enabled: editor.ready && !editor.working
            focusPolicy: Qt.StrongFocus
            leftPadding: 9; rightPadding: 9
            onMoved: if (editor.controller) editor.controller.setAdjustment(row.control.id, value)
            Accessible.name: row.control.label
            Accessible.description: editor.valueText(row.control, value)
            background: Rectangle {
                x: adjustment.leftPadding
                y: adjustment.topPadding + adjustment.availableHeight / 2 - height / 2
                width: adjustment.availableWidth; height: 4; radius: 2; color: "#e8e0ef"
                readonly property double origin: Math.max(0, Math.min(1, (0 - adjustment.from) / (adjustment.to - adjustment.from)))
                Rectangle {
                    x: Math.min(parent.origin, adjustment.visualPosition) * parent.width
                    width: Math.abs(adjustment.visualPosition - parent.origin) * parent.width
                    height: parent.height; radius: 2; color: "#a17abe"
                }
                Rectangle {
                    visible: adjustment.from < 0
                    x: parent.origin * parent.width - 0.5
                    y: -2; width: 1; height: 8; color: "#d2c3de"
                }
            }
            handle: Rectangle {
                x: adjustment.leftPadding + adjustment.visualPosition * adjustment.availableWidth - width / 2
                y: adjustment.topPadding + adjustment.availableHeight / 2 - height / 2
                implicitWidth: 16; implicitHeight: 16; radius: 8
                color: adjustment.pressed ? "#f1e4fb" : "#fffdfa"
                border.width: adjustment.activeFocus || adjustment.hovered ? 2 : 1
                border.color: adjustment.activeFocus || adjustment.hovered ? "#9664b8" : "#c7b0d7"
            }
        }
        Text {
            Layout.preferredWidth: 58
            textFormat: Text.PlainText; text: editor.valueText(row.control, row.currentValue)
            color: Math.abs(row.currentValue) > 0.0001 ? editor.violet : editor.muted
            font.pixelSize: 11; font.weight: Font.Medium
            horizontalAlignment: Text.AlignRight
        }
        SoftButton {
            id: resetAdjustmentButton
            objectName: "photoReset_" + row.control.id
            implicitWidth: 25; implicitHeight: 28; leftPadding: 0; rightPadding: 0
            text: "↺"; font.pixelSize: 17; quiet: true
            enabled: editor.ready && !editor.working && Math.abs(row.currentValue) > 0.0001
            onClicked: if (editor.controller) editor.controller.resetAdjustment(row.control.id)
            CleanToolTip {
                visible: resetAdjustmentButton.hovered
                text: "Redefinir " + row.control.label.toLocaleLowerCase()
            }
            Accessible.name: "Redefinir " + row.control.label
        }
    }

    Rectangle {
        anchors.fill: parent; anchors.margins: 10; anchors.topMargin: 15
        radius: 25; color: "#3039224d"
    }
    Rectangle {
        id: card
        anchors.fill: parent; anchors.margins: 6; anchors.bottomMargin: 12
        radius: 22; color: "#fffdf8"; border.color: "#dfd0e9"
        ColumnLayout {
            anchors.fill: parent; anchors.margins: editor.width < 540 ? 16 : 20
            spacing: 12
            Item {
                Layout.fillWidth: true; implicitHeight: 39
                Rectangle {
                    id: headerIcon
                    width: 36; height: 36; radius: 11; color: "#f1e7fa"
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                    Text { anchors.centerIn: parent; textFormat: Text.PlainText; text: "◐"; color: editor.violet; font.pixelSize: 24 }
                }
                Column {
                    anchors.left: headerIcon.right; anchors.leftMargin: 10
                    anchors.right: closeButton.left; anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter; spacing: 3
                    Text { textFormat: Text.PlainText; text: "Editar foto"; color: editor.ink; font.pixelSize: 19; font.weight: Font.DemiBold }
                    Text {
                        width: parent.width; textFormat: Text.PlainText
                        text: editor.controller && editor.controller.fileName ? editor.controller.fileName : "Uma nova perspectiva para sua foto"
                        color: editor.muted; font.pixelSize: 11; elide: Text.ElideMiddle
                    }
                }
                MouseArea {
                    anchors.left: parent.left; anchors.right: closeButton.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                    onPressed: if (typeof photoEditorWindowMover !== "undefined") photoEditorWindowMover.beginMove()
                }
                SoftButton {
                    id: closeButton; objectName: "photoClose"
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 30; implicitHeight: 30; leftPadding: 0; rightPadding: 0
                    text: "×"; quiet: true; font.pixelSize: 21
                    enabled: !editor.working; onClicked: editor.requestClose()
                    Accessible.name: "Fechar editor de foto"
                }
            }
            Rectangle {
                id: viewport
                objectName: "photoViewport"
                Layout.fillWidth: true
                Layout.preferredHeight: editor.height < 650 ? 175 : 250
                Layout.minimumHeight: 140
                radius: 13; color: "#211c29"; clip: true
                Image {
                    id: photoPreview; objectName: "photoPreview"
                    anchors.fill: parent; anchors.margins: 9; anchors.bottomMargin: 35
                    source: editor.controller ? (compareButton.down ? editor.controller.originalSource : editor.controller.previewSource) : ""
                    sourceSize.width: 1280; sourceSize.height: 1280
                    asynchronous: true; cache: false; fillMode: Image.PreserveAspectFit
                    autoTransform: true
                }
                Text {
                    anchors.centerIn: parent; visible: !editor.ready && !editor.working
                    textFormat: Text.PlainText; text: "Escolha uma foto para começar"
                    color: "#d2c4df"; font.pixelSize: 13
                }
                RowLayout {
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    anchors.leftMargin: 12; anchors.rightMargin: 9; anchors.bottomMargin: 7
                    spacing: 7
                    Rectangle {
                        implicitWidth: (compareButton.down ? originalLabel.implicitWidth : adjustedLabel.implicitWidth) + 15
                        implicitHeight: 21; radius: 6; color: "#3d334b"
                        Text { id: originalLabel; visible: compareButton.down; anchors.centerIn: parent; textFormat: Text.PlainText; text: "ORIGINAL"; color: "#e5d8ed"; font.pixelSize: 9; font.letterSpacing: 1 }
                        Text { id: adjustedLabel; visible: !compareButton.down; anchors.centerIn: parent; textFormat: Text.PlainText; text: "PRÉVIA"; color: "#e5d8ed"; font.pixelSize: 9; font.letterSpacing: 1 }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        visible: editor.rendering && !editor.working
                        textFormat: Text.PlainText; text: "Atualizando…"; color: "#c4aad8"; font.pixelSize: 10
                    }
                    Button {
                        id: compareButton; objectName: "photoCompare"
                        property bool tooltipDismissed: false
                        text: "Mostrar Original"; implicitHeight: 24; leftPadding: 9; rightPadding: 9
                        enabled: editor.ready && !editor.working
                        onPressed: tooltipDismissed = true
                        onHoveredChanged: if (!hovered) tooltipDismissed = false
                        contentItem: Text { textFormat: Text.PlainText; text: compareButton.text; color: "#f2e9f9"; font.pixelSize: 10; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { radius: 6; color: compareButton.down ? "#795493" : compareButton.hovered ? "#4b3d5b" : "#382f43" }
                        CleanToolTip {
                            visible: compareButton.hovered && compareButton.enabled && !compareButton.down && !compareButton.tooltipDismissed
                            text: "Segure para comparar com o arquivo original"
                        }
                        Accessible.name: compareButton.text
                        Accessible.description: "Mantenha pressionado para comparar com o arquivo original"
                    }
                }
                Rectangle {
                    anchors.fill: parent; radius: 13; visible: editor.working
                    color: "#aa211c29"
                    Column {
                        anchors.centerIn: parent; spacing: 7
                        BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: editor.working; width: 32; height: 32; palette.dark: "#d6b9ef" }
                        Text { textFormat: Text.PlainText; text: editor.ready ? "Aguarde um instante…" : "Preparando foto…"; color: "#f2e9f9"; font.pixelSize: 12 }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: 35; radius: 10; color: "#f3edf7"
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 3; spacing: 3
                        Repeater {
                            model: editor.groups
                            Button {
                                id: groupButton
                                required property var modelData
                                objectName: "photoGroup_" + modelData.id
                                Layout.fillWidth: true; Layout.preferredWidth: 1
                                implicitHeight: 29
                                enabled: !editor.working
                                onClicked: { editor.selectedGroup = modelData.id; controlsScroll.contentItem.contentY = 0 }
                                contentItem: Text {
                                    textFormat: Text.PlainText; text: groupButton.modelData.icon + "  " + groupButton.modelData.label
                                    font.pixelSize: 11; font.weight: Font.DemiBold
                                    color: editor.selectedGroup === groupButton.modelData.id ? editor.violet : editor.muted
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: 7
                                    color: editor.selectedGroup === groupButton.modelData.id ? "#fffdfa" : groupButton.hovered ? "#eae0f2" : "transparent"
                                    border.color: editor.selectedGroup === groupButton.modelData.id ? "#e6d9ed" : "transparent"
                                }
                                Accessible.checkable: true; Accessible.checked: editor.selectedGroup === modelData.id
                                Accessible.name: modelData.label
                            }
                        }
                    }
                }
                SoftButton {
                    id: resetAllButton
                    objectName: "photoResetAll"; text: "Redefinir"; quiet: true
                    enabled: editor.ready && !editor.working && editor.hasAdjustments
                    onClicked: if (editor.controller) editor.controller.resetAll()
                    CleanToolTip {
                        visible: resetAllButton.hovered
                        text: "Voltar todos os ajustes ao original"
                    }
                }
            }
            ScrollView {
                id: controlsScroll
                objectName: "photoControlsScroll"
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.preferredHeight: 230; Layout.minimumHeight: 105
                contentWidth: availableWidth; clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: controlsScroll.availableWidth; spacing: 0
                    Repeater {
                        model: editor.controlsForGroup()
                        AdjustmentRow { required property var modelData; control: modelData; Layout.fillWidth: true }
                    }
                    Item { implicitHeight: 3 }
                }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#eee4f1" }
            RowLayout {
                Layout.fillWidth: true; spacing: 9
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    Text { textFormat: Text.PlainText; text: "SALVAR UMA CÓPIA"; color: editor.muted; font.pixelSize: 9; font.weight: Font.DemiBold; font.letterSpacing: 0.8 }
                    Text { textFormat: Text.PlainText; text: "Seu original fica intacto"; color: editor.ink; font.pixelSize: 11 }
                }
                ComboBox {
                    id: formatCombo; objectName: "photoExportFormat"
                    implicitWidth: 86; implicitHeight: 32
                    visible: !editor.working
                    enabled: editor.ready && !editor.working
                    model: editor.controller ? editor.controller.exportFormats : []
                    currentIndex: editor.controller ? Math.max(0, model.indexOf(editor.controller.exportFormat)) : -1
                    displayText: currentText.toLocaleUpperCase()
                    onActivated: if (editor.controller) editor.controller.exportFormat = currentText
                    leftPadding: 11; rightPadding: 24
                    contentItem: Text { textFormat: Text.PlainText; text: formatCombo.displayText; color: editor.ink; font.pixelSize: 11; font.weight: Font.DemiBold; verticalAlignment: Text.AlignVCenter }
                    indicator: Text { textFormat: Text.PlainText; text: "⌄"; color: editor.muted; font.pixelSize: 16; x: parent.width - width - 9; y: (parent.height - height) / 2 - 2 }
                    background: Rectangle { radius: 9; color: "#f5eff9"; border.color: formatCombo.activeFocus ? editor.violet : "#e8ddee" }
                    delegate: ItemDelegate {
                        id: formatItem
                        required property var modelData
                        required property int index
                        width: formatCombo.width; implicitHeight: 32
                        text: String(modelData).toLocaleUpperCase()
                        highlighted: formatCombo.highlightedIndex === index
                        contentItem: Text { textFormat: Text.PlainText; text: formatItem.text; color: editor.ink; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 7; color: formatItem.highlighted ? "#eadcf5" : "transparent" }
                    }
                    popup: Popup {
                        y: -height - 5; width: formatCombo.width; padding: 5
                        implicitHeight: formatList.contentHeight + padding * 2
                        contentItem: ListView { id: formatList; implicitHeight: contentHeight; model: formatCombo.popup.visible ? formatCombo.delegateModel : null; currentIndex: formatCombo.highlightedIndex; clip: true }
                        background: Rectangle { radius: 11; color: "#fffdfa"; border.color: "#e4d5ec" }
                    }
                    Accessible.name: "Formato da cópia editada"
                }
                SoftButton {
                    objectName: "photoCancel"; text: "Cancelar"
                    visible: editor.working && editor.cancellable; implicitWidth: 86
                    onClicked: if (editor.controller) editor.controller.cancel()
                }
                SoftButton {
                    objectName: "photoSaveCopy"; text: "Salvar cópia"; primary: true
                    enabled: editor.ready && !editor.working && !editor.rendering
                    onClicked: if (editor.controller) editor.controller.saveCopy()
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                Text {
                    objectName: "photoStatus"
                    Layout.fillWidth: true; textFormat: Text.PlainText
                    text: editor.controller && editor.controller.error ? editor.controller.error
                        : editor.controller && editor.controller.status ? editor.controller.status : "Ajustes locais · sem enviar sua foto à internet"
                    color: editor.controller && editor.controller.error ? "#b04563" : editor.muted
                    font.pixelSize: 10; wrapMode: Text.WordWrap
                    maximumLineCount: 2; elide: Text.ElideRight
                    CleanToolTip {
                        visible: statusHover.containsMouse && parent.truncated
                        text: parent.text
                    }
                    MouseArea { id: statusHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                }
                SoftButton {
                    objectName: "photoOpenOutput"; text: "Abrir pasta"; quiet: true
                    visible: editor.controller && editor.controller.outputPath.length > 0
                    enabled: !editor.working; implicitHeight: 25; font.pixelSize: 10
                    onClicked: if (editor.controller) editor.controller.openOutputFolder()
                }
            }
        }
    }
    Dialog {
        id: discardDialog; objectName: "photoDiscardDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(parent ? parent.width - 54 : 370, 370)
        modal: true; focus: true; padding: 22
        closePolicy: Popup.CloseOnEscape
        background: Rectangle { radius: 17; color: "#fffdf8"; border.color: "#ddcce9" }
        Overlay.modal: Rectangle { color: "#7730243c" }
        contentItem: ColumnLayout {
            spacing: 10
            Text { textFormat: Text.PlainText; text: "Fechar sem salvar?"; color: editor.ink; font.pixelSize: 18; font.weight: Font.DemiBold }
            Text { Layout.fillWidth: true; textFormat: Text.PlainText; text: "Seus ajustes ainda não foram salvos em uma cópia. O arquivo original continua intacto."; color: editor.muted; font.pixelSize: 12; wrapMode: Text.WordWrap }
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 5
                Item { Layout.fillWidth: true }
                SoftButton { objectName: "photoKeepEditing"; text: "Continuar"; onClicked: discardDialog.close() }
                SoftButton { objectName: "photoDiscardChanges"; text: "Descartar"; primary: true; onClicked: { discardDialog.close(); editor.finishClosing() } }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: appearance
    objectName: "wheelAppearanceDialog"
    property var desktopBridge: null
    property int draftTransparency: 35
    property int draftSize: 100
    property string draftColor: "#b99cde"
    property bool draftSoundEnabled: true
    property string saveError: ""
    readonly property bool validColor: /^#[0-9a-f]{6}$/.test(draftColor)
    readonly property color previewColor: validColor ? draftColor : "#b99cde"
    readonly property color ink: "#412c53"
    readonly property color muted: "#897792"
    readonly property color violet: "#63358b"
    anchors.centerIn: parent
    width: Math.min(parent.width - 32, 434)
    height: Math.min(implicitHeight, parent.height - 32)
    padding: 22
    bottomPadding: 0
    spacing: 12
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    onOpened: {
        draftTransparency = desktopBridge && typeof desktopBridge.wheelTransparency === "number" ? desktopBridge.wheelTransparency : 35;
        draftSize = desktopBridge && typeof desktopBridge.wheelSize === "number" ? desktopBridge.wheelSize : 100;
        draftColor = desktopBridge && typeof desktopBridge.wheelColor === "string" ? desktopBridge.wheelColor.toLowerCase() : "#b99cde";
        draftSoundEnabled = desktopBridge && typeof desktopBridge.wheelSoundEnabled === "boolean" ? desktopBridge.wheelSoundEnabled : true;
        saveError = "";
        transparencySlider.forceActiveFocus();
    }
    onDraftTransparencyChanged: {
        saveError = "";
        if (wheelPreview)
            wheelPreview.requestPaint();
    }
    onDraftSizeChanged: {
        saveError = "";
        if (wheelPreview)
            wheelPreview.requestPaint();
    }
    onDraftColorChanged: {
        saveError = "";
        if (wheelPreview)
            wheelPreview.requestPaint();
    }
    onDraftSoundEnabledChanged: saveError = ""
    function tintedRgb(red, green, blue) {
        var color = previewColor;
        var amount = Math.min(1, (Math.max(red, green, blue) - Math.min(red, green, blue)) / (66 / 255));
        return [Math.max(0, Math.min(1, red + amount * (color.r - 185 / 255))),
                Math.max(0, Math.min(1, green + amount * (color.g - 156 / 255))),
                Math.max(0, Math.min(1, blue + amount * (color.b - 222 / 255)))];
    }
    function glassColor(red, green, blue, alpha) {
        var rgb = tintedRgb(red, green, blue);
        return "rgba(" + Math.round(rgb[0] * 255) + "," + Math.round(rgb[1] * 255) + ","
                + Math.round(rgb[2] * 255) + "," + alpha + ")";
    }
    ColorDialog {
        id: colorPicker
        objectName: "wheelColorPicker"
        title: "Cor da roda"
        onAccepted: {
            var hex = function (channel) { return ("0" + Math.round(channel * 255).toString(16)).slice(-2); };
            appearance.draftColor = "#" + hex(selectedColor.r) + hex(selectedColor.g) + hex(selectedColor.b);
        }
    }
    background: Rectangle {
        radius: 22
        color: "#fffdfb"
        border.color: "#ddcce9"
    }
    Overlay.modal: Rectangle {
        color: "#69412c53"
    }

    component AppearanceButton: Button {
        id: action
        property bool primary: false
        implicitHeight: 36
        leftPadding: 14
        rightPadding: 14
        font.pixelSize: 12
        font.weight: Font.DemiBold
        contentItem: Text {
            text: action.text
            font: action.font
            color: !action.enabled ? "#a799ae" : action.primary ? "#fffdfb" : "#412c53"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 11
            color: !action.enabled ? "#eee7f2" : action.primary ? (action.down ? "#512773" : action.hovered ? "#8050a3" : "#63358b") : action.down ? "#eaddf4" : action.hovered ? "#f4ecfa" : "#fffdfb"
            border.width: action.visualFocus ? 2 : action.primary ? 0 : 1
            border.color: action.visualFocus ? "#a47bc2" : "#e7ddeb"
        }
    }

    contentItem: ColumnLayout {
        spacing: 12
        clip: true
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5
            Text {
                text: "Aparência da roda"
                color: appearance.violet
                font.pixelSize: 22
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: "Sua cor, seu tamanho e a leveza que você preferir."
                color: appearance.muted
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 168
            Layout.minimumHeight: 168
            Layout.maximumHeight: 168
            radius: 16
            clip: true
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0
                    color: "#bcc9f4"
                }
                GradientStop {
                    position: 0.5
                    color: "#d4c4e9"
                }
                GradientStop {
                    position: 1
                    color: "#efd0b5"
                }
            }
            Canvas {
                id: wheelPreview
                objectName: "wheelTransparencyPreview"
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.clearRect(0, 0, width, height);
                    ctx.save();
                    ctx.beginPath();
                    ctx.moveTo(16, 0);
                    ctx.lineTo(width - 16, 0);
                    ctx.quadraticCurveTo(width, 0, width, 16);
                    ctx.lineTo(width, height - 16);
                    ctx.quadraticCurveTo(width, height, width - 16, height);
                    ctx.lineTo(16, height);
                    ctx.quadraticCurveTo(0, height, 0, height - 16);
                    ctx.lineTo(0, 16);
                    ctx.quadraticCurveTo(0, 0, 16, 0);
                    ctx.closePath();
                    ctx.clip();
                    ctx.fillStyle = "#a3c6cf";
                    ctx.beginPath();
                    ctx.arc(52, height + 28, 122, 0, Math.PI * 2);
                    ctx.fill();
                    ctx.fillStyle = "#c697bc";
                    ctx.beginPath();
                    ctx.arc(width - 31, -18, 117, 0, Math.PI * 2);
                    ctx.fill();
                    ctx.fillStyle = "#e5bc98";
                    ctx.beginPath();
                    ctx.arc(width - 14, height + 37, 99, 0, Math.PI * 2);
                    ctx.fill();
                    ctx.restore();
                    var scale = appearance.draftSize / 100;
                    var cx = width / 2, cy = height / 2, outer = 58 * scale, inner = 24 * scale;
                    var alpha = 1 - appearance.draftTransparency / 100;
                    var labels = ["PNG", "JPG", "PDF", "TXT", "WEBP", "DOCX"];
                    for (var i = 0; i < labels.length; ++i) {
                        var start = -Math.PI * 2 / 3 + i * Math.PI / 3 + 0.025;
                        var end = start + Math.PI / 3 - 0.05;
                        ctx.beginPath();
                        ctx.arc(cx, cy, outer, start, end);
                        ctx.arc(cx, cy, inner, end, start, true);
                        ctx.closePath();
                        var glass = ctx.createLinearGradient(0, cy - outer, 0, cy + outer);
                        glass.addColorStop(0, appearance.glassColor(1, 0.994, 0.98, alpha));
                        glass.addColorStop(1, appearance.glassColor(0.94, 0.90, 0.99, alpha));
                        ctx.fillStyle = glass;
                        ctx.fill();
                        ctx.strokeStyle = "rgba(255,255,255,0.65)";
                        ctx.lineWidth = 0.8;
                        ctx.stroke();
                        var angle = (start + end) / 2;
                        ctx.font = "600 " + Math.max(7, 8.5 * scale) + "px 'Segoe UI'";
                        ctx.fillStyle = "#42304f";
                        ctx.textAlign = "center";
                        ctx.textBaseline = "middle";
                        var sample = appearance.tintedRgb(0.94, 0.90, 0.99);
                        var halo = Math.max(Math.min(sample[0], sample[1], sample[2]) < 0.8 ? 0.9 : 0,
                                            0.95 * Math.max(0, Math.min(1, (appearance.draftTransparency - 35) / 30)));
                        if (halo > 0) {
                            ctx.strokeStyle = "rgba(255,255,255," + halo + ")";
                            ctx.lineWidth = 2;
                            ctx.strokeText(labels[i], cx + Math.cos(angle) * 41 * scale, cy + Math.sin(angle) * 41 * scale);
                        }
                        ctx.fillText(labels[i], cx + Math.cos(angle) * 41 * scale, cy + Math.sin(angle) * 41 * scale);
                    }
                    ctx.beginPath();
                    ctx.arc(cx, cy, inner - 3 * scale, 0, Math.PI * 2);
                    ctx.fillStyle = appearance.glassColor(0.974, 0.92, 0.998, alpha);
                    ctx.fill();
                    ctx.strokeStyle = "rgba(255,255,255,0.75)";
                    ctx.stroke();
                }
            }
            Image {
                anchors.centerIn: parent
                width: 24 * appearance.draftSize / 100
                height: width
                source: "qrc:/mandarina/assets/org.winux.Mandarina.svg"
                sourceSize.width: 56
                sourceSize.height: 56
            }
            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 12
                text: "PRÉVIA"
                color: "#604d70"
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 1
            }
        }
        ScrollView {
            id: appearanceScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: appearanceColumn.implicitHeight
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: appearanceColumn
                width: appearanceScroll.availableWidth
                spacing: 14
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: soundRow.implicitHeight + 24
                    radius: 13
                    color: "#f7f0fc"
                    RowLayout {
                        id: soundRow
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Text {
                                text: "Som da roda"
                                color: appearance.ink
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "Um toque suave ao passar pelas opções."
                                color: appearance.muted
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }
                        }
                        Switch {
                            id: soundSwitch
                            objectName: "wheelSoundToggle"
                            implicitWidth: 42
                            implicitHeight: 28
                            padding: 0
                            checked: appearance.draftSoundEnabled
                            Accessible.name: "Som da roda"
                            Accessible.description: "Ativar ou desativar o som ao passar pelas opções. A preferência só é aplicada ao salvar."
                            onToggled: appearance.draftSoundEnabled = checked
                            indicator: Rectangle {
                                x: (soundSwitch.width - width) / 2
                                y: (soundSwitch.height - height) / 2
                                width: 40
                                height: 24
                                radius: 12
                                color: soundSwitch.checked ? "#a47bc2" : "#ddd1e5"
                                border.width: soundSwitch.visualFocus ? 2 : 1
                                border.color: soundSwitch.visualFocus ? appearance.violet : soundSwitch.checked ? "#a47bc2" : "#d2c3de"
                                Behavior on color { ColorAnimation { duration: 120 } }
                                Rectangle {
                                    x: soundSwitch.checked ? parent.width - width - 3 : 3
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 18
                                    height: 18
                                    radius: 9
                                    color: soundSwitch.down ? "#f4eafa" : "#fffdfb"
                                    Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                                }
                            }
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: "Cor da roda"
                        color: appearance.ink
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: [
                                { label: "Lilás", hex: "#b99cde" },
                                { label: "Azul", hex: "#64a8ed" },
                                { label: "Verde", hex: "#58b89d" },
                                { label: "Rosa", hex: "#e58fac" },
                                { label: "Âmbar", hex: "#e7b45a" },
                                { label: "Neutro", hex: "#a5afbe" }
                            ]
                            delegate: Button {
                                id: preset
                                required property var modelData
                                required property int index
                                objectName: "wheelColorPreset" + index
                                Layout.fillWidth: true
                                implicitHeight: 33
                                Accessible.name: "Cor da roda: " + modelData.label
                                onClicked: appearance.draftColor = modelData.hex
                                background: Rectangle {
                                    radius: 10
                                    color: preset.down ? "#eee4f5" : preset.hovered ? "#f5eef9" : "#fffdfb"
                                    border.width: preset.visualFocus || appearance.draftColor === preset.modelData.hex ? 2 : 1
                                    border.color: preset.visualFocus || appearance.draftColor === preset.modelData.hex ? appearance.violet : "#e7ddeb"
                                }
                                contentItem: Item {
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18
                                        radius: 9
                                        color: preset.modelData.hex
                                        border.width: 1
                                        border.color: "#30ffffff"
                                    }
                                }
                                CleanToolTip {
                                    visible: preset.hovered
                                    text: preset.modelData.label
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        AppearanceButton {
                            id: chooseColorButton
                            objectName: "wheelColorChoose"
                            text: "Escolher cor"
                            Accessible.description: "Abrir o seletor de cor. A preferência só é aplicada ao salvar."
                            contentItem: RowLayout {
                                spacing: 8
                                Rectangle {
                                    implicitWidth: 16
                                    implicitHeight: 16
                                    radius: 8
                                    color: appearance.previewColor
                                    border.color: "#20000000"
                                }
                                Text {
                                    text: chooseColorButton.text
                                    color: appearance.ink
                                    font: chooseColorButton.font
                                }
                            }
                            onClicked: {
                                colorPicker.selectedColor = appearance.previewColor;
                                colorPicker.open();
                            }
                        }
                        TextField {
                            id: colorHex
                            objectName: "wheelColorHex"
                            Layout.fillWidth: true
                            implicitHeight: 36
                            text: appearance.draftColor
                            placeholderText: "#RRGGBB"
                            maximumLength: 7
                            selectByMouse: true
                            font.pixelSize: 12
                            color: appearance.ink
                            leftPadding: 12
                            rightPadding: 12
                            Accessible.name: "Cor da roda em hexadecimal"
                            Accessible.description: "Use cerquilha e seis dígitos de zero a nove ou A a F."
                            onTextEdited: appearance.draftColor = text.trim().toLowerCase()
                            background: Rectangle {
                                radius: 11
                                color: "#faf6fd"
                                border.color: !appearance.validColor ? "#bb6c80" : colorHex.activeFocus ? "#a47bc2" : "#e7ddeb"
                            }
                        }
                    }
                    Text {
                        objectName: "wheelColorError"
                        Layout.fillWidth: true
                        visible: !appearance.validColor
                        text: "Use uma cor como #B99CDE (seis dígitos após #)."
                        color: "#a85063"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        Accessible.role: Accessible.AlertMessage
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Transparência"
                            color: appearance.ink
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Rectangle {
                            implicitWidth: 54
                            implicitHeight: 29
                            radius: 10
                            color: "#f2e9fa"
                            Text {
                                anchors.centerIn: parent
                                text: appearance.draftTransparency + "%"
                                color: appearance.violet
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                    Slider {
                        id: transparencySlider
                        objectName: "wheelTransparencySlider"
                        Layout.fillWidth: true
                        from: 0
                        to: 80
                        stepSize: 1
                        value: appearance.draftTransparency
                        snapMode: Slider.SnapAlways
                        Accessible.name: "Transparência da roda"
                        Accessible.description: "De zero, mais sólida, a oitenta por cento, mais transparente."
                        onMoved: appearance.draftTransparency = Math.round(value)
                        Keys.onPressed: function (event) {
                            if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                                appearance.draftTransparency = event.key === Qt.Key_Home ? from : to;
                                event.accepted = true;
                            }
                        }
                        background: Rectangle {
                            x: transparencySlider.leftPadding
                            y: transparencySlider.topPadding + transparencySlider.availableHeight / 2 - height / 2
                            width: transparencySlider.availableWidth
                            height: 5
                            radius: 2.5
                            color: "#eee4f5"
                            Rectangle {
                                width: transparencySlider.visualPosition * parent.width
                                height: parent.height
                                radius: 2.5
                                color: "#a47bc2"
                            }
                        }
                        handle: Rectangle {
                            x: transparencySlider.leftPadding + transparencySlider.visualPosition * (transparencySlider.availableWidth - width)
                            y: transparencySlider.topPadding + transparencySlider.availableHeight / 2 - height / 2
                            implicitWidth: 22
                            implicitHeight: 22
                            radius: 11
                            color: transparencySlider.pressed ? "#eee1f8" : "#fffdfb"
                            border.color: transparencySlider.visualFocus ? appearance.violet : "#b18bca"
                            border.width: transparencySlider.visualFocus ? 2 : 1
                            Rectangle {
                                anchors.centerIn: parent
                                width: 6
                                height: 6
                                radius: 3
                                color: "#a47bc2"
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Mais sólido"
                            color: appearance.muted
                            font.pixelSize: 10
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Text {
                            text: "Mais transparente"
                            color: appearance.muted
                            font.pixelSize: 10
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Tamanho"
                            color: appearance.ink
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Rectangle {
                            implicitWidth: 54
                            implicitHeight: 29
                            radius: 10
                            color: "#f2e9fa"
                            Text {
                                anchors.centerIn: parent
                                text: appearance.draftSize + "%"
                                color: appearance.violet
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                    Slider {
                        id: sizeSlider
                        objectName: "wheelSizeSlider"
                        Layout.fillWidth: true
                        from: 70
                        to: 130
                        stepSize: 1
                        value: appearance.draftSize
                        snapMode: Slider.SnapAlways
                        Accessible.name: "Tamanho da roda"
                        Accessible.description: "De setenta a cento e trinta por cento. O tamanho padrão é cem por cento."
                        onMoved: appearance.draftSize = Math.round(value)
                        Keys.onPressed: function (event) {
                            if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                                appearance.draftSize = event.key === Qt.Key_Home ? from : to;
                                event.accepted = true;
                            }
                        }
                        background: Rectangle {
                            x: sizeSlider.leftPadding
                            y: sizeSlider.topPadding + sizeSlider.availableHeight / 2 - height / 2
                            width: sizeSlider.availableWidth
                            height: 5
                            radius: 2.5
                            color: "#eee4f5"
                            Rectangle {
                                width: sizeSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 2.5
                                color: "#a47bc2"
                            }
                        }
                        handle: Rectangle {
                            x: sizeSlider.leftPadding + sizeSlider.visualPosition * (sizeSlider.availableWidth - width)
                            y: sizeSlider.topPadding + sizeSlider.availableHeight / 2 - height / 2
                            implicitWidth: 22
                            implicitHeight: 22
                            radius: 11
                            color: sizeSlider.pressed ? "#eee1f8" : "#fffdfb"
                            border.color: sizeSlider.visualFocus ? appearance.violet : "#b18bca"
                            border.width: sizeSlider.visualFocus ? 2 : 1
                            Rectangle {
                                anchors.centerIn: parent
                                width: 6
                                height: 6
                                radius: 3
                                color: "#a47bc2"
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Mais compacta"
                            color: appearance.muted
                            font.pixelSize: 10
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Text {
                            text: "Mais ampla"
                            color: appearance.muted
                            font.pixelSize: 10
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        text: "Vale para as rodas flutuantes de conversão e edição. Os nomes e o ícone continuam nítidos."
                        color: appearance.muted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        objectName: "wheelAppearanceError"
                        visible: text.length > 0
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        text: appearance.saveError
                        color: "#a85063"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        Accessible.role: Accessible.AlertMessage
                    }
                }
            }
        }
    }
    // Dialog positions its footer independently of the body's deferred layout.
    // Keep these actions reachable while resizing or reopening a compact panel.
    footer: Item {
        implicitWidth: appearanceActions.implicitWidth + appearance.leftPadding + appearance.rightPadding
        implicitHeight: appearanceActions.implicitHeight + 22
        RowLayout {
            id: appearanceActions
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: appearance.leftPadding
            anchors.rightMargin: appearance.rightPadding
            spacing: 8
            AppearanceButton {
                objectName: "wheelAppearanceDefault"
                text: "Restaurar padrão"
                onClicked: {
                    appearance.draftTransparency = 35;
                    appearance.draftSize = 100;
                    appearance.draftColor = "#b99cde";
                    appearance.draftSoundEnabled = true;
                }
            }
            Item {
                Layout.fillWidth: true
            }
            AppearanceButton {
                objectName: "wheelAppearanceCancel"
                text: "Cancelar"
                onClicked: appearance.close()
            }
            AppearanceButton {
                objectName: "wheelAppearanceSave"
                text: "Salvar"
                primary: true
                enabled: appearance.validColor
                onClicked: {
                    if (!appearance.validColor)
                        return;
                    if (appearance.desktopBridge && typeof appearance.desktopBridge.setWheelAppearance === "function" && appearance.desktopBridge.setWheelAppearance(appearance.draftTransparency, appearance.draftSize, appearance.draftColor, appearance.draftSoundEnabled))
                        appearance.close();
                    else
                        appearance.saveError = appearance.desktopBridge && typeof appearance.desktopBridge.wheelAppearanceError === "string" && appearance.desktopBridge.wheelAppearanceError.length ? appearance.desktopBridge.wheelAppearanceError : "Não foi possível salvar a aparência. Tente novamente.";
                }
            }
        }
    }
}

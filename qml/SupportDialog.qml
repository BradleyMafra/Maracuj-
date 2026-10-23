import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: support
    objectName: "supportDialog"
    property var supportBridge: null
    property bool showPayload: false
    property string copyStatus: ""
    property bool copyFailed: false
    readonly property bool available: supportBridge !== null && supportBridge.available === true
    readonly property string pixPayload: available ? supportBridge.pixPayload : ""
    readonly property string pixKey: available ? supportBridge.pixKey : ""
    readonly property color ink: "#412c53"
    readonly property color muted: "#897792"
    readonly property color violet: "#63358b"
    anchors.centerIn: parent
    width: Math.min(parent.width - 32, 420)
    height: Math.min(implicitHeight, parent.height - 32)
    padding: 22
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: {
        showPayload = false;
        copyStatus = "";
        copyFailed = false;
        copyButton.forceActiveFocus();
    }
    background: Rectangle {
        radius: 22
        color: "#fffdfb"
        border.color: "#ddcce9"
    }
    Overlay.modal: Rectangle { color: "#69412c53" }

    component SupportButton: Button {
        id: action
        property bool primary: false
        implicitHeight: 38
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
            color: !action.enabled ? "#eee7f2" : action.primary
                ? (action.down ? "#512773" : action.hovered ? "#8050a3" : "#63358b")
                : action.down ? "#eaddf4" : action.hovered ? "#f4ecfa" : "#fffdfb"
            border.width: action.visualFocus ? 2 : action.primary ? 0 : 1
            border.color: action.visualFocus ? "#a47bc2" : "#e7ddeb"
        }
    }

    contentItem: ColumnLayout {
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Rectangle {
                implicitWidth: 38
                implicitHeight: 38
                radius: 12
                color: "#f3eafa"
                Image {
                    anchors.centerIn: parent
                    width: 23
                    height: 23
                    source: "qrc:/mandarina/assets/icons/menu-support.svg"
                    sourceSize.width: 46
                    sourceSize.height: 46
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Text {
                    Layout.fillWidth: true
                    text: "Apoiar o Maracujá"
                    color: support.violet
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    text: "Um gesto, se você quiser."
                    color: support.muted
                    font.pixelSize: 11
                }
            }
            SupportButton {
                objectName: "closeSupportDialog"
                implicitWidth: 32
                implicitHeight: 32
                leftPadding: 0
                rightPadding: 0
                text: "×"
                Accessible.name: "Fechar apoio"
                onClicked: support.close()
            }
        }
        ScrollView {
            id: supportScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: supportColumn.implicitHeight
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: supportColumn
                width: supportScroll.availableWidth
                spacing: 14
                Text {
                    Layout.fillWidth: true
                    text: "Se o Maracujá te ajuda, você pode apoiar com qualquer valor. O app continua completo e gratuito."
                    textFormat: Text.PlainText
                    color: support.ink
                    font.pixelSize: 12
                    lineHeight: 1.3
                    wrapMode: Text.WordWrap
                }
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 204
                    Layout.preferredHeight: 204
                    radius: 16
                    color: "white"
                    border.color: "#ebe3f1"
                    Image {
                        id: pixQr
                        objectName: "supportPixQr"
                        anchors.fill: parent
                        anchors.margins: 12
                        source: support.available ? support.supportBridge.qrSource : ""
                        sourceSize.width: 720
                        sourceSize.height: 720
                        fillMode: Image.PreserveAspectFit
                        smooth: false
                        Accessible.role: Accessible.Graphic
                        Accessible.name: "QR code Pix de apoio ao Maracujá"
                    }
                    Text {
                        objectName: "supportQrError"
                        anchors.fill: parent
                        anchors.margins: 20
                        visible: pixQr.status === Image.Error
                        text: "Não foi possível mostrar o QR.\nUse Copiar Pix abaixo."
                        color: support.muted
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                        lineHeight: 1.3
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Pix · valor livre"
                        color: support.ink
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "Escaneie com seu banco e confira o destinatário antes de confirmar."
                        color: support.muted
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: support.pixKey.length > 0
                    text: "Chave aleatória · " + support.pixKey
                    textFormat: Text.PlainText
                    color: support.muted
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WrapAnywhere
                }
                Button {
                    id: payloadButton
                    objectName: "showSupportPixPayload"
                    Layout.alignment: Qt.AlignHCenter
                    enabled: support.pixPayload.length > 0
                    text: support.showPayload ? "Ocultar código Pix" : "Ver código Pix"
                    implicitHeight: 28
                    leftPadding: 10
                    rightPadding: 10
                    Accessible.description: "Mostrar ou ocultar o código Pix copia e cola."
                    onClicked: support.showPayload = !support.showPayload
                    contentItem: Text {
                        text: payloadButton.text
                        color: support.violet
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: payloadButton.hovered || payloadButton.down ? "#f4ecfa" : "transparent"
                        border.width: payloadButton.visualFocus ? 1 : 0
                        border.color: "#a47bc2"
                    }
                }
                TextArea {
                    id: payloadField
                    objectName: "supportPixPayload"
                    Layout.fillWidth: true
                    visible: support.showPayload
                    text: support.pixPayload
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: support.ink
                    font.pixelSize: 11
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 10
                    bottomPadding: 10
                    Accessible.name: "Código Pix copia e cola"
                    background: Rectangle {
                        radius: 11
                        color: "#faf6fd"
                        border.color: payloadField.activeFocus ? "#a47bc2" : "#e7ddeb"
                    }
                }
            }
        }
        Text {
            objectName: "supportCopyStatus"
            Layout.fillWidth: true
            visible: support.copyStatus.length > 0
            text: support.copyStatus
            color: support.copyFailed ? "#a85065" : "#347b68"
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
        }
        SupportButton {
            id: copyButton
            objectName: "copySupportPix"
            Layout.fillWidth: true
            text: "Copiar Pix"
            primary: true
            enabled: support.available && support.pixPayload.length > 0
            Accessible.description: "Copiar o código Pix para colar no aplicativo do seu banco."
            onClicked: {
                var copied = support.supportBridge.copyPix();
                support.copyFailed = !copied;
                support.copyStatus = copied ? "Pix copiado. Obrigado pelo carinho!"
                    : "Não foi possível copiar. Você pode selecionar o código Pix acima.";
                if (!copied) support.showPayload = true;
            }
        }
    }
}

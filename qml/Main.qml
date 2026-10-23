import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    property bool compact: compactMode
    readonly property bool resident: typeof residentMode !== "undefined" && residentMode
    property bool exitRequested: false
    readonly property var desktopBridge: typeof desktopIntegration !== "undefined" ? desktopIntegration : null
    readonly property var toolsBridge: typeof fileTools !== "undefined" ? fileTools : null
    readonly property var toolsWindow: typeof fileToolsWindow !== "undefined" ? fileToolsWindow : null
    readonly property var photoBridge: typeof photoEditor !== "undefined" ? photoEditor : null
    readonly property var photoWindow: typeof photoEditorWindow !== "undefined" ? photoEditorWindow : null
    readonly property var supportBridge: typeof supportInfo !== "undefined" ? supportInfo : null
    readonly property bool toolsVisible: (toolsWindow ? toolsWindow.visible : false) || (photoWindow ? photoWindow.visible : false)
    property bool editingMode: false
    property string editingError: ""
    property string editingAction: ""
    readonly property bool interactionBusy: converter.busy || (toolsBridge ? toolsBridge.busy : false)
        || (photoBridge ? photoBridge.busy || photoBridge.rendering : false)
    readonly property var wheelFormats: editingMode ? (toolsBridge ? toolsBridge.formats || [] : []) : converter.formats
    readonly property string selectedWheelId: editingMode ? "tool:" + (editingAction || (toolsBridge ? toolsBridge.action : "compress")) : converter.selectedFormat
    readonly property string editingShortcutLabel: desktopBridge ? desktopBridge.toolsShortcutLabel : "Alt"
    readonly property bool presetPopupOpen: (qualityPreset && qualityPreset.popup ? qualityPreset.popup.visible : false)
        || (resolutionPreset && resolutionPreset.popup ? resolutionPreset.popup.visible : false)
    readonly property bool presetHasFocus: (qualityPreset ? qualityPreset.activeFocus : false) || (resolutionPreset ? resolutionPreset.activeFocus : false)
    readonly property string gestureShortcutLabel: desktopBridge ? desktopBridge.shortcutLabel : "Shift"
    readonly property bool gestureDragRequired: desktopBridge ? desktopBridge.dragRequired : true
    width: compact ? 550 : 1010; height: compact ? 610 : 720
    minimumWidth: compact ? 520 : 930; minimumHeight: compact ? 580 : 680
    onCompactChanged: Qt.callLater(function() {
        // Apply the target size after the mode's minimum size has updated.
        // Otherwise Qt can clamp the compact request to the old panel minimum.
        window.width = window.compact ? 550 : 1010
        window.height = window.compact ? 610 : 720
    })
    visible: !(typeof startInBackground !== "undefined" && startInBackground)
    title: "Maracujá · Conversor de arquivos"
    color: "#fffcf2"
    font.family: "Segoe UI"
    font.pixelSize: 14
    property bool closeAfterCancel: false
    readonly property color ink: "#412c53"
    readonly property color muted: "#897792"
    readonly property color violet: "#63358b"
    readonly property color gold: "#f3c64d"
    readonly property bool imageOptions: converter.inputKind === "image" && ["png", "jpg", "webp", "tiff", "bmp", "avif", "heic"].indexOf(converter.selectedFormat) >= 0
    readonly property bool pageImageOptions: (converter.inputKind === "pdf" || converter.inputKind === "document") && (converter.selectedFormat === "png" || converter.selectedFormat === "jpg")
    function urlStrings(urls) {
        // Qt 6.8 exposes native URL lists as QML sequences. Copy their strings
        // into a JS array so C++ QVariantList parameters receive each URL.
        var result = []
        for (var index = 0; index < urls.length; ++index) result.push(urls[index].toString())
        return result
    }
    function fileBadge(file) {
        if (file.kind === "pdf") return "PDF"
        if (file.kind === "audio") return "♪"
        var parts = file.name.split(".")
        return parts.length > 1 ? parts[parts.length - 1].toUpperCase().slice(0, 5) : "ARQ"
    }
    function setEditingMode(value) {
        if (interactionBusy) return
        editingMode = value && toolsBridge !== null
    }
    onEditingModeChanged: {
        editingError = ""
        converter.clearDragPreview()
        wheel.editingDrag = false
        wheel.hovered = -1
        wheelCanvas.requestPaint()
    }
    function addSelectedUrls(urls) {
        if (interactionBusy) return
        editingError = ""
        if (editingMode) converter.addEditingUrls(urls)
        else converter.addUrls(urls)
    }
    function selectedUrls() {
        var urls = []
        for (var index = 0; index < converter.files.length; ++index)
            urls.push(converter.files[index].url.toString())
        return urls
    }
    function openEditingAction(action, urls) {
        if (interactionBusy || !toolsBridge) return
        editingError = ""
        if (action === "photo" && urls.length === 0) { photoFileDialog.open(); return }
        if (action !== "photo" && toolsBridge.metadataDirty) {
            discardEditDialog.pendingAction = action
            discardEditDialog.pendingUrls = urls
            discardEditDialog.open()
        } else presentEditingOptions(action, urls)
    }
    function presentEditingOptions(action, urls) {
        if (!toolsBridge.openUrls(urls, action))
            editingError = action === "photo" ? "Escolha uma única foto estática de até 24 megapixels para editar."
                : "Não foi possível abrir esta seleção. Confira se os arquivos ainda estão disponíveis."
        else editingAction = action
    }
    onClosing: function(close) {
        if (window.resident && !window.exitRequested) {
            close.accepted = false
            window.hide()
            return
        }
        if (converter.busy || (toolsBridge && toolsBridge.busy) || (photoBridge && photoBridge.busy)) {
            close.accepted = false; closeAfterCancel = true; converter.cancel()
            if(toolsBridge) toolsBridge.cancel()
            if(photoBridge) photoBridge.cancel()
        }
        else if (desktopBridge) Qt.quit()
    }
    Connections {
        target: converter
        function onBusyChanged() { if (!converter.busy && !window.interactionBusy && window.closeAfterCancel) window.close() }
        function onFormatsChanged() { wheel.hovered = -1; wheelCanvas.requestPaint() }
        function onSelectedFormatChanged() { wheelCanvas.requestPaint() }
    }
    Connections {
        target: window.toolsBridge
        function onBusyChanged() { if (!window.interactionBusy && window.closeAfterCancel) window.close() }
        function onChanged() { if (window.editingMode) wheelCanvas.requestPaint() }
    }
    Connections {
        target: window.photoBridge
        function onBusyChanged() { if (!window.interactionBusy && window.closeAfterCancel) window.close() }
        function onChanged() { if (!window.interactionBusy && window.closeAfterCancel) window.close() }
    }

    component SoftButton: Button {
        id: control
        property bool primary: false
        implicitHeight: 42
        leftPadding: 18; rightPadding: 18
        font.pixelSize: 13
        font.weight: Font.DemiBold
        contentItem: Text {
            text: control.text; font: control.font
            color: !control.enabled ? "#aaa2b5" : control.primary ? "white" : window.ink
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 12
            color: !control.enabled ? "#ece7ef" : control.primary ? (control.down ? "#512773" : control.hovered ? "#9458c4" : window.violet) : control.hovered ? "#f0e7f7" : "#fffdfa"
            border.width: control.primary ? 0 : 1
            border.color: "#e7ddeb"
            Behavior on color { ColorAnimation { duration: 140 } }
        }
    }

    component AppMenuItem: MenuItem {
        id: menuEntry
        property string hint: ""
        property url symbol
        property bool exitAction: false
        // Menu owns one highlighted item across both pointer and keyboard
        // navigation. A previous item may still retain activeFocus.
        readonly property bool emphasized: highlighted
        hoverEnabled: true
        implicitWidth: 246; implicitHeight: 54
        leftPadding: 10; rightPadding: 10
        topPadding: 7; bottomPadding: 7
        font.pixelSize: 13
        Accessible.description: hint
        contentItem: RowLayout {
            spacing: 10
            Rectangle {
                Layout.preferredWidth: 32; Layout.preferredHeight: 32
                radius: 10
                color: menuEntry.exitAction ? (menuEntry.emphasized ? "#f6dde5" : "#faf0f3")
                    : menuEntry.emphasized ? "#e6d6f2" : "#f2eaf8"
                Image {
                    anchors.centerIn: parent; width: 18; height: 18
                    source: menuEntry.symbol
                    sourceSize.width: 36; sourceSize.height: 36
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 3
                Text {
                    Layout.fillWidth: true
                    text: menuEntry.text; textFormat: Text.PlainText
                    font.pixelSize: 13; font.weight: Font.DemiBold
                    color: menuEntry.exitAction ? "#984b65" : menuEntry.emphasized ? window.violet : window.ink
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: menuEntry.hint; textFormat: Text.PlainText
                    font.pixelSize: 10; color: window.muted
                    elide: Text.ElideRight
                }
            }
        }
        background: Rectangle {
            radius: 11
            color: !menuEntry.emphasized ? "transparent" : menuEntry.exitAction
                ? (menuEntry.down ? "#f6dfe7" : "#fff1f5") : (menuEntry.down ? "#e5d1f1" : "#f3eafb")
            border.width: menuEntry.emphasized ? (menuEntry.visualFocus ? 2 : 1) : 0
            border.color: menuEntry.exitAction ? "#ebc7d4" : menuEntry.visualFocus ? "#b18acb" : "#e0cdee"
            // Switch the fill immediately: fading an outgoing row would show
            // two hovered backgrounds and interpolate through a gray shade.
            Rectangle {
                x: 0; y: (parent.height - height) / 2
                width: 3; height: 22; radius: 1.5
                visible: menuEntry.emphasized
                color: menuEntry.exitAction ? "#b46782" : window.violet
            }
        }
    }

    component ConversionPreset: ComboBox {
        id: preset
        property string caption: ""
        property url symbol
        readonly property var choice: currentIndex >= 0 && currentIndex < count ? model[currentIndex] : null
        implicitHeight: 58
        leftPadding: 10; rightPadding: 36
        topPadding: 9; bottomPadding: 9
        textRole: "label"; valueRole: "value"
        enabled: !window.interactionBusy
        hoverEnabled: true
        Accessible.name: caption
        Accessible.description: choice ? choice.label + ", " + choice.detail : caption
        contentItem: RowLayout {
            spacing: 10; opacity: preset.enabled ? 1 : 0.5
            Rectangle {
                Layout.preferredWidth: 32; Layout.preferredHeight: 32
                radius: 10; color: "#eaddf4"
                Image { anchors.centerIn: parent; width: 18; height: 18; sourceSize.width: 36; sourceSize.height: 36; source: preset.symbol }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 3
                Text {
                    Layout.fillWidth: true
                    text: preset.caption; textFormat: Text.PlainText
                    color: window.muted; font.pixelSize: 10; font.weight: Font.Medium
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    Text {
                        text: preset.choice ? preset.choice.label : ""; textFormat: Text.PlainText
                        color: window.ink; font.pixelSize: 13; font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: preset.choice ? preset.choice.detail : ""; textFormat: Text.PlainText
                        color: window.muted; font.pixelSize: 10; elide: Text.ElideRight
                    }
                }
            }
        }
        indicator: Image {
            width: 14; height: 14; sourceSize.width: 28; sourceSize.height: 28
            x: preset.width - width - 12; y: (preset.height - height) / 2
            source: "qrc:/mandarina/assets/icons/selector-chevron.svg"
            opacity: preset.enabled ? 1 : 0.4
            rotation: preset.popup.visible ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
        background: Rectangle {
            radius: 13
            color: !preset.enabled ? "#f3eff6" : preset.down || preset.popup.visible ? "#eee1f8" : preset.hovered ? "#f2e9fa" : "#f7f1fb"
            border.color: preset.visualFocus || preset.popup.visible ? "#b18acb" : "transparent"
            Behavior on color { ColorAnimation { duration: 140 } }
        }
        delegate: ItemDelegate {
            id: presetOption
            required property var modelData
            required property int index
            width: ListView.view ? ListView.view.width : preset.width - 12
            implicitHeight: 48
            leftPadding: 11; rightPadding: 11
            highlighted: preset.highlightedIndex === index
            hoverEnabled: preset.hoverEnabled
            text: modelData.label
            Accessible.description: modelData.detail
            contentItem: RowLayout {
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 3
                    Text { text: presetOption.modelData.label; textFormat: Text.PlainText; color: window.ink; font.pixelSize: 12; font.weight: Font.DemiBold }
                    Text { Layout.fillWidth: true; text: presetOption.modelData.detail; textFormat: Text.PlainText; color: window.muted; font.pixelSize: 10; elide: Text.ElideRight }
                }
                Rectangle {
                    Layout.preferredWidth: 6; Layout.preferredHeight: 6
                    radius: 3; color: window.violet
                    visible: preset.currentIndex === presetOption.index
                }
            }
            background: Rectangle {
                radius: 9
                color: presetOption.highlighted ? "#eee2f8" : preset.currentIndex === presetOption.index ? "#f7f1fb" : "transparent"
                Behavior on color { ColorAnimation { duration: 100 } }
            }
        }
        popup: Popup {
            y: preset.height + 6
            width: preset.width
            implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
            height: Math.min(implicitHeight, window.height - 24)
            padding: 6; margins: 10
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
            onAboutToShow: {
                var bottom = preset.mapToItem(window.contentItem, 0, preset.height).y
                y = window.height - bottom < height + 10 ? -height - 6 : preset.height + 6
            }
            contentItem: ListView {
                clip: true; implicitHeight: contentHeight
                model: preset.delegateModel
                currentIndex: preset.highlightedIndex
                highlightMoveDuration: 0
                ScrollIndicator.vertical: ScrollIndicator { }
            }
            background: Rectangle {
                radius: 14; color: "#fffdfb"; border.color: "#ddcee8"
                Rectangle { z: -1; x: -1; y: 3; width: parent.width + 2; height: parent.height + 1; radius: 15; color: "#12412c53" }
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: window.editingMode ? "Escolher arquivos para editar" : "Escolher arquivos para converter"
        fileMode: FileDialog.OpenFiles
        nameFilters: window.editingMode ? ["Todos os arquivos (*)"] : [
            "Arquivos compatíveis (*.jpg *.jpeg *.png *.webp *.bmp *.tif *.tiff *.avif *.heic *.heif *.gif *.pdf *.txt *.md *.markdown *.html *.htm *.docx *.odt *.rtf *.mp3 *.m4a *.wav *.flac *.ogg *.opus *.aiff *.aif *.wma)",
            "Imagens (*.jpg *.jpeg *.png *.webp *.bmp *.tif *.tiff *.avif *.heic *.heif *.gif)",
            "Documentos e PDF (*.pdf *.txt *.md *.markdown *.html *.htm *.docx *.odt *.rtf)",
            "Áudio (*.mp3 *.m4a *.wav *.flac *.ogg *.opus *.aiff *.aif *.wma)",
            "Todos os arquivos (*)"
        ]
        onAccepted: window.addSelectedUrls(window.urlStrings(selectedFiles))
    }
    FolderDialog {
        id: folderDialog
        title: "Salvar cópias em…"
        onAccepted: converter.outputDirectory = selectedFolder.toString()
    }
    FileDialog {
        id: photoFileDialog
        title: "Escolher uma foto para editar"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Imagens (*.jpg *.jpeg *.png *.webp *.bmp *.tif *.tiff *.avif *.heic *.heif)", "Todos os arquivos (*)"]
        onAccepted: if (!window.interactionBusy) window.presentEditingOptions("photo", window.urlStrings(selectedFiles))
    }
    Shortcut { sequence: "Ctrl+O"; enabled: !window.presetPopupOpen && !appMenu.visible && !shortcutDialog.visible && !wheelAppearanceDialog.visible && !supportDialog.visible && !discardEditDialog.visible && !window.toolsVisible; onActivated: if (!window.interactionBusy) fileDialog.open() }
    Shortcut { sequence: "Return"; enabled: !window.presetPopupOpen && !window.presetHasFocus && !appMenu.visible && !window.editingMode && !window.interactionBusy && !shortcutDialog.visible && !wheelAppearanceDialog.visible && !supportDialog.visible && !discardEditDialog.visible && !window.toolsVisible; onActivated: if (!fileDialog.visible && !photoFileDialog.visible && !folderDialog.visible) converter.start() }
    Shortcut { sequence: "Escape"; enabled: !window.presetPopupOpen && !appMenu.visible && !shortcutDialog.visible && !wheelAppearanceDialog.visible && !supportDialog.visible && !discardEditDialog.visible && !window.toolsVisible; onActivated: if (converter.busy) converter.cancel(); else if (window.compact) window.close(); else converter.clearDragPreview() }

    WheelAppearanceDialog {
        id: wheelAppearanceDialog
        desktopBridge: window.desktopBridge
    }

    SupportDialog {
        id: supportDialog
        supportBridge: window.supportBridge
    }

    Dialog {
        id: discardEditDialog
        anchors.centerIn: parent
        title: "Descartar alterações nos metadados?"
        modal: true
        width: Math.min(window.width - 40, 400)
        property string pendingAction: "compress"
        property var pendingUrls: []
        standardButtons: Dialog.Discard | Dialog.Cancel
        contentItem: Text {
            text: "Há alterações que ainda não foram salvas. Descartá-las para abrir esta ação?"
            color: window.ink; wrapMode: Text.WordWrap
        }
        onDiscarded: if (!window.interactionBusy && window.toolsBridge) window.presentEditingOptions(pendingAction, pendingUrls)
    }

    Dialog {
        id: shortcutDialog
        objectName: "gestureShortcutDialog"
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 510)
        height: Math.min(implicitHeight, window.height - 36)
        padding: 26
        modal: true
        focus: true
        closePolicy: recording ? Popup.NoAutoClose : Popup.CloseOnEscape
        property string draftShortcut: "Shift"
        property string draftLabel: "Shift"
        property bool draftDragRequired: true
        property string validationError: ""
        property bool recording: false
        property bool editingTools: false
        property bool receivedKey: false
        property string previousShortcut: "Shift"
        property string previousLabel: "Shift"
        function updateDraft(value) {
            draftShortcut = value
            draftLabel = window.desktopBridge ? window.desktopBridge.shortcutDisplay(value) : value
            validationError = validateDraft(value)
        }
        function validateDraft(value) {
            return window.desktopBridge ? (editingTools ? window.desktopBridge.validateToolsShortcut(value)
                : window.desktopBridge.validateShortcut(value,draftDragRequired)) : ""
        }
        function loadCurrent() {
            recording=false; receivedKey=false
            draftDragRequired=editingTools || !window.desktopBridge ? true : window.desktopBridge.dragRequired
            updateDraft(window.desktopBridge ? (editingTools ? window.desktopBridge.toolsShortcut : window.desktopBridge.shortcut)
                : editingTools ? "Alt" : "Shift")
        }
        function updateMode(requireDrag) {
            draftDragRequired = requireDrag
            validationError = validateDraft(draftShortcut)
        }
        function beginRecording() {
            previousShortcut = draftShortcut
            previousLabel = draftLabel
            validationError = ""
            receivedKey = false
            recording = true
            shortcutCapture.forceActiveFocus()
        }
        function cancelRecording() {
            recording = false
            draftShortcut = previousShortcut
            draftLabel = previousLabel
            validationError = validateDraft(draftShortcut)
        }
        onOpened: {
            if (window.desktopBridge) window.desktopBridge.setShortcutCaptureActive(true)
            loadCurrent()
        }
        onClosed: {
            recording = false
            if (window.desktopBridge) window.desktopBridge.setShortcutCaptureActive(false)
        }
        background: Rectangle {
            radius: 22
            color: "#fffdfb"
            border.color: "#ddcce9"
        }
        Overlay.modal: Rectangle { color: "#69412c53" }
        contentItem: ScrollView {
            id: shortcutSettingsScroll
            implicitHeight: shortcutSettingsColumn.implicitHeight
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: shortcutSettingsColumn
                width: shortcutSettingsScroll.availableWidth
                spacing: 18
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Text { text: shortcutDialog.editingTools ? "Atalho de edição" : "Atalho da roda"; color: window.violet; font.pixelSize: 23; font.weight: Font.DemiBold }
                        Text { text: "Seu gesto, do seu jeito."; color: window.muted; font.pixelSize: 12 }
                    }
                    Rectangle {
                        Layout.preferredWidth: 40; Layout.preferredHeight: 40
                        radius: 13; color: "#f4ecfc"
                        Text { anchors.centerIn: parent; text: "⌨"; color: window.violet; font.pixelSize: 23 }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    SoftButton { text:"Conversão"; Layout.fillWidth:true; primary:!shortcutDialog.editingTools
                        onClicked:{ shortcutDialog.editingTools=false; shortcutDialog.loadCurrent() } }
                    SoftButton { objectName:"configureToolsShortcut"; text:"Edição"; Layout.fillWidth:true; primary:shortcutDialog.editingTools
                        onClicked:{ shortcutDialog.editingTools=true; shortcutDialog.loadCurrent() } }
                }
                Text {
                    Layout.fillWidth: true
                    text: shortcutDialog.editingTools ? "Arraste um arquivo, pressione seu segundo atalho e escolha Comprimir, Metadados ou Editar foto."
                        : shortcutDialog.draftDragRequired
                        ? "Enquanto arrasta um arquivo, pressione seu atalho. Depois solte o arquivo no formato desejado."
                        : "Pressione seu atalho para abrir a roda com os arquivos do painel. Selecione arquivos na roda quando a lista estiver vazia."
                    color: window.ink; font.pixelSize: 13; wrapMode: Text.WordWrap; lineHeight: 1.25
                }
                ColumnLayout {
                    visible: !shortcutDialog.editingTools
                    Layout.fillWidth: true; spacing: 8
                    Text { text: "COMO ABRIR"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1; font.weight: Font.DemiBold }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        SoftButton {
                            objectName: "gestureModeWithDrag"
                            Layout.fillWidth: true; text: "Com arraste"
                            primary: shortcutDialog.draftDragRequired
                            implicitHeight: 38
                            onClicked: { shortcutDialog.recording = false; shortcutDialog.updateMode(true) }
                        }
                        SoftButton {
                            objectName: "gestureModeShortcutOnly"
                            Layout.fillWidth: true; text: "Somente teclas"
                            primary: !shortcutDialog.draftDragRequired
                            implicitHeight: 38
                            onClicked: { shortcutDialog.recording = false; shortcutDialog.updateMode(false) }
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: savedShortcutLabel.implicitHeight + 24
                    radius: 12; color: "#f8f3fc"
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 12; spacing: 12
                        Text { text: "Em uso"; color: window.muted; font.pixelSize: 12 }
                        Text {
                            id: savedShortcutLabel
                            objectName: "savedGestureShortcut"
                            Layout.fillWidth: true
                            text: shortcutDialog.editingTools
                                ? (window.desktopBridge ? window.desktopBridge.toolsShortcutLabel : "Alt") + " + arrastar"
                                : window.gestureShortcutLabel + (window.gestureDragRequired ? " + arrastar" : " · sem arraste")
                            color: window.violet; font.pixelSize: 13; font.weight: Font.DemiBold
                            wrapMode: Text.WrapAnywhere; horizontalAlignment: Text.AlignRight
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 8
                    Text { text: "NOVO ATALHO"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1; font.weight: Font.DemiBold }
                    FocusScope {
                        id: shortcutCapture
                        objectName: "gestureShortcutCapture"
                        Layout.fillWidth: true; implicitHeight: 76
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: "Gravar atalho da roda"
                        Accessible.description: "Ative este campo e pressione uma tecla ou combinação. Escape cancela a gravação."
                        Keys.priority: Keys.BeforeItem
                        Keys.onPressed: function(event) {
                            event.accepted = shortcutDialog.recording
                            if (event.isAutoRepeat) return
                            if (!shortcutDialog.recording) {
                                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                    event.accepted = true
                                    shortcutDialog.beginRecording()
                                }
                                return
                            }
                            if (event.key === Qt.Key_Escape) { shortcutDialog.cancelRecording(); return }
                            var value = window.desktopBridge ? window.desktopBridge.shortcutFromKey(event.key, event.modifiers) : ""
                            if (value.length) {
                                shortcutDialog.updateDraft(value)
                                shortcutDialog.receivedKey = true
                            }
                        }
                        Keys.onReleased: function(event) {
                            event.accepted = shortcutDialog.recording
                            if (!event.isAutoRepeat && shortcutDialog.recording && shortcutDialog.receivedKey) shortcutDialog.recording = false
                        }
                        Rectangle {
                            anchors.fill: parent; radius: 14
                            color: shortcutDialog.recording ? "#f3e7ff" : captureHover.containsMouse ? "#faf5ff" : "#fffcf7"
                            border.width: shortcutDialog.recording || shortcutCapture.activeFocus ? 2 : 1
                            border.color: shortcutDialog.recording ? "#9c67c6" : shortcutCapture.activeFocus ? window.violet : "#e3d5ea"
                            Behavior on color { ColorAnimation { duration: 140 } }
                            Behavior on border.color { ColorAnimation { duration: 140 } }
                            Column {
                                anchors.centerIn: parent; spacing: 7; width: parent.width - 24
                                Text {
                                    objectName: "gestureShortcutDraftLabel"
                                    width: parent.width
                                    text: shortcutDialog.recording && !shortcutDialog.receivedKey ? "Pressione seu atalho…" : shortcutDialog.draftLabel
                                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WrapAnywhere
                                    color: window.violet; font.pixelSize: 20; font.weight: Font.DemiBold
                                }
                                Text {
                                    width: parent.width
                                    text: shortcutDialog.recording ? "Solte as teclas para concluir · Esc cancela" : "Clique para gravar outra tecla ou combinação"
                                    horizontalAlignment: Text.AlignHCenter; color: window.muted; font.pixelSize: 11
                                }
                            }
                        }
                        MouseArea { id: captureHover; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: shortcutDialog.beginRecording() }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: shortcutDialog.validationError.length ? shortcutDialog.validationError
                            : shortcutDialog.draftDragRequired
                                ? "Shift ou Alt funcionam com arraste. Para letras e números, use Ctrl, Alt ou Meta. A captura acontece apenas neste campo."
                                : "Use uma combinação como Ctrl+Alt+M ou uma tecla de função, como F9. O atalho fica reservado enquanto o Maracujá estiver ativo."
                        color: shortcutDialog.validationError.length ? "#a85061" : window.muted
                        font.pixelSize: 11; wrapMode: Text.WordWrap; lineHeight: 1.2
                    }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#ede4f2" }
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    SoftButton {
                        objectName: "resetGestureShortcut"
                        text: shortcutDialog.editingTools ? "Usar Alt" : "Usar Shift"
                        implicitHeight: 38; leftPadding: 12; rightPadding: 12
                        onClicked: { shortcutDialog.recording = false; shortcutDialog.draftDragRequired = true; shortcutDialog.updateDraft(shortcutDialog.editingTools ? "Alt" : "Shift") }
                    }
                    Item { Layout.fillWidth: true }
                    SoftButton {
                        objectName: "cancelGestureShortcut"
                        text: "Cancelar"
                        implicitHeight: 38; leftPadding: 12; rightPadding: 12
                        onClicked: shortcutDialog.close()
                    }
                    SoftButton {
                        objectName: "saveGestureShortcut"
                        text: "Salvar"
                        primary: true; implicitHeight: 38; leftPadding: 18; rightPadding: 18
                        enabled: !shortcutDialog.recording && !shortcutDialog.validationError.length && shortcutDialog.draftShortcut.length > 0
                        onClicked: {
                            if (window.desktopBridge && (shortcutDialog.editingTools ? window.desktopBridge.setToolsActivation(shortcutDialog.draftShortcut)
                                : window.desktopBridge.setActivation(shortcutDialog.draftShortcut, shortcutDialog.draftDragRequired))) shortcutDialog.close()
                            else shortcutDialog.validationError = window.desktopBridge && window.desktopBridge.shortcutError.length
                                ? window.desktopBridge.shortcutError : "Não foi possível salvar o atalho. Tente novamente."
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: "#fffcf2" }
            GradientStop { position: 0.55; color: "#faf6fb" }
            GradientStop { position: 1; color: "#f0e8fa" }
        }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: window.compact ? 20 : 26; spacing: window.compact ? 14 : 20
        RowLayout {
            spacing: 12
            Image { source: "qrc:/mandarina/assets/org.winux.Mandarina.svg"; Layout.preferredWidth: window.compact ? 40 : 48; Layout.preferredHeight: window.compact ? 40 : 48 }
            ColumnLayout {
                spacing: 1
                Text { text: "Maracujá"; font.pixelSize: window.compact ? 22 : 26; font.weight: Font.DemiBold; color: window.violet }
                Text { text: window.compact ? (window.editingMode ? "Edição local · originais preservados" : "Conversão local · originais preservados") : "Seu atalho entre formatos."; font.pixelSize: window.compact ? 10 : 13; color: window.muted }
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                visible: !window.compact
                implicitWidth: offlineLabel.implicitWidth + 22; implicitHeight: 30; radius: 15; color: "#e8f2ed"
                Text { id: offlineLabel; anchors.centerIn: parent; text: "●  100% local"; color: "#3c7963"; font.pixelSize: 12; font.weight: Font.Medium }
            }
            Switch {
                id: gestureEnabledSwitch
                objectName: "gestureEnabledSwitch"
                visible: !window.compact && window.desktopBridge !== null
                implicitWidth: 116; implicitHeight: 36
                leftPadding: mirrored ? 12 : 56
                rightPadding: mirrored ? 56 : 12
                text: checked ? "Ativo" : "Inativo"
                checked: window.desktopBridge ? window.desktopBridge.enabled : false
                enabled: window.desktopBridge ? window.desktopBridge.available : false
                onToggled: if (window.desktopBridge) window.desktopBridge.enabled = checked
                opacity: enabled ? 1 : 0.55
                hoverEnabled: true
                Accessible.name: "Ativar atalhos globais"
                Accessible.description: text + (window.desktopBridge ? " · " + window.desktopBridge.status : "")
                contentItem: Text {
                    text: gestureEnabledSwitch.text
                    color: gestureEnabledSwitch.checked ? "#39795c" : "#a65a68"
                    font.pixelSize: 12; font.weight: Font.DemiBold
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: gestureEnabledSwitch.mirrored ? Text.AlignRight : Text.AlignLeft
                    Behavior on color { ColorAnimation { duration: 190 } }
                }
                background: Rectangle {
                    radius: 18
                    color: gestureEnabledSwitch.checked
                        ? (gestureEnabledSwitch.hovered ? "#e9f5ed" : "#f1f8f3")
                        : (gestureEnabledSwitch.hovered ? "#fae9ed" : "#fcf2f3")
                    border.width: gestureEnabledSwitch.visualFocus ? 2 : 1
                    border.color: gestureEnabledSwitch.checked
                        ? (gestureEnabledSwitch.visualFocus ? "#61a982" : "#d1e7d9")
                        : (gestureEnabledSwitch.visualFocus ? "#ce8d9a" : "#efd8de")
                    Behavior on color { ColorAnimation { duration: 190 } }
                    Behavior on border.color { ColorAnimation { duration: 190 } }
                }
                indicator: Rectangle {
                    x: gestureEnabledSwitch.mirrored ? gestureEnabledSwitch.width - width - 8 : 8
                    y: (gestureEnabledSwitch.height - height) / 2
                    width: 40; height: 22; radius: 11
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: gestureEnabledSwitch.checked ? "#82c5a2" : "#e2a6b0"; Behavior on color { ColorAnimation { duration: 190 } } }
                        GradientStop { position: 1; color: gestureEnabledSwitch.checked ? "#56a980" : "#ce8091"; Behavior on color { ColorAnimation { duration: 190 } } }
                    }
                    Rectangle {
                        anchors.fill: parent; anchors.margins: -3; z: -1; radius: 14
                        color: gestureEnabledSwitch.checked ? "#1865b58a" : "#18d58b9b"
                        Behavior on color { ColorAnimation { duration: 190 } }
                    }
                    Rectangle {
                        width: 18; height: 18; radius: 9; y: 2
                        x: 2 + gestureEnabledSwitch.visualPosition * (parent.width - width - 4)
                        color: "#fffefd"
                        border.color: gestureEnabledSwitch.checked ? "#e5f3e9" : "#f7e7ed"
                        scale: gestureEnabledSwitch.down ? 0.93 : 1
                        Behavior on x { enabled: !gestureEnabledSwitch.down; NumberAnimation { duration: 190; easing.type: Easing.OutCubic } }
                        Behavior on scale { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }
                        Behavior on border.color { ColorAnimation { duration: 190 } }
                    }
                }
                CleanToolTip {
                    parent: gestureEnabledSwitch
                    visible: gestureEnabledSwitch.hovered
                    heading: window.gestureShortcutLabel + (window.gestureDragRequired ? " + arrastar" : " · sem arraste")
                    text: window.desktopBridge ? window.desktopBridge.status : ""
                }
            }
            SoftButton {
                objectName: "openGestureShortcutSettings"
                visible: !window.compact && window.desktopBridge !== null
                text: "Atalho"
                implicitHeight: 36; leftPadding: 12; rightPadding: 12
                onClicked: { shortcutDialog.editingTools=false; shortcutDialog.open() }
                CleanToolTip {
                    visible: parent.hovered && !shortcutDialog.visible
                    text: "Configurar como abrir a roda de conversões"
                }
            }
            SoftButton { text: window.compact ? "＋  Arquivos" : "＋  Adicionar arquivos"; enabled: !window.interactionBusy; onClicked: fileDialog.open() }
            SoftButton {
                id: moreButton
                objectName: "openAppMenu"
                visible: window.resident
                implicitWidth: 40; implicitHeight: 40
                leftPadding: 0; rightPadding: 0
                Accessible.name: "Mais opções"
                CleanToolTip {
                    parent: moreButton
                    visible: moreButton.hovered && !appMenu.visible
                    text: "Mais opções"
                }
                contentItem: Item {
                    Row {
                        anchors.centerIn: parent; spacing: 3
                        Repeater {
                            model: 3
                            Rectangle { width: 3; height: 3; radius: 1.5; color: window.violet }
                        }
                    }
                }
                background: Rectangle {
                    radius: 12
                    color: appMenu.visible ? "#eaddf4" : moreButton.hovered ? "#f1e8f8" : "#fffdfa"
                    border.color: appMenu.visible || moreButton.activeFocus ? "#b18acb" : "#e2d6eb"
                    Behavior on color { ColorAnimation { duration: 140 } }
                }
                onClicked: appMenu.visible ? appMenu.close() : appMenu.open()
                Menu {
                    id: appMenu
                    objectName: "appMenu"
                    x: moreButton.width - width
                    y: moreButton.height + 8
                    width: 262; padding: 8; margins: 10
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                    background: Rectangle {
                        radius: 18; color: "#fffdfb"
                        border.color: "#ddcee8"
                        Rectangle {
                            z: -1; x: -1; y: 3
                            width: parent.width + 2; height: parent.height + 2
                            radius: 19; color: "#12412c53"
                        }
                    }
                    AppMenuItem {
                        objectName: "menuHideWindow"
                        text: "Ocultar janela"; hint: "Continuar em segundo plano"
                        symbol: "qrc:/mandarina/assets/icons/menu-hide.svg"
                        onTriggered: window.hide()
                    }
                    AppMenuItem {
                        objectName: "menuConfigureShortcuts"
                        text: "Configurar atalhos"; hint: "Conversão e edição, do seu jeito"
                        symbol: "qrc:/mandarina/assets/icons/menu-shortcuts.svg"
                        onTriggered: { shortcutDialog.editingTools=false; shortcutDialog.open() }
                    }
                    AppMenuItem {
                        objectName: "menuWheelAppearance"
                        text: "Aparência da roda"; hint: "Cor, tamanho e transparência"
                        symbol: "qrc:/mandarina/assets/icons/menu-wheel.svg"
                        enabled: window.desktopBridge !== null
                        onTriggered: wheelAppearanceDialog.open()
                    }
                    AppMenuItem {
                        objectName: "menuSupport"
                        text: "Apoiar o Maracujá"; hint: "Opcional, com qualquer valor"
                        symbol: "qrc:/mandarina/assets/icons/menu-support.svg"
                        enabled: window.supportBridge !== null && window.supportBridge.available === true
                        onTriggered: supportDialog.open()
                    }
                    MenuSeparator {
                        topPadding: 6; bottomPadding: 6; leftPadding: 10; rightPadding: 10
                        contentItem: Rectangle { implicitHeight: 1; color: "#eee5f3" }
                    }
                    AppMenuItem {
                        objectName: "menuQuitApp"
                        text: "Sair do Maracujá"; hint: "Encerrar o aplicativo"
                        exitAction: true
                        symbol: "qrc:/mandarina/assets/icons/menu-exit.svg"
                        onTriggered: if (window.desktopBridge) window.desktopBridge.Quit()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 24
            Rectangle {
                visible: !window.compact
                Layout.preferredWidth: 300; Layout.fillHeight: true
                radius: 20; color: "#fffdfb"; border.color: "#e9e0ee"
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 20; spacing: 14
                    RowLayout {
                        Text { text: "Seus arquivos"; color: window.ink; font.pixelSize: 16; font.weight: Font.DemiBold }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "Limpar"; flat: true; enabled: converter.files.length > 0 && !window.interactionBusy
                            onClicked: converter.clearFiles()
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        radius: 14; color: listDrop.containsDrag ? "#f0e4fc" : "#faf7fb"
                        border.color: listDrop.containsDrag ? "#ad7ed0" : "#eee6f2"
                        Behavior on color { ColorAnimation { duration: 160 } }
                        Behavior on border.color { ColorAnimation { duration: 160 } }
                        Column {
                            anchors.centerIn: parent; width: parent.width - 30; spacing: 12
                            visible: converter.files.length === 0
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "＋"; color: "#b5a6c9"; font.pixelSize: 38; font.weight: Font.Light }
                            Text { width: parent.width; text: window.editingMode ? "Arraste os arquivos\nque deseja editar" : "Arraste imagens, documentos\nou áudios para cá"; horizontalAlignment: Text.AlignHCenter; color: window.muted; lineHeight: 1.3 }
                            Text { width: parent.width; text: "ou use Adicionar arquivos"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; color: "#aaa0b5" }
                        }
                        ListView {
                            anchors.fill: parent; anchors.margins: 8
                            clip: true; spacing: 6; model: converter.files
                            ScrollBar.vertical: ScrollBar { }
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                width: ListView.view.width; height: 62; radius: 10; color: "white"
                                RowLayout {
                                    anchors.fill: parent; anchors.margins: 8; spacing: 10
                                    Rectangle {
                                        Layout.preferredWidth: 40; Layout.preferredHeight: 42; radius: 8
                                        color: modelData.kind === "pdf" ? "#fff0ed" : modelData.kind === "audio" ? "#f5ebfc" : modelData.kind === "document" ? "#edf2fc" : "#f0edf8"
                                        Image { anchors.fill: parent; anchors.margins: 2; source: modelData.kind === "image" ? modelData.url : ""; sourceSize.width: 80; sourceSize.height: 80; fillMode: Image.PreserveAspectFit; asynchronous: true }
                                        Text { anchors.centerIn: parent; visible: modelData.kind !== "image"; text: window.fileBadge(modelData); font.pixelSize: modelData.kind === "audio" ? 24 : 10; font.bold: true; color: modelData.kind === "pdf" ? "#bd705f" : window.violet }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true; spacing: 3
                                        Text { Layout.fillWidth: true; textFormat: Text.PlainText; text: modelData.name; elide: Text.ElideMiddle; color: window.ink; font.pixelSize: 12 }
                                        Text { text: modelData.size; color: window.muted; font.pixelSize: 10 }
                                    }
                                    Button { text: "×"; flat: true; implicitWidth: 24; enabled: !window.interactionBusy; onClicked: converter.removeFile(index); Accessible.name: "Remover arquivo da seleção" }
                                }
                            }
                        }
                        DropArea {
                            id: listDrop
                            anchors.fill: parent
                            enabled: !window.interactionBusy
                            onEntered: function(drag) { if (drag.hasUrls && (drag.supportedActions & Qt.CopyAction)) drag.accept(Qt.CopyAction); else drag.accepted = false }
                            onPositionChanged: function(drag) { if (drag.supportedActions & Qt.CopyAction) drag.accept(Qt.CopyAction); else drag.accepted = false }
                            onDropped: function(drop) { if (!(drop.supportedActions & Qt.CopyAction)) { drop.accepted = false; return } window.addSelectedUrls(window.urlStrings(drop.urls)); drop.accept(Qt.CopyAction) }
                        }
                    }
                    ColumnLayout {
                        id: outputLocationSection
                        Layout.fillWidth: true; spacing: 8
                        readonly property bool original: converter.outputDirectory.length === 0
                        Text { text: "SALVAR EM"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.1; font.weight: Font.DemiBold }
                        Rectangle {
                            Layout.fillWidth: true; implicitHeight: 58
                            radius: 13; color: "#f7f1fb"
                            Accessible.role: Accessible.StaticText
                            Accessible.name: outputLocationSection.original ? "Local original. Na mesma pasta de cada arquivo."
                                : "Pasta escolhida: " + converter.outputDirectory
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 10; spacing: 10
                                Rectangle {
                                    Layout.preferredWidth: 32; Layout.preferredHeight: 32
                                    radius: 10; color: "#eaddf4"
                                    Image { anchors.centerIn: parent; width: 18; height: 18; sourceSize.width: 36; sourceSize.height: 36
                                        source: "qrc:/mandarina/assets/icons/output-folder.svg" }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 3
                                    Text {
                                        Layout.fillWidth: true; textFormat: Text.PlainText
                                        text: outputLocationSection.original ? "Local original" : "Pasta escolhida"
                                        font.pixelSize: 12; font.weight: Font.DemiBold; color: window.ink
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true; textFormat: Text.PlainText
                                        text: outputLocationSection.original ? "Na mesma pasta de cada arquivo" : converter.outputDirectory
                                        font.pixelSize: 10; color: window.muted; elide: Text.ElideMiddle
                                    }
                                }
                            }
                            HoverHandler { id: outputLocationHover }
                            CleanToolTip {
                                id: outputLocationTip
                                visible: outputLocationHover.hovered
                                heading: "Local de salvamento"
                                text: outputLocationSection.original ? "Cada cópia é salva junto ao respectivo arquivo original." : converter.outputDirectory
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true; spacing: 8
                            SoftButton {
                                objectName: "chooseOutputFolder"
                                Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0
                                text: "Escolher pasta"; implicitHeight: 34
                                leftPadding: 9; rightPadding: 9; font.pixelSize: 12
                                enabled: !window.interactionBusy
                                onClicked: { folderDialog.currentFolder = converter.outputFolderUrl; folderDialog.open() }
                            }
                            SoftButton {
                                id: originalLocationButton
                                objectName: "useOriginalLocation"
                                Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0
                                text: "Local original"; implicitHeight: 34
                                leftPadding: 9; rightPadding: 9; font.pixelSize: 12
                                enabled: !window.interactionBusy
                                Accessible.description: outputLocationSection.original ? "Selecionado. Salvar junto aos arquivos originais." : "Restaurar o salvamento junto aos arquivos originais."
                                contentItem: Text {
                                    text: originalLocationButton.text; font: originalLocationButton.font
                                    color: !originalLocationButton.enabled ? "#aaa2b5" : window.violet
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: 12
                                    color: originalLocationButton.down ? "#dfcbed" : outputLocationSection.original ? "#ece0f6" : originalLocationButton.hovered ? "#f1e8f8" : "#fffdfa"
                                    border.color: originalLocationButton.activeFocus ? "#a47bc4" : outputLocationSection.original ? "#d6bfe7" : "#e7ddeb"
                                    Behavior on color { ColorAnimation { duration: 140 } }
                                }
                                onClicked: converter.outputDirectory = ""
                            }
                        }
                    }
                    ColumnLayout {
                        visible: !window.editingMode && (window.imageOptions || window.pageImageOptions)
                        Layout.fillWidth: true
                        ConversionPreset {
                            id: qualityPreset
                            objectName: "qualityPreset"
                            Layout.fillWidth: true
                            visible: window.imageOptions
                            caption: "Qualidade"
                            symbol: "qrc:/mandarina/assets/icons/output-quality.svg"
                            model: [
                                {label: "90%", detail: "Alta qualidade", value: 90},
                                {label: "80%", detail: "Arquivo menor", value: 80},
                                {label: "100%", detail: "Qualidade máxima", value: 100}
                            ]
                            currentIndex: converter.quality === 80 ? 1 : converter.quality === 100 ? 2 : 0
                            onActivated: converter.quality = currentValue
                        }
                        ConversionPreset {
                            id: resolutionPreset
                            objectName: "resolutionPreset"
                            Layout.fillWidth: true
                            visible: window.pageImageOptions
                            caption: "Resolução"
                            symbol: "qrc:/mandarina/assets/icons/output-resolution.svg"
                            model: [
                                {label: "72 DPI", detail: "Mais leve", value: 72},
                                {label: "150 DPI", detail: "Equilibrada", value: 150},
                                {label: "300 DPI", detail: "Mais detalhes", value: 300}
                            ]
                            currentIndex: converter.dpi === 72 ? 0 : converter.dpi === 300 ? 2 : 1
                            onActivated: converter.dpi = currentValue
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 4
                Text { Layout.alignment: Qt.AlignHCenter; text: "Escolha o próximo formato"; color: window.ink; font.pixelSize: window.compact ? 19 : 21; font.weight: Font.DemiBold }
                Rectangle {
                    visible: window.toolsBridge !== null
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 6; Layout.bottomMargin: 6
                    implicitWidth: 238; implicitHeight: 36
                    radius: 16; color: "#eee5f4"
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 3; spacing: 3
                        SoftButton {
                            objectName: "modeConversion"
                            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                            implicitHeight: 30; text: "Conversão"; primary: !window.editingMode
                            enabled: !window.interactionBusy
                            Accessible.description: !window.editingMode ? "Selecionado" : "Mostrar formatos de conversão"
                            onClicked: window.setEditingMode(false)
                        }
                        SoftButton {
                            objectName: "modeEditing"
                            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1
                            implicitHeight: 30; text: "Edição"; primary: window.editingMode
                            enabled: !window.interactionBusy
                            Accessible.description: window.editingMode ? "Selecionado" : "Mostrar compressão, metadados e edição de fotos"
                            onClicked: window.setEditingMode(true)
                        }
                    }
                }
                Text { Layout.alignment: Qt.AlignHCenter; text: window.editingMode ? "Escolha uma ação para abrir as opções." : !window.wheelFormats.length && converter.formatsLoading ? "Carregando os formatos disponíveis…" : "Clique para converter. Ou arraste e solte no formato."; color: window.muted; font.pixelSize: 12 }
                Item {
                    id: wheel
                    objectName: "formatWheel"
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: window.compact ? 300 : 350
                    property int hovered: -1
                    property bool editingDrag: false
                    property real hoverAmount: 0
                    property real entrance: 0
                    readonly property bool crowded: window.wheelFormats.length > 8
                    readonly property var hoveredFormat: hovered >= 0 && hovered < window.wheelFormats.length && !window.interactionBusy ? window.wheelFormats[hovered] : null
                    readonly property real cx: width / 2
                    readonly property real cy: height / 2
                    readonly property real radius: Math.min(width / 2 - 12, height / 2 - 12, 204)
                    readonly property real inner: radius * 0.43
                    onHoveredChanged: {
                        hoverAnimation.restart()
                        wheelCanvas.requestPaint()
                        if (hovered >= 0 && hovered < window.wheelFormats.length && window.visible
                                && !window.interactionBusy && window.desktopBridge
                                && typeof window.desktopBridge.playWheelHoverSound === "function")
                            window.desktopBridge.playWheelHoverSound()
                    }
                    onHoverAmountChanged: wheelCanvas.requestPaint()
                    onEntranceChanged: wheelCanvas.requestPaint()
                    Component.onCompleted: entranceAnimation.start()
                    NumberAnimation { id: entranceAnimation; target: wheel; property: "entrance"; from: 0; to: 1; duration: 420; easing.type: Easing.OutCubic }
                    NumberAnimation { id: hoverAnimation; target: wheel; property: "hoverAmount"; from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic }
                    function targetAt(x, y) {
                        var dx = x - cx, dy = y - cy, distance = Math.sqrt(dx * dx + dy * dy)
                        var count = window.wheelFormats.length
                        if (!count || distance < inner || distance > radius) return -1
                        var step = 2 * Math.PI / count
                        var angle = (Math.atan2(dy, dx) + Math.PI / 2 + step / 2 + 2 * Math.PI) % (2 * Math.PI)
                        return Math.floor(angle / step) % count
                    }
                    function activate(index) {
                        if (index < 0 || index >= window.wheelFormats.length || window.interactionBusy) return
                        if (window.editingMode) {
                            window.openEditingAction(window.wheelFormats[index].id.slice(5), window.selectedUrls())
                        } else {
                            converter.selectedFormat = window.wheelFormats[index].id
                            if (converter.files.length) converter.start(); else fileDialog.open()
                        }
                    }
                    Rectangle {
                        visible: window.wheelFormats.length > 0
                        x: wheel.cx - wheel.radius; y: wheel.cy - wheel.radius + 9
                        width: wheel.radius * 2; height: width; radius: width / 2
                        color: "#63358b"; opacity: 0.065 * wheel.entrance
                        scale: 0.96 + 0.04 * wheel.entrance
                    }
                    Canvas {
                        id: wheelCanvas
                        anchors.fill: parent; antialiasing: true
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d"); ctx.reset()
                            var formats = window.wheelFormats, count = formats.length
                            if (!count) return
                            ctx.save()
                            ctx.translate(wheel.cx, wheel.cy)
                            var revealScale = 0.96 + 0.04 * wheel.entrance
                            ctx.scale(revealScale, revealScale)
                            ctx.globalAlpha = wheel.entrance
                            var halo = ctx.createRadialGradient(0, 0, wheel.radius - 6, 0, 0, wheel.radius + 11)
                            halo.addColorStop(0, "#e4d2f0")
                            halo.addColorStop(1, "rgba(245,239,249,0)")
                            ctx.beginPath(); ctx.arc(0, 0, wheel.radius + 11, 0, 2 * Math.PI)
                            ctx.fillStyle = halo; ctx.fill()
                            ctx.beginPath(); ctx.arc(0, 0, wheel.radius + 5, 0, 2 * Math.PI)
                            ctx.strokeStyle = "#dbc8e9"; ctx.lineWidth = 1; ctx.stroke()
                            var step = 2 * Math.PI / count
                            for (var i = 0; i < count; i++) {
                                var center = -Math.PI / 2 + i * step, gap = 0.024
                                var start = center - step / 2 + gap, end = center + step / 2 - gap
                                var selected = formats[i].id === window.selectedWheelId
                                var hover = i === wheel.hovered && !window.interactionBusy
                                var lift = hover ? wheel.hoverAmount * 3 : 0
                                ctx.save()
                                ctx.translate(Math.cos(center) * lift, Math.sin(center) * lift)
                                ctx.beginPath()
                                ctx.arc(0, 0, wheel.radius, start, end, false)
                                ctx.lineTo(wheel.inner * Math.cos(end), wheel.inner * Math.sin(end))
                                ctx.arc(0, 0, wheel.inner, end, start, true)
                                ctx.closePath()
                                var petal = ctx.createLinearGradient(0, -wheel.radius, 0, wheel.radius)
                                petal.addColorStop(0, hover ? "#a36cd1" : selected ? "#f3e8ff" : "#fffefb")
                                petal.addColorStop(1, hover ? "#63358b" : selected ? "#e1cdf2" : "#f5eff9")
                                ctx.fillStyle = petal; ctx.fill()
                                ctx.lineWidth = hover || selected ? 1.4 : 1
                                ctx.strokeStyle = hover ? "#7e49a7" : selected ? "#bb94d8" : "#dfd1e9"; ctx.stroke()
                                ctx.beginPath()
                                ctx.arc(0, 0, wheel.radius - 4, start + 0.018, end - 0.018, false)
                                ctx.lineWidth = hover ? 3 : selected ? 2.5 : 1
                                ctx.lineCap = "round"
                                ctx.strokeStyle = hover || selected ? "#f3c64d" : "#ffffff"
                                ctx.stroke()
                                ctx.restore()
                            }
                            ctx.restore()
                        }
                        opacity: converter.busy ? 0.55 : 1
                    }
                    Repeater {
                        model: window.wheelFormats
                        delegate: Column {
                            required property var modelData
                            required property int index
                            readonly property real angle: -Math.PI / 2 + index * 2 * Math.PI / window.wheelFormats.length
                            readonly property bool highlighted: index === wheel.hovered && !window.interactionBusy
                            readonly property bool selected: modelData.id === window.selectedWheelId
                            width: window.editingMode ? 136 : wheel.crowded ? 76 : 96; spacing: 3
                            opacity: wheel.entrance
                            scale: highlighted ? 1.07 : 1
                            Behavior on scale { NumberAnimation { duration: 170; easing.type: Easing.OutCubic } }
                            x: wheel.cx + ((wheel.radius + wheel.inner) / 2 + (highlighted ? 3 * wheel.hoverAmount : 0)) * Math.cos(angle) - width / 2
                            y: wheel.cy + ((wheel.radius + wheel.inner) / 2 + (highlighted ? 3 * wheel.hoverAmount : 0)) * Math.sin(angle) - height / 2
                            Text { width: parent.width; text: modelData.label; horizontalAlignment: Text.AlignHCenter; color: parent.highlighted ? "#fffdf5" : window.violet; font.pixelSize: wheel.crowded ? 14 : 16; font.weight: Font.DemiBold; Behavior on color { ColorAnimation { duration: 120 } } }
                            Text { visible: window.wheelFormats.length < 8; width: parent.width; text: modelData.detail; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter; color: parent.highlighted ? "#f1e2ff" : "#9982a8"; font.pixelSize: 10; Behavior on color { ColorAnimation { duration: 120 } } }
                            Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 5; height: 3; radius: 1.5; color: window.gold; opacity: parent.selected || parent.highlighted ? 1 : 0; Behavior on opacity { NumberAnimation { duration: 120 } } }
                        }
                    }
                    Rectangle {
                        visible: window.wheelFormats.length > 0
                        x: wheel.cx - wheel.inner - 1; y: wheel.cy - wheel.inner - 1
                        width: (wheel.inner + 1) * 2; height: width; radius: width / 2
                        color: "#f3c64d"
                        opacity: wheel.editingDrag || converter.dragPreview ? 0.24 : 0.09
                        scale: wheel.editingDrag || converter.dragPreview ? 1.025 : 1
                        Behavior on opacity { NumberAnimation { duration: 180 } }
                        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    }
                    Rectangle {
                        visible: window.wheelFormats.length > 0
                        x: wheel.cx - wheel.inner + 7; y: wheel.cy - wheel.inner + 7
                        width: (wheel.inner - 7) * 2; height: width; radius: width / 2
                        gradient: Gradient {
                            GradientStop { position: 0; color: "#fffef6" }
                            GradientStop { position: 1; color: "#f4eafa" }
                        }
                        border.color: "#e3d0ee"
                        Column {
                            anchors.centerIn: parent; spacing: 3
                            Image {
                                anchors.horizontalCenter: parent.horizontalCenter; width: 41; height: 41
                                source: "qrc:/mandarina/assets/org.winux.Mandarina.svg"
                                RotationAnimator on rotation { from: 0; to: 360; duration: 3200; loops: Animation.Infinite; running: converter.busy }
                            }
                            Text {
                                width: wheel.inner * 1.5; horizontalAlignment: Text.AlignHCenter
                                text: converter.busy ? Math.round(converter.progress * 100) + "%" : wheel.hoveredFormat ? wheel.hoveredFormat.label : wheel.editingDrag ? "Solte na\nação" : converter.dragPreview ? "Solte no\nformato" : converter.files.length ? converter.files.length + " arquivo(s)" : "Solte\narquivos"
                                color: window.ink; font.pixelSize: 12; font.weight: Font.Medium
                            }
                            Text {
                                visible: wheel.hoveredFormat !== null
                                width: wheel.inner * 1.5; horizontalAlignment: Text.AlignHCenter
                                text: wheel.hoveredFormat ? wheel.hoveredFormat.detail : ""
                                wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
                                color: window.muted; font.pixelSize: 10
                            }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent; hoverEnabled: true; enabled: !window.interactionBusy
                        cursorShape: wheel.hovered >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onPositionChanged: function(mouse) { wheel.hovered = wheel.targetAt(mouse.x, mouse.y); wheelCanvas.requestPaint() }
                        onExited: { wheel.hovered = -1; wheelCanvas.requestPaint() }
                        onClicked: function(mouse) { wheel.activate(wheel.targetAt(mouse.x, mouse.y)) }
                    }
                    DropArea {
                        anchors.fill: parent; enabled: !window.interactionBusy
                        onEntered: function(drag) {
                            var urls = window.urlStrings(drag.urls)
                            var valid = drag.hasUrls && (drag.supportedActions & Qt.CopyAction)
                                && (window.editingMode ? converter.canEditUrls(urls) : converter.previewUrls(urls))
                            if (valid) {
                                drag.accept(Qt.CopyAction)
                                wheel.editingDrag = window.editingMode
                                wheel.hovered = wheel.targetAt(drag.x, drag.y)
                                wheelCanvas.requestPaint()
                            } else drag.accepted = false
                        }
                        onPositionChanged: function(drag) {
                            if (!(drag.supportedActions & Qt.CopyAction)) { drag.accepted = false; return }
                            drag.accept(Qt.CopyAction)
                            wheel.hovered = wheel.targetAt(drag.x, drag.y); wheelCanvas.requestPaint()
                        }
                        onExited: { converter.clearDragPreview(); wheel.editingDrag = false; wheel.hovered = -1; wheelCanvas.requestPaint() }
                        onDropped: function(drop) {
                            if (!(drop.supportedActions & Qt.CopyAction)) { converter.clearDragPreview(); wheel.editingDrag = false; drop.accepted = false; return }
                            var index = wheel.targetAt(drop.x, drop.y)
                            var format = index >= 0 ? window.wheelFormats[index].id : ""
                            var urls = window.urlStrings(drop.urls)
                            if (window.editingMode) {
                                if (!converter.canEditUrls(urls)) { wheel.editingDrag = false; drop.accepted = false; return }
                                if (format) {
                                    if (format === "tool:photo") window.openEditingAction("photo", urls)
                                    else {
                                        converter.clearFiles()
                                        converter.addEditingUrls(urls)
                                        window.openEditingAction(format.slice(5), window.selectedUrls())
                                    }
                                } else converter.addEditingUrls(urls)
                            } else if (format) converter.dropOnFormat(urls, format)
                            else { converter.clearDragPreview(); converter.addUrls(urls) }
                            wheel.editingDrag = false
                            drop.accept(Qt.CopyAction); wheel.hovered = -1; wheelCanvas.requestPaint()
                        }
                    }
                    Text { anchors.centerIn: parent; width: 230; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; visible: !window.editingMode && window.wheelFormats.length === 0; text: converter.inputKind === "mixed" ? "Separe imagens, PDFs, documentos e áudios em conversões diferentes." : "Sem conversão para esta seleção. Use Edição para comprimir ou ver metadados."; color: window.muted }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter; Layout.maximumWidth: 510
                    text: window.editingMode ? "Arrastar + " + window.editingShortcutLabel.toUpperCase() + " também abre estas ações. Os originais são preservados."
                        : converter.inputKind === "unsupported" ? "Este tipo ainda não tem conversões. Edição permite comprimir ou consultar metadados."
                        : converter.inputKind === "pdf" ? (converter.selectedFormat === "pptx" ? "Um slide por página, como imagem; o texto dos slides não fica editável."
                            : converter.selectedFormat === "xlsx" ? "Extrai tabelas detectadas em PDFs com texto. Confira as células do resultado."
                            : converter.selectedFormat === "png" || converter.selectedFormat === "jpg" ? "Cada página vira uma imagem, na resolução escolhida."
                            : converter.selectedFormat === "pdf" ? "Reúne os PDFs na ordem da seleção, preservando os originais."
                            : "Preserva o texto existente e usa OCR local nas páginas sem texto. Revise o resultado; a diagramação pode mudar.")
                        : converter.inputKind === "document" ? (converter.selectedFormat === "png" || converter.selectedFormat === "jpg" ? "Cada página do documento vira uma imagem. Confira a paginação do resultado." : "Cada documento gera um novo arquivo. A formatação pode variar entre os formatos.")
                        : converter.inputKind === "audio" ? "Cada áudio gera um novo arquivo. Converter não recupera qualidade já perdida."
                        : converter.inputKind === "mixed" ? "A roda mostra os formatos compatíveis com a seleção."
                        : converter.selectedFormat === "txt" || converter.selectedFormat === "docx" ? "OCR local em português + inglês. O texto fica editável; revise o reconhecimento e a formatação."
                        : converter.selectedFormat === "pdf" ? "Várias fotos → um PDF, com uma imagem por página."
                        : "Arrastar o arquivo + " + window.editingShortcutLabel.toUpperCase() + " abre a roda de atalhos"
                    color: "#9a8fa5"; font.pixelSize: 11; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        Rectangle {
            readonly property string message: window.editingError || converter.errors.slice(0, 2).join("\n")
            Layout.fillWidth: true; implicitHeight: message.length ? 58 : 0; visible: message.length > 0
            radius: 12; color: "#fff0ef"; border.color: "#f2dbd8"
            Text { anchors.fill: parent; anchors.margins: 12; textFormat: Text.PlainText; text: parent.message; color: "#9d5a55"; font.pixelSize: 12; elide: Text.ElideRight; wrapMode: Text.WrapAnywhere }
        }
        RowLayout {
            visible: converter.busy || (!window.editingMode && converter.outputs.length > 0)
            Layout.fillWidth: true; spacing: 14
            ColumnLayout {
                Layout.fillWidth: true; spacing: 6
                Text { Layout.fillWidth: true; textFormat: Text.PlainText; text: converter.status; color: window.ink; font.pixelSize: 12; elide: Text.ElideMiddle }
                ProgressBar { Layout.fillWidth: true; visible: converter.busy; value: converter.progress; implicitHeight: 5 }
            }
            SoftButton { visible: !window.editingMode && converter.outputs.length > 0; text: "Abrir pasta"; onClicked: converter.openOutputFolder() }
            SoftButton { visible: converter.busy; text: "Cancelar"; onClicked: converter.cancel() }
        }
    }
}

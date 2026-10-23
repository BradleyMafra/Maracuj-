import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: panel
    objectName: "fileToolsPanel"
    property var controller: null
    property bool standalone: false
    property bool autoPresent: true
    property bool advancedExpanded: false
    property bool technicalExpanded: false
    property bool operationRequested: false
    readonly property int preferredStandaloneHeight: Math.max(340, Math.min(500, Math.ceil(mainLayout.implicitHeight + padding * 2 + 24)))
    readonly property bool hasEditableMetadata: metadataRows.some(function(row) { return row.editable || row.removable })
    readonly property int technicalCount: metadataRows.filter(function(row) { return !row.editable && !row.removable }).length
    signal moveRequested()
    readonly property bool working: controller ? controller.busy : false
    readonly property bool blocked: controller && controller.blocked !== undefined ? controller.blocked : false
    readonly property bool unavailable: working || blocked
    readonly property bool metadataMode: controller ? controller.action === "metadata" : false
    readonly property var fileRows: controller ? controller.files : []
    readonly property var metadataRows: controller ? controller.metadata : []
    readonly property string outputDirectory: controller && controller.outputDirectory !== undefined ? controller.outputDirectory : ""
    readonly property color ink: "#412c53"
    readonly property color muted: "#897792"
    readonly property color violet: "#63358b"
    readonly property double targetValue: Number(targetInput.text.replace(",", "."))
    readonly property bool targetValid: !targetEnabled.checked || (isFinite(targetValue) && targetValue > 0
        && (targetUnit.currentIndex === 0 ? targetValue <= 1048576 : targetValue < 100))
    property string pendingAction: ""
    property int pendingFile: -1
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - (standalone ? 24 : 32) : 760, 760)
    height: Math.min(parent ? parent.height - (standalone ? 24 : 32) : 720, 720)
    padding: standalone ? 18 : width < 560 ? 18 : 26
    modal: true
    focus: true
    closePolicy: working ? Popup.NoAutoClose : Popup.CloseOnEscape
    background: Rectangle { radius: panel.standalone ? 20 : 24; color: "#fffdf8"; border.color: "#ddcce9" }
    Overlay.modal: Rectangle { color: panel.standalone ? "transparent" : "#69412c53" }

    function requestAction(action) {
        if (!controller || unavailable || controller.action === action) return
        if (controller.metadataDirty) {
            pendingAction = action
            pendingFile = -1
            discardDraft.open()
        } else controller.setAction(action)
    }
    function urlStrings(urls) {
        var result = []
        for (var index = 0; index < urls.length; ++index) result.push(urls[index].toString())
        return result
    }
    function requestFile(index) {
        if (!controller || unavailable || index === controller.currentFile) return
        if (controller.metadataDirty) {
            pendingAction = ""
            pendingFile = index
            discardDraft.open()
        } else controller.inspect(index)
    }
    function startCompression() {
        if (!controller || unavailable || !fileRows.length || !targetValid) return
        operationRequested = true
        controller.compress(preset.currentIndex === 0 ? "balanced" : "strong",
            archiveMode.currentIndex === 0 ? "auto" : "zip",
            dimensions.currentIndex === 1 ? 2048 : dimensions.currentIndex === 2 ? 1280 : 0,
            targetEnabled.checked && targetUnit.currentIndex === 1 ? targetValue : 0,
            targetEnabled.checked && targetUnit.currentIndex === 0 ? Math.round(targetValue * 1048576) : 0)
    }
    function filteredMetadata() {
        var query = metadataSearch.text.trim().toLocaleLowerCase()
        return metadataRows.filter(function(row) {
            if (panel.standalone && panel.hasEditableMetadata && !panel.technicalExpanded && !query.length
                    && !row.editable && !row.removable) return false
            return !query.length || [row.group || "", row.label || row.name || "", row.key || "", row.value || ""]
                .join(" ").toLocaleLowerCase().indexOf(query) >= 0
        })
    }
    function detailText(detail) {
        var note = detail.message || detail.note || ""
        var name = detail.name || (detail.output ? String(detail.output).split(/[\\/]/).pop() : "")
        var text = note
        if (detail.originalBytes !== undefined && detail.outputBytes !== undefined) {
            text = (name ? name + " · " : "")
                + (Number(detail.originalBytes) / 1048576).toFixed(2) + " MB → "
                + (Number(detail.outputBytes) / 1048576).toFixed(2) + " MB"
            if (detail.savedPercent !== undefined) text += " · " + Math.abs(Number(detail.savedPercent)).toFixed(1)
                + (Number(detail.savedPercent) >= 0 ? "% menor" : "% maior")
            if (detail.targetMet === false) text += " · A meta solicitada não foi atingida."
            if (note) text += "\n" + note
        }
        return text
    }
    Connections {
        target: panel.controller
        function onPanelRequested() { if (panel.autoPresent) panel.open() }
        function onFilesChanged() {
            removeAll.checked = false; metadataSearch.clear()
            panel.advancedExpanded = false; panel.technicalExpanded = false; panel.operationRequested = false
        }
        function onMetadataChanged() {
            if (panel.controller && !panel.controller.metadataDirty) removeAll.checked = false
        }
    }

    component SoftButton: Button {
        id: button
        property bool primary: false
        property bool quiet: false
        implicitHeight: panel.standalone ? 32 : 39
        leftPadding: 15; rightPadding: 15
        font.pixelSize: panel.standalone ? 12 : 13; font.weight: Font.DemiBold
        contentItem: Text { textFormat: Text.PlainText;
            text: button.text; font: button.font
            color: !button.enabled ? "#aaa2b5" : button.primary ? "white" : panel.ink
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 9; border.width: button.primary || button.quiet ? 0 : 1; border.color: "#e7ddeb"
            color: button.quiet ? (button.hovered ? "#f2edf6" : "transparent") : !button.enabled ? "#ece7ef" : button.primary ? (button.down ? "#512773" : button.hovered ? "#9458c4" : panel.violet)
                : button.hovered ? "#f0e7f7" : "#fffdfa"
        }
    }
    component CompactCombo: ComboBox {
        id: combo
        implicitHeight: 32
        font.pixelSize: 12
        leftPadding: 10; rightPadding: 27
        contentItem: Text {
            textFormat: Text.PlainText; text: combo.displayText; font: combo.font
            verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
            color: combo.enabled ? panel.ink : panel.muted
        }
        indicator: Text {
            text: "⌄"; color: panel.muted; font.pixelSize: 17
            x: combo.width - width - 10; y: (combo.height - height) / 2 - 2
        }
        background: Rectangle { radius: 8; color: "#f6f2f8"; border.color: combo.activeFocus ? panel.violet : "#eae3ef"; opacity: combo.enabled ? 1 : 0.55 }
    }
    component CompactField: TextField {
        id: field
        implicitHeight: 32
        font.pixelSize: 12; leftPadding: 10; rightPadding: 10
        color: panel.ink; placeholderTextColor: "#a195aa"
        selectByMouse: true
        background: Rectangle { radius: 8; color: "#ffffff"; border.color: field.activeFocus ? panel.violet : "#e7dfea" }
    }
    component QuietCheck: CheckBox {
        id: check
        implicitHeight: 28
        spacing: 7
        indicator: Rectangle {
            implicitWidth: 16; implicitHeight: 16
            x: check.leftPadding; y: (check.height - height) / 2
            radius: 4; color: check.checked ? panel.violet : "transparent"
            border.color: check.activeFocus ? panel.violet : check.checked ? panel.violet : "#cfc3d9"
            opacity: check.enabled ? 1 : 0.45
            Text { textFormat: Text.PlainText; anchors.centerIn: parent; text: check.checked ? "✓" : ""; font.pixelSize: 12; color: "white" }
        }
        contentItem: Text {
            textFormat: Text.PlainText; text: check.text; font.pixelSize: 12
            leftPadding: check.indicator.width + check.spacing
            verticalAlignment: Text.AlignVCenter; color: check.enabled ? panel.ink : panel.muted
        }
    }
    component ProfileButton: Button {
        id: choice
        property bool selected: false
        implicitHeight: 32
        font.pixelSize: 12; font.weight: selected ? Font.DemiBold : Font.Normal
        contentItem: Text { textFormat: Text.PlainText; text: choice.text; font: choice.font; color: choice.selected ? panel.violet : panel.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 7; color: choice.selected ? "white" : "transparent"; border.color: choice.selected ? "#e4d9ed" : "transparent" }
        Accessible.checkable: true
        Accessible.checked: selected
    }
    FileDialog {
        id: chooseFiles
        title: "Escolher arquivos para as ferramentas"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Todos os arquivos (*)"]
        onAccepted: if (panel.controller && !panel.unavailable) panel.controller.addUrls(panel.urlStrings(selectedFiles))
    }
    Dialog {
        id: discardDraft
        objectName: "fileToolsDiscardDraft"
        parent: panel.parent
        anchors.centerIn: parent
        width: Math.min(parent ? parent.width - 54 : 400, 400)
        padding: 23; modal: true; focus: true
        title: "Descartar alterações?"
        standardButtons: Dialog.Discard | Dialog.Cancel
        contentItem: Text { textFormat: Text.PlainText;
            text: "Há metadados editados que ainda não foram salvos em uma cópia. Deseja descartá-los?"
            color: panel.ink; wrapMode: Text.WordWrap
        }
        onDiscarded: {
            if (!panel.controller || panel.unavailable) return
            if (panel.pendingFile >= 0) panel.controller.inspect(panel.pendingFile)
            else panel.controller.setAction(panel.pendingAction)
        }
    }
    contentItem: ColumnLayout {
        id: mainLayout
        spacing: panel.standalone ? 11 : 14
        Item {
            Layout.fillWidth: true
            implicitHeight: panel.standalone ? 32 : 43
            Rectangle {
                id: headerMark
                width: panel.standalone ? 29 : 43; height: width
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                radius: 9; color: "#f3eafa"
                Text { textFormat: Text.PlainText; anchors.centerIn: parent; text: "✦"; font.pixelSize: panel.standalone ? 20 : 26; color: panel.violet }
            }
            Column {
                anchors.left: headerMark.right; anchors.leftMargin: 9
                anchors.right: headerClose.left; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Text {
                    textFormat: Text.PlainText
                    text: panel.standalone ? (panel.metadataMode ? "Metadados" : "Comprimir") : "Ferramentas do arquivo"
                    color: panel.ink; font.pixelSize: panel.standalone ? 18 : 22; font.weight: Font.DemiBold
                }
                Text { visible: !panel.standalone; textFormat: Text.PlainText; text: "Local, simples e com os originais preservados."; color: panel.muted; font.pixelSize: 11 }
            }
            MouseArea {
                anchors.left: headerMark.right; anchors.right: headerClose.left
                anchors.top: parent.top; anchors.bottom: parent.bottom
                enabled: panel.standalone; onPressed: panel.moveRequested()
            }
            SoftButton {
                id: headerClose
                objectName: "fileToolsClose"; text: "×"; quiet: panel.standalone
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 28; leftPadding: 4; rightPadding: 4; font.pixelSize: 18
                enabled: !panel.working; onClicked: panel.close(); Accessible.name: "Fechar ferramentas"
            }
        }
        RowLayout {
            visible: !panel.standalone
            Layout.fillWidth: true; spacing: 8
            SoftButton { objectName: "fileToolsCompressTab"; text: "Comprimir"; primary: !panel.metadataMode; Layout.fillWidth: true; enabled: !panel.unavailable; onClicked: panel.requestAction("compress") }
            SoftButton { objectName: "fileToolsMetadataTab"; text: "Metadados"; primary: panel.metadataMode; Layout.fillWidth: true; enabled: !panel.unavailable; onClicked: panel.requestAction("metadata") }
        }
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 46; radius: 10; color: "#f6f2f8"
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 11; anchors.rightMargin: 6; spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    Text {
                        textFormat: Text.PlainText; Layout.fillWidth: true; color: panel.ink
                        font.pixelSize: 12; font.weight: Font.Medium; elide: Text.ElideMiddle
                        text: !panel.fileRows.length ? "Escolha um arquivo" : panel.fileRows.length === 1 ? panel.fileRows[0].name : panel.fileRows.length + " arquivos"
                    }
                    Text {
                        textFormat: Text.PlainText; Layout.fillWidth: true; color: panel.muted; font.pixelSize: 10
                        text: panel.fileRows.length === 1 ? panel.fileRows[0].size : "Os originais ficam intactos"
                    }
                }
                SoftButton { objectName: "fileToolsChooseFiles"; text: "Trocar"; quiet: true; enabled: !panel.unavailable; onClicked: chooseFiles.open(); Accessible.name: "Escolher arquivos" }
            }
        }
        Text {
            objectName: "fileToolsBlockedStatus"
            visible: panel.blocked
            Layout.fillWidth: true; textFormat: Text.PlainText
            text: "Aguarde a outra operação para continuar."
            color: panel.muted; font.pixelSize: 11; wrapMode: Text.WordWrap
        }
        ScrollView {
            id: bodyScroll
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.preferredHeight: bodyContent.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: bodyContent
                width: bodyScroll.availableWidth; spacing: 11
                ColumnLayout {
                    visible: !panel.metadataMode; Layout.fillWidth: true; spacing: 9
                    Text { textFormat: Text.PlainText; text: "Perfil de compressão"; color: panel.muted; font.pixelSize: 11 }
                    CompactCombo {
                        id: preset; objectName: "fileToolsCompressionPreset"; Layout.fillWidth: true; enabled: !panel.unavailable
                        visible: !panel.standalone
                        model: ["Equilibrado · preservar mais qualidade", "Forte · reduzir mais o tamanho"]
                    }
                    Rectangle {
                        visible: panel.standalone; Layout.fillWidth: true; implicitHeight: 38; radius: 9; color: "#f3edf7"
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 3; spacing: 3
                            ProfileButton { text: "Equilibrado"; Layout.fillWidth: true; Layout.preferredWidth: 1; selected: preset.currentIndex === 0; enabled: !panel.unavailable; onClicked: preset.currentIndex = 0 }
                            ProfileButton { text: "Forte"; Layout.fillWidth: true; Layout.preferredWidth: 1; selected: preset.currentIndex === 1; enabled: !panel.unavailable; onClicked: preset.currentIndex = 1 }
                        }
                    }
                    Text {
                        visible: panel.standalone; textFormat: Text.PlainText; font.pixelSize: 10; color: panel.muted
                        text: preset.currentIndex === 0 ? "Menos perda de qualidade" : "Maior redução de tamanho"
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        Text { textFormat: Text.PlainText; text: "Saída"; color: panel.ink; font.pixelSize: 12; Layout.preferredWidth: 64 }
                        CompactCombo {
                            id: archiveMode; objectName: "fileToolsCompressionMode"; Layout.fillWidth: true; enabled: !panel.unavailable
                            model: ["Automático", "Arquivo ZIP"]
                            Accessible.name: "Tipo de saída"
                            CleanToolTip {
                                visible: archiveMode.hovered
                                text: "Automático: otimiza imagens e PDFs; outros arquivos viram ZIP."
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        Text { textFormat: Text.PlainText; text: "Imagens"; color: panel.ink; font.pixelSize: 12; Layout.preferredWidth: 64 }
                        CompactCombo {
                            id: dimensions; objectName: "fileToolsImageDimensions"; Layout.fillWidth: true
                            enabled: !panel.unavailable && archiveMode.currentIndex === 0
                            model: ["Tamanho original", "Até 2048 px", "Até 1280 px"]
                            Accessible.name: "Dimensões das imagens"
                        }
                    }
                    SoftButton {
                        objectName: "fileToolsAdvancedToggle"; visible: panel.standalone
                        quiet: true; text: (panel.advancedExpanded ? "⌃  " : "⌄  ") + "Mais opções" + (targetEnabled.checked ? " · meta ativa" : "")
                        leftPadding: 0; enabled: !panel.unavailable
                        onClicked: panel.advancedExpanded = !panel.advancedExpanded
                        Accessible.name: panel.advancedExpanded ? "Recolher opções avançadas" : "Mostrar opções avançadas"
                    }
                    ColumnLayout {
                        visible: !panel.standalone || panel.advancedExpanded
                        Layout.fillWidth: true; spacing: 9
                        QuietCheck {
                            id: targetEnabled; objectName: "fileToolsTargetEnabled"; text: "Definir uma meta de tamanho"
                            enabled: !panel.unavailable && archiveMode.currentIndex === 0
                            onEnabledChanged: if (!enabled && !panel.unavailable) checked = false
                        }
                        RowLayout {
                            Layout.fillWidth: true; visible: targetEnabled.checked
                            CompactCombo { id: targetUnit; objectName: "fileToolsTargetUnit"; Layout.fillWidth: true; model: ["Tamanho em MB", "Redução em %"]; enabled: !panel.unavailable; Accessible.name: "Unidade da meta" }
                            CompactField {
                                id: targetInput; objectName: "fileToolsTargetValue"; Layout.preferredWidth: 78
                                text: "1"; enabled: !panel.unavailable
                                inputMethodHints: Qt.ImhFormattedNumbersOnly
                                validator: DoubleValidator { bottom: 0.001; top: 1048576; decimals: 3; notation: DoubleValidator.StandardNotation }
                                Accessible.name: "Valor da meta"
                            }
                        }
                        Text {
                            textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: 10
                            color: panel.targetValid ? panel.muted : "#a34954"
                            text: !panel.targetValid ? "Informe um valor positivo; a redução deve ser menor que 100%."
                                : "A meta é uma tentativa. O formato e o conteúdo podem impedir a redução."
                        }
                    }
                }
                ColumnLayout {
                    visible: panel.metadataMode; Layout.fillWidth: true; spacing: 9
                    RowLayout {
                        visible: !panel.standalone || panel.fileRows.length > 1
                        Layout.fillWidth: true
                        CompactCombo {
                            objectName: "fileToolsMetadataFile"; Layout.fillWidth: true
                            model: panel.fileRows; textRole: "name"
                            currentIndex: panel.controller ? panel.controller.currentFile : -1
                            enabled: !panel.unavailable && panel.fileRows.length > 0
                            onActivated: panel.requestFile(currentIndex)
                            Accessible.name: "Arquivo para inspecionar"
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 5
                        CompactField {
                            id: metadataSearch; objectName: "fileToolsMetadataSearch"; Layout.fillWidth: true
                            visible: !panel.standalone || panel.hasEditableMetadata || panel.metadataRows.length > 5
                            placeholderText: "Buscar metadados"; enabled: !panel.unavailable
                            Accessible.name: "Buscar campo, grupo ou valor"
                        }
                        Text {
                            visible: !metadataSearch.visible; textFormat: Text.PlainText
                            text: "Informações do arquivo"; color: panel.muted; font.pixelSize: 11; Layout.fillWidth: true
                        }
                        SoftButton {
                            id: refreshMetadataButton
                            objectName: "fileToolsRefreshMetadata"; text: "↻"; quiet: true; font.pixelSize: 16; leftPadding: 9; rightPadding: 9
                            enabled: !panel.unavailable && panel.fileRows.length > 0 && !(panel.controller && panel.controller.metadataDirty)
                            onClicked: if (panel.controller) panel.controller.inspect(Math.max(0, panel.controller.currentFile))
                            Accessible.name: "Reler metadados"
                            CleanToolTip {
                                visible: refreshMetadataButton.hovered
                                text: "Reler metadados"
                            }
                        }
                    }
                    RowLayout {
                        visible: !panel.standalone || panel.hasEditableMetadata
                        Layout.fillWidth: true
                        QuietCheck {
                            id: removeAll; objectName: "fileToolsRemoveAllMetadata"; text: "Limpar metadados"
                            enabled: !panel.unavailable && panel.metadataRows.length > 0
                            onToggled: if (panel.controller) panel.controller.removeAllMetadata(checked)
                        }
                        Item { Layout.fillWidth: true }
                        SoftButton {
                            objectName: "fileToolsTechnicalToggle"; visible: panel.standalone && panel.technicalCount > 0
                            text: panel.technicalExpanded ? "− Detalhes" : "+ Detalhes"; quiet: true; leftPadding: 7; rightPadding: 7
                            enabled: !panel.unavailable
                            onClicked: panel.technicalExpanded = !panel.technicalExpanded
                            Accessible.name: panel.technicalExpanded ? "Recolher informações técnicas" : "Mostrar informações técnicas"
                        }
                    }
                    Text {
                        visible: removeAll.checked; textFormat: Text.PlainText
                        Layout.fillWidth: true; wrapMode: Text.WordWrap; color: panel.muted; font.pixelSize: 10
                        text: "Limpa os metadados suportados na cópia. Orientação e cores são mantidas; o conteúdo do arquivo não é apagado."
                    }
                    Repeater {
                        id: metadataRepeater
                        model: panel.filteredMetadata()
                        delegate: ColumnLayout {
                            id: metadataRow
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true; spacing: 5
                            Text {
                                textFormat: Text.PlainText; Layout.fillWidth: true
                                visible: metadataRow.index === 0 || (metadataRepeater.model[metadataRow.index - 1].group || "") !== (metadataRow.modelData.group || "")
                                text: metadataRow.modelData.group || "Metadados"
                                color: panel.violet; font.pixelSize: 10; font.weight: Font.DemiBold
                                topPadding: metadataRow.index === 0 ? 0 : 7
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                ColumnLayout {
                                    visible: !!metadataRow.modelData.editable
                                    Layout.fillWidth: true; spacing: 4
                                    Text {
                                        textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                                        color: panel.muted; font.pixelSize: 10
                                        text: metadataRow.modelData.label || metadataRow.modelData.name || metadataRow.modelData.key
                                    }
                                    CompactField {
                                        objectName: "fileToolsMetadataEdit_" + metadataRow.modelData.key
                                        Layout.fillWidth: true
                                        text: metadataRow.modelData.draft === undefined ? String(metadataRow.modelData.value || "") : String(metadataRow.modelData.draft)
                                        enabled: !panel.unavailable && !metadataRow.modelData.removed
                                        maximumLength: 4096
                                        onTextEdited: if (panel.controller) panel.controller.editMetadata(metadataRow.modelData.key, text)
                                        Accessible.name: metadataRow.modelData.label || metadataRow.modelData.key
                                    }
                                }
                                RowLayout {
                                    visible: !metadataRow.modelData.editable
                                    Layout.fillWidth: true; spacing: 10
                                    Text {
                                        textFormat: Text.PlainText; Layout.preferredWidth: 105
                                        text: metadataRow.modelData.label || metadataRow.modelData.name || metadataRow.modelData.key
                                        color: panel.muted; font.pixelSize: 11; wrapMode: Text.WrapAnywhere
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true; readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText
                                        text: String(metadataRow.modelData.value === undefined ? "" : metadataRow.modelData.value)
                                        color: panel.ink; font.pixelSize: 12; wrapMode: TextEdit.WrapAnywhere
                                        opacity: metadataRow.modelData.removed ? 0.45 : 1
                                    }
                                }
                                SoftButton {
                                    objectName: "fileToolsMetadataRemove_" + metadataRow.modelData.key
                                    text: checked ? "↶" : "−"; checkable: true; checked: !!metadataRow.modelData.removed
                                    quiet: !checked; primary: checked
                                    implicitWidth: 25; implicitHeight: 25; leftPadding: 6; rightPadding: 6
                                    font.pixelSize: 16
                                    visible: !!metadataRow.modelData.removable
                                    enabled: !panel.unavailable && !removeAll.checked
                                    onToggled: if (panel.controller) panel.controller.removeMetadata(metadataRow.modelData.key, checked)
                                    Accessible.name: (checked ? "Restaurar " : "Remover ") + (metadataRow.modelData.label || metadataRow.modelData.key)
                                    CleanToolTip {
                                        visible: parent.hovered
                                        text: parent.checked ? "Restaurar este campo" : "Remover este campo na cópia"
                                    }
                                }
                            }
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#f0eaf3" }
                        }
                    }
                    Text {
                        textFormat: Text.PlainText; Layout.fillWidth: true; visible: metadataRepeater.count === 0
                        text: panel.working ? "Lendo metadados…" : panel.metadataRows.length ? "Nenhum campo corresponde à busca." : "Escolha um arquivo para inspecionar."
                        color: panel.muted; font.pixelSize: 11; wrapMode: Text.WordWrap
                    }
                }
                ColumnLayout {
                    visible: !panel.standalone || panel.operationRequested || (panel.controller && (panel.controller.errors.length > 0 || panel.controller.outputs.length > 0))
                    Layout.fillWidth: true; spacing: 8
                    Text {
                        textFormat: Text.PlainText; objectName: "fileToolsStatus"; Layout.fillWidth: true
                        text: panel.controller ? panel.controller.status : "Selecione arquivos. Os originais serão preservados."
                        color: panel.ink; wrapMode: Text.WordWrap; font.pixelSize: 11
                    }
                    Repeater {
                        model: panel.controller ? panel.controller.errors : []
                        delegate: Text { textFormat: Text.PlainText; required property string modelData; Layout.fillWidth: true; text: modelData; color: "#a34954"; wrapMode: Text.WordWrap; font.pixelSize: 11 }
                    }
                    Repeater {
                        model: panel.controller ? panel.controller.details : []
                        delegate: Text { textFormat: Text.PlainText; required property var modelData; Layout.fillWidth: true; text: panel.detailText(modelData); visible: text.length > 0; color: panel.muted; wrapMode: Text.WordWrap; font.pixelSize: 10 }
                    }
                    Repeater {
                        model: panel.controller ? panel.controller.outputs : []
                        delegate: Text {
                            textFormat: Text.PlainText; required property string modelData; Layout.fillWidth: true
                            text: "✓ " + modelData; color: panel.violet; wrapMode: Text.WrapAnywhere; font.pixelSize: 10
                            Accessible.name: "Cópia criada: " + modelData
                        }
                    }
                }
            }
        }
        ProgressBar { Layout.fillWidth: true; visible: panel.working; value: panel.controller ? panel.controller.progress : 0; indeterminate: value === 0 }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#ede6f1" }
        RowLayout {
            Layout.fillWidth: true; spacing: 7
            Text {
                textFormat: Text.PlainText; Layout.fillWidth: true; font.pixelSize: 10; color: panel.muted
                text: panel.working ? "Processando…" : panel.metadataMode && !panel.hasEditableMetadata ? "Somente leitura"
                    : panel.outputDirectory.length ? "Na pasta escolhida" : "No local original"
                visible: !(panel.controller && panel.controller.outputs.length > 0) && !panel.unavailable
                elide: Text.ElideRight
            }
            SoftButton {
                objectName: "fileToolsOutputFolder"; text: "Abrir pasta"
                visible: panel.controller && panel.controller.outputs.length > 0
                enabled: !panel.working; onClicked: if (panel.controller) panel.controller.openOutputFolder()
            }
            Item { visible: panel.working || (panel.controller && panel.controller.outputs.length > 0); Layout.fillWidth: true }
            SoftButton { objectName: "fileToolsCancel"; text: "Cancelar operação"; visible: panel.working; onClicked: if (panel.controller) panel.controller.cancel() }
            SoftButton {
                objectName: "fileToolsRun"; primary: true
                text: panel.metadataMode ? "Salvar cópia" : "Comprimir"
                visible: !panel.standalone || !panel.metadataMode || panel.hasEditableMetadata || panel.working
                enabled: !!panel.controller && !panel.unavailable && panel.fileRows.length > 0
                    && (panel.metadataMode ? panel.controller.metadataDirty : panel.targetValid)
                onClicked: {
                    if (panel.unavailable) return
                    if (panel.metadataMode) { panel.operationRequested = true; panel.controller.saveMetadata() }
                    else panel.startCompression()
                }
            }
            SoftButton {
                text: "Fechar"; visible: panel.standalone && panel.metadataMode && !panel.hasEditableMetadata && !panel.working
                onClicked: panel.close()
            }
        }
    }
}

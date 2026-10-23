#include "converter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTest>
#include <cstdio>
#include <functional>

class ShortcutBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString status READ status CONSTANT)
    Q_PROPERTY(QString shortcut READ shortcut NOTIFY changed)
    Q_PROPERTY(QString shortcutLabel READ shortcut NOTIFY changed)
    Q_PROPERTY(QString toolsShortcut READ toolsShortcut NOTIFY changed)
    Q_PROPERTY(QString toolsShortcutLabel READ toolsShortcut NOTIFY changed)
    Q_PROPERTY(QString shortcutError READ shortcutError CONSTANT)
    Q_PROPERTY(bool dragRequired READ dragRequired NOTIFY changed)
    Q_PROPERTY(int wheelTransparency READ wheelTransparency NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(int wheelSize READ wheelSize NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(QString wheelColor READ wheelColor NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(bool wheelSoundEnabled READ wheelSoundEnabled NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(QString wheelAppearanceError READ wheelAppearanceError NOTIFY wheelAppearanceChanged)
public:
    bool available() const { return true; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool value) { m_enabled = value; emit changed(); }
    QString status() const { return QStringLiteral("Teste local"); }
    QString shortcut() const { return m_shortcut; }
    QString toolsShortcut() const { return m_toolsShortcut; }
    QString shortcutError() const { return {}; }
    bool dragRequired() const { return m_dragRequired; }
    int wheelTransparency() const { return m_wheelTransparency; }
    int wheelSize() const { return m_wheelSize; }
    QString wheelColor() const { return m_wheelColor; }
    bool wheelSoundEnabled() const { return m_wheelSoundEnabled; }
    QString wheelAppearanceError() const { return m_wheelAppearanceError; }
    int appearanceSaves = 0;
    int hoverSounds = 0;
    bool failAppearanceSave = false;
    int saves = 0;
    int toolsSaves = 0;
    int capturedKeys = 0;
    bool captureActive = false;
    int captureStarts = 0;
    int captureStops = 0;

    Q_INVOKABLE void setShortcutCaptureActive(bool active)
    {
        captureActive = active;
        if (active) ++captureStarts; else ++captureStops;
    }

    Q_INVOKABLE QString shortcutFromKey(int key, int modifiers)
    {
        ++capturedKeys;
        if (key == Qt::Key_Shift) return QStringLiteral("Shift");
        if (key == Qt::Key_Control) return QStringLiteral("Ctrl");
        if (key == Qt::Key_Alt) return QStringLiteral("Alt");
        return QKeySequence(QKeyCombination(Qt::KeyboardModifiers(modifiers), Qt::Key(key)))
            .toString(QKeySequence::PortableText);
    }
    Q_INVOKABLE QString shortcutDisplay(const QString &value) const { return value; }
    Q_INVOKABLE QString validateShortcut(const QString &value, bool requireDrag) const
    {
        if (value.isEmpty()) return QStringLiteral("Escolha um atalho.");
        if (value == m_toolsShortcut) return QStringLiteral("Escolha atalhos diferentes para conversão e ferramentas.");
        if (!requireDrag && (value == QStringLiteral("Shift") || value == QStringLiteral("Ctrl") || value == QStringLiteral("Alt")))
            return QStringLiteral("Inclua uma tecla para abrir a roda sem arraste.");
        return {};
    }
    Q_INVOKABLE bool setActivation(const QString &value, bool requireDrag)
    {
        if (!validateShortcut(value, requireDrag).isEmpty() || value == m_toolsShortcut) return false;
        m_shortcut = value;
        m_dragRequired = requireDrag;
        ++saves;
        emit changed();
        return true;
    }
    Q_INVOKABLE QString validateToolsShortcut(const QString &value) const
    {
        if (value.isEmpty()) return QStringLiteral("Escolha um atalho.");
        if (value == m_shortcut) return QStringLiteral("Escolha atalhos diferentes para conversão e ferramentas.");
        return {};
    }
    Q_INVOKABLE bool setToolsActivation(const QString &value)
    {
        if (!validateToolsShortcut(value).isEmpty()) return false;
        m_toolsShortcut = value;
        ++toolsSaves;
        emit changed();
        return true;
    }
    Q_INVOKABLE void Quit() {}
    Q_INVOKABLE void playWheelHoverSound()
    {
        if (m_wheelSoundEnabled) ++hoverSounds;
    }
    Q_INVOKABLE bool setWheelAppearance(int percent, int size, const QString &color)
    {
        return setWheelAppearance(percent, size, color, m_wheelSoundEnabled);
    }
    Q_INVOKABLE bool setWheelAppearance(int percent, int size, const QString &color, bool soundEnabled)
    {
        static const QRegularExpression validColor(QStringLiteral("^#[0-9a-f]{6}$"));
        if (failAppearanceSave || percent < 0 || percent > 80 || size < 70 || size > 130 || !validColor.match(color).hasMatch()) {
            m_wheelAppearanceError = QStringLiteral("Não foi possível salvar a aparência.");
            emit wheelAppearanceChanged();
            return false;
        }
        m_wheelTransparency = percent;
        m_wheelSize = size;
        m_wheelColor = color;
        m_wheelSoundEnabled = soundEnabled;
        m_wheelAppearanceError.clear();
        ++appearanceSaves;
        emit wheelAppearanceChanged();
        return true;
    }
signals:
    void changed();
    void wheelAppearanceChanged();
private:
    int m_wheelTransparency = 35;
    int m_wheelSize = 100;
    QString m_wheelColor = QStringLiteral("#b99cde");
    bool m_wheelSoundEnabled = true;
    QString m_wheelAppearanceError;
    QString m_shortcut = QStringLiteral("Shift");
    QString m_toolsShortcut = QStringLiteral("Alt");
    bool m_dragRequired = true;
    bool m_enabled = true;
};

namespace {
QQuickItem *findVisual(QQuickItem *root, const QString &name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = findVisual(child, name)) return found;
    return nullptr;
}

bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "qml_shortcut_check: %s\n", message);
    return condition;
}
bool invoke(QObject *object, const char *method)
{
    const bool result = object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection);
    QCoreApplication::processEvents();
    return result;
}
bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 2000) QTest::qWait(10);
    return condition();
}
bool preview(QQuickWindow *window, const QString &name)
{
    const QString directory = qEnvironmentVariable("MARACUJA_SHORTCUT_PREVIEW_DIR");
    if (directory.isEmpty()) return true;
    if (!QDir().mkpath(directory)) return false;
    window->requestUpdate();
    QTest::qWait(80);
    const QImage image = window->grabWindow();
    return !image.isNull() && image.save(QDir(directory).filePath(name));
}
}

int main(int argc, char **argv)
{
    // Synthetic input is delivered only to this test's own offscreen window.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    Converter converter;
    ShortcutBridge bridge;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("converter"), &converter);
    engine.rootContext()->setContextProperty(QStringLiteral("compactMode"), false);
    engine.rootContext()->setContextProperty(QStringLiteral("desktopIntegration"), &bridge);
    engine.load(QUrl(QStringLiteral("qrc:/mandarina/qml/Main.qml")));
    if (!check(!engine.rootObjects().isEmpty(), "a interface não carregou")) return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!check(window != nullptr, "a interface não criou sua janela")) return 1;
    window->show();
    QTest::qWait(50);
    auto *dialog = window->findChild<QObject *>(QStringLiteral("gestureShortcutDialog"));
    auto *capture = window->findChild<QQuickItem *>(QStringLiteral("gestureShortcutCapture"));
    auto *save = window->findChild<QObject *>(QStringLiteral("saveGestureShortcut"));
    auto *cancel = window->findChild<QObject *>(QStringLiteral("cancelGestureShortcut"));
    auto *reset = window->findChild<QObject *>(QStringLiteral("resetGestureShortcut"));
    auto *standalone = window->findChild<QObject *>(QStringLiteral("gestureModeShortcutOnly"));
    auto *toolsShortcut = window->findChild<QObject *>(QStringLiteral("configureToolsShortcut"));
    if (!check(dialog && capture && save && cancel && reset && standalone && toolsShortcut, "os controles de atalho não estão disponíveis")) return 1;

    // Typing elsewhere must never be captured by the settings recorder.
    QTest::keyClick(window, Qt::Key_F8);
    if (!check(bridge.capturedKeys == 0, "houve captura fora do campo de gravação")) return 1;
    if (!check(invoke(dialog, "open"), "não foi possível abrir configurações")) return 1;
    waitFor([&] { return bridge.captureActive; });
    if (!check(bridge.captureActive && bridge.captureStarts == 1, "o atalho global não foi suspenso ao abrir configurações")
        || !check(dialog->property("draftShortcut").toString() == QStringLiteral("Shift") && dialog->property("draftDragRequired").toBool(),
               "o padrão não é Shift com arraste")) return 1;
    if (!check(preview(window, QStringLiteral("shortcut-drag-normal.png")), "não foi possível salvar a prévia normal solicitada")) return 1;
    if (!check(invoke(dialog, "beginRecording"), "não iniciou a gravação local")) return 1;
    QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::AltModifier);
    if (!check(dialog->property("draftShortcut").toString() == QStringLiteral("Ctrl+Alt+M"), "a combinação pressionada não apareceu no campo")
        || !check(!dialog->property("recording").toBool(), "soltar as teclas não concluiu a gravação")
        || !check(bridge.shortcut() == QStringLiteral("Shift") && bridge.saves == 0, "o rascunho foi aplicado antes de Salvar")) return 1;
    if (!check(invoke(cancel, "clicked") && waitFor([&] { return !dialog->property("visible").toBool(); }), "Cancelar não fechou o diálogo")
        || !check(!bridge.captureActive && bridge.captureStops == 1, "o atalho global não foi restaurado ao cancelar")
        || !check(bridge.shortcut() == QStringLiteral("Shift") && bridge.saves == 0, "Cancelar alterou o atalho salvo")) return 1;

    invoke(dialog, "open");
    waitFor([&] { return bridge.captureActive; });
    invoke(standalone, "clicked");
    if (!check(!dialog->property("draftDragRequired").toBool(), "o modo sem arraste não foi selecionado")
        || !check(!save->property("enabled").toBool() && !dialog->property("validationError").toString().isEmpty(),
                  "Shift sozinho foi aceito no modo sem arraste")) return 1;
    invoke(dialog, "beginRecording");
    QTest::keyClick(window, Qt::Key_F9);
    if (!check(dialog->property("draftShortcut").toString() == QStringLiteral("F9"), "a tecla de função não foi gravada")
        || !check(save->property("enabled").toBool(), "a tecla de função válida não habilitou Salvar")) return 1;
    if (!check(preview(window, QStringLiteral("shortcut-keys-normal.png")), "não foi possível salvar a prévia sem arraste solicitada")) return 1;
    invoke(dialog, "beginRecording");
    QTest::keyClick(window, Qt::Key_Escape);
    if (!check(dialog->property("visible").toBool() && !dialog->property("recording").toBool(), "Escape fechou o diálogo ao cancelar a gravação")
        || !check(dialog->property("draftShortcut").toString() == QStringLiteral("F9"), "Escape não preservou o rascunho anterior")) return 1;
    invoke(save, "clicked");
    waitFor([&] { return !bridge.captureActive; });
    if (!check(bridge.shortcut() == QStringLiteral("F9") && !bridge.dragRequired() && bridge.saves == 1,
               "Salvar não aplicou o atalho e o modo juntos")
        || !check(!bridge.captureActive && bridge.captureStarts == bridge.captureStops, "Salvar deixou o atalho global suspenso")) return 1;

    invoke(dialog, "open");
    waitFor([&] { return bridge.captureActive; });
    invoke(reset, "clicked");
    if (!check(dialog->property("draftShortcut").toString() == QStringLiteral("Shift") && dialog->property("draftDragRequired").toBool(),
               "restaurar Shift não restaurou também o arraste")
        || !check(bridge.shortcut() == QStringLiteral("F9") && bridge.saves == 1, "restaurar o rascunho salvou sem confirmação")) return 1;
    invoke(save, "clicked");
    waitFor([&] { return !bridge.captureActive; });
    if (!check(bridge.shortcut() == QStringLiteral("Shift") && bridge.dragRequired() && bridge.saves == 2,
               "o padrão restaurado não foi salvo")) return 1;

    // The tools tab configures a second, independent drag gesture. Recording
    // never changes conversion or commits a draft before explicit confirmation.
    invoke(dialog, "open");
    waitFor([&] { return bridge.captureActive; });
    if (!check(invoke(toolsShortcut, "clicked") && dialog->property("editingTools").toBool()
                    && dialog->property("draftShortcut").toString() == QStringLiteral("Alt")
                    && dialog->property("draftDragRequired").toBool(),
               "Ferramentas não carregou Alt com arraste como padrão independente")) return 1;
    invoke(dialog, "beginRecording");
    QTest::keyClick(window, Qt::Key_Alt);
    if (!check(dialog->property("draftShortcut").toString() == QStringLiteral("Alt")
                    && save->property("enabled").toBool() && bridge.toolsSaves == 0 && bridge.saves == 2,
               "a captura do Alt de ferramentas não preservou o rascunho")) return 1;
    invoke(dialog, "beginRecording");
    QTest::keyClick(window, Qt::Key_Shift);
    if (!check(!save->property("enabled").toBool() && !dialog->property("validationError").toString().isEmpty()
                    && bridge.toolsShortcut() == QStringLiteral("Alt") && bridge.shortcut() == QStringLiteral("Shift"),
               "o conflito entre o gesto de ferramentas e conversão não foi bloqueado")) return 1;
    invoke(dialog, "beginRecording");
    QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier);
    if (!check(dialog->property("draftShortcut").toString() == QStringLiteral("Ctrl+Alt+K")
                    && save->property("enabled").toBool() && bridge.toolsSaves == 0 && bridge.saves == 2,
               "a combinação de ferramentas não foi gravada como rascunho independente")) return 1;
    if (!check(preview(window, QStringLiteral("shortcut-tools-normal.png")), "não foi possível salvar a prévia de ferramentas")) return 1;
    invoke(save, "clicked");
    waitFor([&] { return !bridge.captureActive; });
    if (!check(bridge.toolsShortcut() == QStringLiteral("Ctrl+Alt+K") && bridge.toolsSaves == 1
                    && bridge.shortcut() == QStringLiteral("Shift") && bridge.dragRequired() && bridge.saves == 2
                    && bridge.captureStarts == bridge.captureStops,
               "Salvar ferramentas alterou conversão ou deixou o gravador ativo")) return 1;

    invoke(dialog, "open");
    waitFor([&] { return bridge.captureActive; });
    if (!check(dialog->property("editingTools").toBool()
                    && dialog->property("draftShortcut").toString() == bridge.toolsShortcut(),
               "reabrir o gravador perdeu o atalho salvo de ferramentas")) return 1;
    invoke(dialog, "beginRecording");
    QTest::keyClick(window, Qt::Key_F8);
    if (!check(dialog->property("draftShortcut").toString() == QStringLiteral("F8"),
               "a nova gravação de ferramentas não atualizou o rascunho")) return 1;
    invoke(cancel, "clicked");
    waitFor([&] { return !bridge.captureActive; });
    if (!check(bridge.toolsShortcut() == QStringLiteral("Ctrl+Alt+K") && bridge.toolsSaves == 1
                    && bridge.shortcut() == QStringLiteral("Shift") && bridge.saves == 2,
               "Cancelar ferramentas alterou um dos atalhos salvos")) return 1;
    invoke(dialog, "open");
    waitFor([&] { return bridge.captureActive; });
    invoke(reset, "clicked");
    if (!check(dialog->property("draftShortcut").toString() == QStringLiteral("Alt")
                    && dialog->property("draftDragRequired").toBool() && bridge.toolsSaves == 1,
               "Usar Alt não restaurou somente o rascunho de ferramentas")) return 1;
    invoke(save, "clicked");
    waitFor([&] { return !bridge.captureActive; });
    if (!check(bridge.toolsShortcut() == QStringLiteral("Alt") && bridge.toolsSaves == 2
                    && bridge.shortcut() == QStringLiteral("Shift") && bridge.dragRequired() && bridge.saves == 2,
               "restaurar ferramentas alterou o atalho de conversão")) return 1;

    // Main-wheel pointer transitions route cues through the bridge without
    // playing actual audio in this offscreen fixture.
    auto *wheel = window->findChild<QQuickItem *>(QStringLiteral("formatWheel"));
    if (!check(wheel != nullptr, "a roda principal não está disponível")) return 1;
    const QPointF wheelCenter(wheel->property("cx").toReal(), wheel->property("cy").toReal());
    const qreal cueRadius = wheel->property("radius").toReal() * 0.7;
    const QPoint topSector = wheel->mapToScene(wheelCenter + QPointF(0, -cueRadius)).toPoint();
    const QPoint rightSector = wheel->mapToScene(wheelCenter + QPointF(cueRadius, 0)).toPoint();
    const QPoint outsideWheel(4, window->height() / 2);
    QTest::mouseMove(window, outsideWheel);
    if (!check(waitFor([&] { return wheel->property("hovered").toInt() == -1; }),
               "o ponteiro não saiu da roda antes de testar os sons")) return 1;
    const int previousHoverSounds = bridge.hoverSounds;
    QTest::mouseMove(window, topSector);
    if (!check(waitFor([&] { return wheel->property("hovered").toInt() == 0; })
                    && bridge.hoverSounds == previousHoverSounds + 1,
               "entrar em uma opção não encaminhou um único som")) return 1;
    QTest::mouseMove(window, topSector + QPoint(2, 0));
    if (!check(wheel->property("hovered").toInt() == 0 && bridge.hoverSounds == previousHoverSounds + 1,
               "mover dentro da mesma opção repetiu o som")) return 1;
    QTest::mouseMove(window, rightSector);
    if (!check(waitFor([&] { return wheel->property("hovered").toInt() > 0; })
                    && bridge.hoverSounds == previousHoverSounds + 2,
               "mudar de opção não encaminhou exatamente outro som")) return 1;
    QTest::mouseMove(window, wheel->mapToScene(wheelCenter).toPoint());
    if (!check(waitFor([&] { return wheel->property("hovered").toInt() == -1; })
                    && bridge.hoverSounds == previousHoverSounds + 2,
               "o centro da roda reproduziu um som")) return 1;
    QTest::mouseMove(window, outsideWheel);
    if (!check(bridge.hoverSounds == previousHoverSounds + 2,
               "sair da roda reproduziu um som")) return 1;

    // Appearance has its own saved draft and never suspends global shortcuts.
    auto *appearance = window->findChild<QObject *>(QStringLiteral("wheelAppearanceDialog"));
    auto *appearanceMenu = window->findChild<QObject *>(QStringLiteral("menuWheelAppearance"));
    auto *transparency = window->findChild<QQuickItem *>(QStringLiteral("wheelTransparencySlider"));
    auto *size = window->findChild<QQuickItem *>(QStringLiteral("wheelSizeSlider"));
    auto *sound = window->findChild<QQuickItem *>(QStringLiteral("wheelSoundToggle"));
    auto *appearanceSave = window->findChild<QQuickItem *>(QStringLiteral("wheelAppearanceSave"));
    auto *appearanceCancel = window->findChild<QObject *>(QStringLiteral("wheelAppearanceCancel"));
    auto *appearanceDefault = window->findChild<QObject *>(QStringLiteral("wheelAppearanceDefault"));
    auto *colorHex = window->findChild<QQuickItem *>(QStringLiteral("wheelColorHex"));
    auto *colorChoose = window->findChild<QObject *>(QStringLiteral("wheelColorChoose"));
    auto *colorPicker = window->findChild<QObject *>(QStringLiteral("wheelColorPicker"));
    auto *colorError = window->findChild<QObject *>(QStringLiteral("wheelColorError"));
    // Repeater delegates belong to the visual tree, which need not match
    // QObject ownership while the popup is not attached to the overlay yet.
    auto *colorBlue = findVisual(appearance ? appearance->property("contentItem").value<QQuickItem *>() : nullptr,
                                 QStringLiteral("wheelColorPreset1"));
    if (!colorHex || !colorChoose || !colorPicker || !colorError || !colorBlue)
        std::fprintf(stderr, "appearance color controls: hex=%d choose=%d picker=%d error=%d blue=%d\n",
                     bool(colorHex), bool(colorChoose), bool(colorPicker), bool(colorError), bool(colorBlue));
    if (!check(appearance && appearanceMenu && transparency && size && sound && appearanceSave && appearanceCancel && appearanceDefault
                    && colorHex && colorChoose && colorPicker && colorError && colorBlue,
               "os controles de aparência não estão disponíveis")) return 1;
    const int previousCaptureStarts = bridge.captureStarts;
    const auto openAppearance = [&] {
        return invoke(appearanceMenu, "triggered")
            && waitFor([&] { return appearance->property("opened").toBool(); });
    };
    const auto moveTransparency = [&](Qt::Key key) {
        transparency->forceActiveFocus();
        QTest::keyClick(window, key);
        QCoreApplication::processEvents();
    };
    const auto moveSize = [&](Qt::Key key) {
        size->forceActiveFocus();
        QTest::keyClick(window, key);
        QCoreApplication::processEvents();
    };
    const auto toggleSound = [&] {
        sound->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QCoreApplication::processEvents();
    };
    const auto editColor = [&](const char *hex) {
        // A platform color dialog owns another offscreen window. Wait for the
        // fixture's main window to regain focus before delivering local keys.
        window->requestActivate();
        colorHex->forceActiveFocus();
        if (!waitFor([&] { return colorHex->hasActiveFocus(); })) return false;
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const char *character = hex; *character; ++character)
            QTest::keyClick(window, *character);
        QCoreApplication::processEvents();
        return true;
    };
    if (!check(openAppearance() && appearance->property("draftTransparency").toInt() == 35
                    && appearance->property("draftSize").toInt() == 100
                    && appearance->property("draftColor").toString() == QStringLiteral("#b99cde")
                    && appearance->property("draftSoundEnabled").toBool() && sound->property("checked").toBool(),
               "a aparência não abriu com o padrão atual")) return 1;
    toggleSound();
    moveTransparency(Qt::Key_End);
    moveSize(Qt::Key_Home);
    colorBlue->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    if (!check(appearance->property("draftTransparency").toInt() == 80 && bridge.wheelTransparency() == 35
                    && appearance->property("draftSize").toInt() == 70 && bridge.wheelSize() == 100
                    && appearance->property("draftColor").toString() == QStringLiteral("#64a8ed")
                    && bridge.wheelColor() == QStringLiteral("#b99cde")
                    && !appearance->property("draftSoundEnabled").toBool() && bridge.wheelSoundEnabled()
                    && bridge.appearanceSaves == 0 && bridge.captureStarts == previousCaptureStarts,
               "a prévia por teclado alterou a preferência ou o atalho global antes de salvar")) return 1;
    if (!check(preview(window, QStringLiteral("wheel-appearance-transparent.png")), "não salvou a prévia de aparência")) return 1;
    invoke(appearanceCancel, "clicked");
    if (!check(waitFor([&] { return !appearance->property("visible").toBool(); }) && bridge.wheelTransparency() == 35 && bridge.wheelSize() == 100
                    && bridge.wheelColor() == QStringLiteral("#b99cde") && bridge.wheelSoundEnabled(),
               "Cancelar alterou a aparência salva")) return 1;
    if (!check(openAppearance() && appearance->property("draftTransparency").toInt() == 35
                    && appearance->property("draftSize").toInt() == 100
                    && appearance->property("draftColor").toString() == QStringLiteral("#b99cde")
                    && appearance->property("draftSoundEnabled").toBool() && sound->property("checked").toBool(),
               "reabrir preservou um rascunho cancelado")) return 1;
    toggleSound();
    if (!check(invoke(colorChoose, "clicked") && waitFor([&] { return colorPicker->property("visible").toBool(); }),
               "Escolher cor não abriu o seletor")) return 1;
    colorPicker->setProperty("selectedColor", QColor(QStringLiteral("#e58fac")));
    if (!check(invoke(colorPicker, "reject") && waitFor([&] { return !colorPicker->property("visible").toBool(); })
                    && appearance->property("draftColor").toString() == QStringLiteral("#b99cde")
                    && bridge.wheelColor() == QStringLiteral("#b99cde") && bridge.appearanceSaves == 0,
               "cancelar o seletor de cor alterou o rascunho ou a preferência salva")) return 1;
    if (!check(invoke(colorChoose, "clicked") && waitFor([&] { return colorPicker->property("visible").toBool(); })
                    && colorPicker->property("selectedColor").value<QColor>().name() == QStringLiteral("#b99cde"),
               "reabrir o seletor não carregou a cor do rascunho")) return 1;
    colorPicker->setProperty("selectedColor", QColor(QStringLiteral("#e58fac")));
    if (!check(invoke(colorPicker, "accept") && waitFor([&] { return !colorPicker->property("visible").toBool(); })
                    && appearance->property("draftColor").toString() == QStringLiteral("#e58fac")
                    && bridge.wheelColor() == QStringLiteral("#b99cde") && bridge.appearanceSaves == 0,
               "o seletor de cor não atualizou somente o rascunho")) return 1;
    if (!check(editColor("#NOPE"), "o campo hexadecimal não recebeu foco")) return 1;
    if (appearanceSave->isEnabled() || !colorError->property("visible").toBool())
        std::fprintf(stderr, "invalid color: draft=%s field=%s save=%d guidance=%d focus=%d\n",
                     qPrintable(appearance->property("draftColor").toString()),
                     qPrintable(colorHex->property("text").toString()), appearanceSave->isEnabled(),
                     colorError->property("visible").toBool(), colorHex->hasActiveFocus());
    if (!check(!appearanceSave->isEnabled() && colorError->property("visible").toBool(),
               "cor inválida não bloqueou Salvar ou não mostrou a orientação")) return 1;
    invoke(appearanceSave, "clicked");
    if (!check(appearance->property("visible").toBool() && bridge.appearanceSaves == 0
                    && bridge.wheelColor() == QStringLiteral("#b99cde"),
               "cor inválida foi salva ou fechou o diálogo")) return 1;
    if (!check(editColor("#58B89D"), "o campo hexadecimal não recebeu foco")) return 1;
    if (!check(appearanceSave->isEnabled() && !colorError->property("visible").toBool()
                    && appearance->property("draftColor").toString() == QStringLiteral("#58b89d"),
               "cor válida não foi normalizada ou não liberou Salvar")) return 1;
    moveTransparency(Qt::Key_Home);
    moveSize(Qt::Key_End);
    if (!check(preview(window, QStringLiteral("wheel-appearance-large.png")), "não salvou a prévia ampliada")) return 1;
    invoke(appearanceSave, "clicked");
    if (!check(waitFor([&] { return !appearance->property("visible").toBool(); }) && bridge.wheelTransparency() == 0
                    && bridge.wheelSize() == 130 && bridge.wheelColor() == QStringLiteral("#58b89d")
                    && !bridge.wheelSoundEnabled()
                    && bridge.appearanceSaves == 1, "Salvar não aplicou a aparência combinada uma única vez")) return 1;
    if (!check(openAppearance() && appearance->property("draftTransparency").toInt() == 0
                    && appearance->property("draftSize").toInt() == 130
                    && appearance->property("draftColor").toString() == QStringLiteral("#58b89d")
                    && !appearance->property("draftSoundEnabled").toBool() && !sound->property("checked").toBool(),
               "reabrir não carregou a aparência salva")) return 1;
    invoke(appearanceDefault, "clicked");
    if (!check(appearance->property("draftTransparency").toInt() == 35 && bridge.wheelTransparency() == 0
                    && appearance->property("draftSize").toInt() == 100 && bridge.wheelSize() == 130
                    && appearance->property("draftColor").toString() == QStringLiteral("#b99cde")
                    && bridge.wheelColor() == QStringLiteral("#58b89d")
                    && appearance->property("draftSoundEnabled").toBool() && !bridge.wheelSoundEnabled(),
               "restaurar o padrão salvou sem confirmação")) return 1;
    if (!check(preview(window, QStringLiteral("wheel-appearance-default.png")), "não salvou a prévia padrão")) return 1;
    invoke(appearanceSave, "clicked");
    if (!check(waitFor([&] { return !appearance->property("visible").toBool(); }) && bridge.wheelTransparency() == 35
                    && bridge.wheelSize() == 100 && bridge.wheelColor() == QStringLiteral("#b99cde")
                    && bridge.wheelSoundEnabled()
                    && bridge.appearanceSaves == 2, "o padrão visual não foi restaurado")) return 1;
    bridge.failAppearanceSave = true;
    openAppearance();
    toggleSound();
    moveTransparency(Qt::Key_End);
    moveSize(Qt::Key_Home);
    if (!check(editColor("#e7b45a"), "o campo hexadecimal não recebeu foco")) return 1;
    invoke(appearanceSave, "clicked");
    if (!check(appearance->property("visible").toBool() && bridge.wheelTransparency() == 35
                    && bridge.wheelSize() == 100 && bridge.wheelColor() == QStringLiteral("#b99cde")
                    && bridge.wheelSoundEnabled()
                    && bridge.appearanceSaves == 2 && !bridge.wheelAppearanceError().isEmpty(),
               "a falha de salvamento fechou o diálogo ou alterou o valor salvo")) return 1;
    QTest::keyClick(window, Qt::Key_Escape);
    if (!check(waitFor([&] { return !appearance->property("visible").toBool(); })
                    && bridge.captureStarts == previousCaptureStarts && !bridge.captureActive,
               "Escape não cancelou a aparência ou alterou a captura global")) return 1;
    bridge.failAppearanceSave = false;
    if (!check(openAppearance() && appearance->property("draftTransparency").toInt() == 35
                    && appearance->property("draftSize").toInt() == 100
                    && appearance->property("draftColor").toString() == QStringLiteral("#b99cde")
                    && appearance->property("draftSoundEnabled").toBool() && sound->property("checked").toBool(),
               "Escape preservou um rascunho de aparência descartado")) return 1;
    invoke(appearanceCancel, "clicked");
    waitFor([&] { return !appearance->property("visible").toBool(); });

    // Both settings dialogs remain usable from the compact wheel too.
    dialog->setProperty("editingTools", false);
    window->setProperty("compact", true);
    if (!check(waitFor([&] { return window->width() == 550 && window->height() == 610; }),
               "mudar para roda compacta não atualizou a geometria automaticamente")) return 1;
    invoke(dialog, "open");
    waitFor([&] { return bridge.captureActive; });
    if (!check(dialog->property("height").toReal() <= window->height() - 35, "o diálogo ultrapassa a janela compacta")) return 1;
    if (!check(preview(window, QStringLiteral("shortcut-drag-compact.png")), "não foi possível salvar a prévia compacta solicitada")) return 1;
    invoke(cancel, "clicked");
    waitFor([&] { return !bridge.captureActive; });
    if (!check(openAppearance() && appearance->property("width").toReal() <= window->width() - 20
                    && appearance->property("height").toReal() <= window->height() - 20,
               "a aparência ultrapassa a janela compacta")) return 1;
    moveSize(Qt::Key_End);
    if (!check(editColor("#64a8ed"), "o campo hexadecimal não recebeu foco")) return 1;
    if (!check(preview(window, QStringLiteral("wheel-appearance-compact.png")), "não salvou a prévia compacta de aparência")) return 1;
    const QPointF savePosition = appearanceSave->mapToScene(QPointF(appearanceSave->width() / 2,
                                                                   appearanceSave->height() / 2));
    if (!(savePosition.y() > 0 && savePosition.y() < window->height() && appearanceSave->isVisible()))
        std::fprintf(stderr, "compact appearance: windowHeight=%d dialogHeight=%.1f saveY=%.1f visible=%d\n",
                     window->height(), appearance->property("height").toReal(), savePosition.y(), appearanceSave->isVisible());
    if (!check(savePosition.y() > 0 && savePosition.y() < window->height()
                    && appearanceSave->isVisible(), "Salvar não está acessível na janela compacta")) return 1;
    invoke(appearanceCancel, "clicked");
    std::puts("qml_shortcut_check: captura local, rascunho, cancelar, dois modos, ferramentas independentes, conflitos, Escape e padrões restaurados");
    return 0;
}

#include "qml_shortcut_check.moc"

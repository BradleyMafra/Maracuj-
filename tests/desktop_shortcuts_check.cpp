// Test the shared settings/capture transaction against an in-process platform
// adapter. No desktop shortcut is reserved, no global input is inspected, and
// preferences are written only below this test's private temporary directory.
#include "desktopintegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QVector>
#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace {

bool initiallyEnabled = true;
QString blockedShortcut;
bool dispatchOnEnable = false;
QVector<bool> enableCalls;
int appearanceApplications = 0;

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

QString savedShortcut()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Winux"), QStringLiteral("Maracuja"));
    settings.sync();
    return settings.value(QStringLiteral("activation/shortcut"), QStringLiteral("Shift")).toString();
}

bool savedDragRequired()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Winux"), QStringLiteral("Maracuja"));
    settings.sync();
    return settings.value(QStringLiteral("activation/dragRequired"), true).toBool();
}

QString savedToolsShortcut()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Winux"), QStringLiteral("Maracuja"));
    settings.sync();
    return settings.value(QStringLiteral("toolsActivation/shortcut"), QStringLiteral("Alt")).toString();
}

bool savedToolsDragRequired()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Winux"), QStringLiteral("Maracuja"));
    settings.sync();
    return settings.value(QStringLiteral("toolsActivation/dragRequired"), true).toBool();
}

bool enabledSince(qsizetype start)
{
    for (qsizetype index = start; index < enableCalls.size(); ++index)
        if (enableCalls.at(index)) return true;
    return false;
}

} // namespace

// These definitions deliberately replace desktopintegration.cpp/_win.cpp for
// this test target. desktopshortcuts.cpp and the actual shortcut codec remain
// production code, including their real QSettings transaction and signals.
DesktopIntegration::DesktopIntegration(QObject *parent) : QObject(parent)
{
    loadActivation();
    m_available = true;
    m_enabled = initiallyEnabled;
    m_status = QStringLiteral("fixture ready");
}

DesktopIntegration::~DesktopIntegration() = default;
void DesktopIntegration::setConverter(Converter *converter) { m_converter = converter; }
void DesktopIntegration::start() { setEnabled(true); }
void DesktopIntegration::stop() { setEnabled(false); }

void DesktopIntegration::setEnabled(bool enabled)
{
    enableCalls.append(enabled);
    if (enabled && (m_activation.portable() == blockedShortcut || m_toolsActivation.portable() == blockedShortcut)) {
        m_enabled = false;
        m_status = QStringLiteral("fixture shortcut conflict");
        emit changed();
        return;
    }
    m_enabled = enabled;
    m_status = enabled ? QStringLiteral("fixture enabled") : QStringLiteral("fixture disabled");
    emit changed();
    // Simulate the adapter's own activation arriving during a settings save.
    // The shared handler must suppress this while recording is active.
    if (enabled && dispatchOnEnable) activateShortcut();
}

void DesktopIntegration::OpenFiles(const QStringList &paths, bool compact) { emit openRequested(paths, compact); }
void DesktopIntegration::ProbeOpened() {}
void DesktopIntegration::ToolsProbeOpened() {}
void DesktopIntegration::Quit() { emit quitRequested(); }
void DesktopIntegration::notifyFinished(int, bool) {}
void DesktopIntegration::applyWheelAppearance() { ++appearanceApplications; }
#ifndef Q_OS_WIN
void DesktopIntegration::globalShortcutPressed(const QString &, const QString &, qlonglong) { activateShortcut(); }
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create the private preferences directory");
        qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                           QDir(temporary.path()).filePath(QStringLiteral("unused-system")));
        QSettings::setDefaultFormat(QSettings::IniFormat);

        DesktopIntegration desktop;
        require(desktop.available() && desktop.enabled()
                    && desktop.shortcut() == QStringLiteral("Shift") && desktop.dragRequired()
                    && desktop.toolsShortcut() == QStringLiteral("Alt"),
                "Fresh preferences did not preserve the independent Shift/Alt drag gestures");
        {
            QObject appearanceContext;
            int appearanceSignals = 0;
            int shortcutSignals = 0;
            QObject::connect(&desktop, &DesktopIntegration::wheelAppearanceChanged, &appearanceContext,
                             [&] { ++appearanceSignals; });
            QObject::connect(&desktop, &DesktopIntegration::changed, &appearanceContext,
                             [&] { ++shortcutSignals; });
            const auto beforeAppearance = enableCalls.size();
            using Appearance = std::tuple<int, int, QString>;
            auto savedAppearance = [] {
                QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                                   QStringLiteral("Winux"), QStringLiteral("Maracuja"));
                settings.sync();
                return Appearance{settings.value(QStringLiteral("appearance/wheelTransparency"), 35).toInt(),
                                  settings.value(QStringLiteral("appearance/wheelSize"), 100).toInt(),
                                  settings.value(QStringLiteral("appearance/wheelColor"), "#b99cde").toString()};
            };
            auto liveAppearance = [&] {
                return Appearance{desktop.wheelTransparency(), desktop.wheelSize(), desktop.wheelColor()};
            };
            const Appearance defaults{35, 100, QStringLiteral("#b99cde")};
            const Appearance custom{57, 120, QStringLiteral("#0a9bce")};
            require(liveAppearance() == defaults && desktop.wheelAppearanceError().isEmpty(),
                    "Fresh appearance preferences did not preserve the transparency, size and color defaults");
            require(desktop.setWheelAppearance(57, 120, QStringLiteral("#0A9bCE"))
                        && liveAppearance() == custom && savedAppearance() == custom
                        && desktop.wheelAppearanceError().isEmpty()
                        && appearanceSignals == 1 && appearanceApplications == 1,
                    "A valid appearance triple was not normalized, applied, persisted and notified once");
            {
                DesktopIntegration reloaded;
                require(reloaded.wheelTransparency() == 57 && reloaded.wheelSize() == 120
                            && reloaded.wheelColor() == QStringLiteral("#0a9bce"),
                        "Reloading appearance preferences lost the saved transparency, size or color");
            }
            // A malformed member must reject the whole transaction. The other
            // candidates, saved values and renderer must remain untouched.
            require(!desktop.setWheelAppearance(-1, 75, QStringLiteral("#112233"))
                        && !desktop.setWheelAppearance(81, 125, QStringLiteral("#223344"))
                        && !desktop.setWheelAppearance(25, 69, QStringLiteral("#334455"))
                        && !desktop.setWheelAppearance(45, 131, QStringLiteral("#445566"))
                        && liveAppearance() == custom && savedAppearance() == custom
                        && !desktop.wheelAppearanceError().isEmpty()
                        && appearanceSignals == 5 && appearanceApplications == 1,
                    "An invalid appearance triple partially changed the live renderer or saved preferences");
            const QStringList malformedColors = {
                QString(), QStringLiteral("red"), QStringLiteral("transparent"),
                QStringLiteral("#abc"), QStringLiteral("#12345"), QStringLiteral("#1234567"),
                QStringLiteral("#12345678"), QStringLiteral("#12G456"), QStringLiteral("123456"),
                QStringLiteral(" #123456"), QStringLiteral("#123456 "), QStringLiteral("#123456\n"),
                QStringLiteral("#１２３４５６"), QStringLiteral("#аbcdef"),
                QStringLiteral("#123\0ab")
            };
            for (const auto &value : malformedColors) {
                const int previousSignals = appearanceSignals;
                require(!desktop.setWheelAppearance(20, 90, value)
                            && liveAppearance() == custom && savedAppearance() == custom
                            && !desktop.wheelAppearanceError().isEmpty()
                            && appearanceSignals == previousSignals + 1 && appearanceApplications == 1,
                        "A malformed or non-ASCII color was accepted or partially changed appearance");
            }
            // Both old setters and the two-argument invokable retain the
            // selected color instead of restoring the default behind the UI.
            const int beforeCompatibility = appearanceSignals;
            require(desktop.setWheelTransparency(0)
                        && liveAppearance() == Appearance{0, 120, QStringLiteral("#0a9bce")}
                        && savedAppearance() == liveAppearance() && desktop.wheelAppearanceError().isEmpty()
                        && desktop.setWheelTransparency(80)
                        && liveAppearance() == Appearance{80, 120, QStringLiteral("#0a9bce")}
                        && savedAppearance() == liveAppearance()
                        && desktop.setWheelSize(70)
                        && liveAppearance() == Appearance{80, 70, QStringLiteral("#0a9bce")}
                        && savedAppearance() == liveAppearance()
                        && desktop.setWheelSize(130)
                        && liveAppearance() == Appearance{80, 130, QStringLiteral("#0a9bce")}
                        && savedAppearance() == liveAppearance()
                        && desktop.setWheelAppearance(35, 100)
                        && liveAppearance() == Appearance{35, 100, QStringLiteral("#0a9bce")}
                        && savedAppearance() == liveAppearance()
                        && appearanceSignals == beforeCompatibility + 5 && appearanceApplications == 6,
                    "Appearance bounds or compatibility setters lost the selected color");
            require(desktop.setWheelColor(QStringLiteral("#FFFFFF"))
                        && liveAppearance() == Appearance{35, 100, QStringLiteral("#ffffff")}
                        && savedAppearance() == liveAppearance()
                        && desktop.setWheelColor(QStringLiteral("#000000"))
                        && liveAppearance() == Appearance{35, 100, QStringLiteral("#000000")}
                        && savedAppearance() == liveAppearance() && appearanceApplications == 8,
                    "The color setter did not accept and preserve opaque RGB boundary colors");
            bool invocationResult = false;
            require(QMetaObject::invokeMethod(&desktop, "setWheelAppearance", Qt::DirectConnection,
                                             Q_RETURN_ARG(bool, invocationResult), Q_ARG(int, 43), Q_ARG(int, 112),
                                             Q_ARG(QString, QStringLiteral("#AA55CC")))
                        && invocationResult
                        && liveAppearance() == Appearance{43, 112, QStringLiteral("#aa55cc")}
                        && savedAppearance() == liveAppearance(),
                    "The three-argument appearance overload is not available through Qt's metaobject");
            invocationResult = false;
            require(QMetaObject::invokeMethod(&desktop, "setWheelAppearance", Qt::DirectConnection,
                                             Q_RETURN_ARG(bool, invocationResult), Q_ARG(int, 35), Q_ARG(int, 100))
                        && invocationResult
                        && liveAppearance() == Appearance{35, 100, QStringLiteral("#aa55cc")}
                        && savedAppearance() == liveAppearance() && appearanceApplications == 10,
                    "The legacy two-argument appearance overload is unavailable or lost the color");

            // A regular file cannot contain the QSettings directory. This
            // exercises a real write failure without relying on permissions.
            const auto beforeFailure = liveAppearance();
            const int signalsBeforeFailure = appearanceSignals;
            const QString blockedPreferences = temporary.filePath(QStringLiteral("blocked-preferences"));
            QFile blocker(blockedPreferences);
            require(blocker.open(QIODevice::WriteOnly), "Cannot prepare a private settings failure");
            blocker.close();
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, blockedPreferences);
            require(!desktop.setWheelAppearance(41, 117, QStringLiteral("#22bb88"))
                        && liveAppearance() == beforeFailure && !desktop.wheelAppearanceError().isEmpty()
                        && appearanceSignals == signalsBeforeFailure + 1 && appearanceApplications == 10,
                    "A failed appearance save partially changed the live settings or was not reported once");
            {
                QSettings failed(QSettings::IniFormat, QSettings::UserScope,
                                 QStringLiteral("Winux"), QStringLiteral("Maracuja"));
                require(!failed.contains(QStringLiteral("appearance/wheelTransparency"))
                            && !failed.contains(QStringLiteral("appearance/wheelSize"))
                            && !failed.contains(QStringLiteral("appearance/wheelColor")),
                        "A failed appearance save retained a partially staged setting");
            }
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
            require(savedAppearance() == beforeFailure
                        && desktop.setWheelAppearance(35, 100, QStringLiteral("#b99cde"))
                        && liveAppearance() == defaults && savedAppearance() == defaults
                        && desktop.wheelAppearanceError().isEmpty()
                        && appearanceSignals == signalsBeforeFailure + 2 && appearanceApplications == 11,
                    "Appearance save recovery failed to preserve all three previous preferences");

            const QVariantList malformedTransparency = {
                -1, 81, QStringLiteral("invalid"), QString(), QStringLiteral("19.5"),
                QStringLiteral("2147483648"), QStringLiteral("15 trailing"), true
            };
            for (const auto &value : malformedTransparency) {
                QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                                   QStringLiteral("Winux"), QStringLiteral("Maracuja"));
                settings.setValue(QStringLiteral("appearance/wheelTransparency"), value);
                settings.setValue(QStringLiteral("appearance/wheelSize"), 115);
                settings.setValue(QStringLiteral("appearance/wheelColor"), "#22BB88");
                settings.sync();
                DesktopIntegration reloaded;
                require(reloaded.wheelTransparency() == 35 && reloaded.wheelSize() == 115
                            && reloaded.wheelColor() == QStringLiteral("#22bb88")
                            && reloaded.wheelAppearanceError().isEmpty(),
                        "An invalid saved transparency did not default independently of the size and color");
            }
            const QVariantList malformedSize = {
                69, 131, QStringLiteral("invalid"), QString(), QStringLiteral("100.5"),
                QStringLiteral("2147483648"), QStringLiteral("100 trailing"), true
            };
            for (const auto &value : malformedSize) {
                QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                                   QStringLiteral("Winux"), QStringLiteral("Maracuja"));
                settings.setValue(QStringLiteral("appearance/wheelTransparency"), 45);
                settings.setValue(QStringLiteral("appearance/wheelSize"), value);
                settings.setValue(QStringLiteral("appearance/wheelColor"), "#22bb88");
                settings.sync();
                DesktopIntegration reloaded;
                require(reloaded.wheelTransparency() == 45 && reloaded.wheelSize() == 100
                            && reloaded.wheelColor() == QStringLiteral("#22bb88")
                            && reloaded.wheelAppearanceError().isEmpty(),
                        "An invalid saved size did not default independently of transparency and color");
            }
            for (const auto &value : malformedColors) {
                QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                                   QStringLiteral("Winux"), QStringLiteral("Maracuja"));
                settings.setValue(QStringLiteral("appearance/wheelTransparency"), 45);
                settings.setValue(QStringLiteral("appearance/wheelSize"), 115);
                settings.setValue(QStringLiteral("appearance/wheelColor"), value);
                settings.sync();
                DesktopIntegration reloaded;
                require(reloaded.wheelTransparency() == 45 && reloaded.wheelSize() == 115
                            && reloaded.wheelColor() == QStringLiteral("#b99cde")
                            && reloaded.wheelAppearanceError().isEmpty(),
                        "An invalid saved color did not default independently of transparency and size");
            }
            require(desktop.setWheelAppearance(35, 100, QStringLiteral("#b99cde"))
                        && savedAppearance() == defaults && desktop.enabled()
                        && desktop.shortcut() == QStringLiteral("Shift")
                        && desktop.toolsShortcut() == QStringLiteral("Alt") && desktop.dragRequired()
                        && enableCalls.size() == beforeAppearance && shortcutSignals == 0,
                    "Changing wheel appearance restarted, suspended or modified a shortcut");
        }
        {
            require(desktop.wheelSoundEnabled(), "Fresh wheel settings did not enable the gentle hover sound");
            const auto beforeSound = enableCalls.size();
            bool saved = false;
            require(QMetaObject::invokeMethod(&desktop, "setWheelAppearance", Qt::DirectConnection,
                                             Q_RETURN_ARG(bool, saved), Q_ARG(int, 40), Q_ARG(int, 110),
                                             Q_ARG(QString, QStringLiteral("#bb88dd")), Q_ARG(bool, false))
                        && saved && !desktop.wheelSoundEnabled(),
                    "The four-argument appearance save did not disable sound through Qt's metaobject");
            {
                DesktopIntegration reloaded;
                require(!reloaded.wheelSoundEnabled() && reloaded.wheelTransparency() == 40
                            && reloaded.wheelSize() == 110 && reloaded.wheelColor() == QStringLiteral("#bb88dd"),
                        "Reloading settings lost one of the four saved wheel preferences");
            }
            require(desktop.setWheelTransparency(30) && desktop.setWheelSize(95)
                        && desktop.setWheelColor(QStringLiteral("#aaccee"))
                        && desktop.setWheelAppearance(35, 100)
                        && desktop.setWheelAppearance(40, 105, QStringLiteral("#bb99dd"))
                        && !desktop.wheelSoundEnabled(),
                    "A compatibility appearance setter reset the saved sound preference");
            require(!desktop.setWheelAppearance(85, 100, QStringLiteral("#b99cde"), true)
                        && !desktop.wheelSoundEnabled(),
                    "A rejected appearance transaction partially enabled sound");
            const QString blockedSound = temporary.filePath(QStringLiteral("blocked-sound"));
            QFile blocker(blockedSound);
            require(blocker.open(QIODevice::WriteOnly), "Cannot prepare the private sound save failure");
            blocker.close();
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, blockedSound);
            require(!desktop.setWheelSoundEnabled(true) && !desktop.wheelSoundEnabled(),
                    "A failed appearance write changed the live sound preference");
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
            {
                DesktopIntegration reloaded;
                require(!reloaded.wheelSoundEnabled(), "A failed write changed the persisted sound preference");
            }
            require(desktop.setWheelAppearance(35, 100, QStringLiteral("#b99cde"), true)
                        && desktop.wheelSoundEnabled() && enableCalls.size() == beforeSound,
                    "Saving wheel sound restarted a shortcut or could not restore the defaults");
        }
        const auto beforeDuplicate = enableCalls.size();
        require(!desktop.validateToolsShortcut(QStringLiteral("Shift")).isEmpty()
                    && !desktop.setToolsActivation(QStringLiteral("Shift"))
                    && !desktop.setActivation(QStringLiteral("Alt"), true)
                    && desktop.shortcut() == QStringLiteral("Shift")
                    && desktop.toolsShortcut() == QStringLiteral("Alt")
                    && enableCalls.size() == beforeDuplicate,
                "Duplicate conversion/tools gestures changed a live shortcut");
        require(desktop.setActivation(QStringLiteral("Ctrl+Alt+M"), true)
                    && desktop.shortcut() == QStringLiteral("Ctrl+Alt+M") && desktop.dragRequired()
                    && desktop.enabled() && savedShortcut() == desktop.shortcut() && savedDragRequired(),
                "A valid drag shortcut was not applied and persisted");
        require(desktop.setToolsActivation(QStringLiteral("Ctrl+Alt+T"))
                    && desktop.toolsShortcut() == QStringLiteral("Ctrl+Alt+T")
                    && desktop.shortcut() == QStringLiteral("Ctrl+Alt+M") && desktop.dragRequired()
                    && savedToolsShortcut() == desktop.toolsShortcut() && savedToolsDragRequired(),
                "Saving tools did not persist its own drag shortcut independently");
        {
            DesktopIntegration reloaded;
            require(reloaded.shortcut() == desktop.shortcut()
                        && reloaded.toolsShortcut() == desktop.toolsShortcut() && reloaded.dragRequired(),
                    "Reloading preferences lost one of the two independent gestures");
        }
        const auto working = desktop.shortcut();
        const auto beforeInvalid = enableCalls.size();
        require(!desktop.setActivation(QString(), false)
                    && !desktop.shortcutError().isEmpty()
                    && desktop.shortcut() == working && desktop.dragRequired() && desktop.enabled()
                    && enableCalls.size() == beforeInvalid && savedShortcut() == working && savedDragRequired(),
                "An invalid replacement changed the live or persisted shortcut");
        require(!desktop.setActivation(QStringLiteral("M"), false)
                    && desktop.shortcut() == working && desktop.dragRequired() && desktop.enabled(),
                "A bare letter was accepted as a system-wide standalone shortcut");

        const auto cancelBegin = enableCalls.size();
        desktop.setShortcutCaptureActive(true);
        desktop.setShortcutCaptureActive(true); // Must not forget that it was enabled.
        require(!desktop.enabled(), "Opening the recorder did not suspend the active shortcut");
        desktop.setShortcutCaptureActive(false);
        desktop.setShortcutCaptureActive(false);
        require(desktop.enabled() && desktop.shortcut() == working
                    && enableCalls.size() == cancelBegin + 2
                    && !enableCalls.at(cancelBegin) && enableCalls.at(cancelBegin + 1),
                "Cancelling a repeated recorder open did not resume exactly the prior state");

        int unexpectedActivations = 0;
        QObject::connect(&desktop, &DesktopIntegration::openRequested, &app,
                         [&](const QStringList &, bool) { ++unexpectedActivations; });
        desktop.setShortcutCaptureActive(true);
        const auto saveBegin = enableCalls.size();
        dispatchOnEnable = true;
        require(desktop.setActivation(QStringLiteral("Ctrl+Alt+K"), false),
                "A valid standalone shortcut was not saved from the recorder");
        dispatchOnEnable = false;
        require(!desktop.enabled() && !desktop.dragRequired()
                    && desktop.shortcut() == QStringLiteral("Ctrl+Alt+K")
                    && enableCalls.size() == saveBegin + 2
                    && enableCalls.at(saveBegin) && !enableCalls.at(saveBegin + 1)
                    && unexpectedActivations == 0
                    && savedShortcut() == desktop.shortcut() && !savedDragRequired(),
                "Saving did not validate temporarily, remain suspended, or suppress recorder activation");
        desktop.setShortcutCaptureActive(false);
        require(desktop.enabled(), "Closing the saved recorder did not restore the enabled state");
        desktop.setShortcutCaptureActive(true);
        dispatchOnEnable = true;
        require(desktop.setToolsActivation(QStringLiteral("Ctrl+Alt+P"))
                    && !desktop.enabled() && desktop.shortcut() == QStringLiteral("Ctrl+Alt+K")
                    && !desktop.dragRequired() && desktop.toolsShortcut() == QStringLiteral("Ctrl+Alt+P")
                    && savedToolsShortcut() == desktop.toolsShortcut() && savedToolsDragRequired()
                    && unexpectedActivations == 0,
                "Saving tools from capture enabled input, activated conversion or changed the conversion binding");
        dispatchOnEnable = false;
        desktop.setShortcutCaptureActive(false);
        require(desktop.enabled(), "Closing the tools recorder did not resume the active adapter");
        const auto toolsBaseline = desktop.toolsShortcut();
        require(!desktop.setToolsActivation(desktop.shortcut())
                    && !desktop.setActivation(toolsBaseline, false)
                    && desktop.shortcut() == QStringLiteral("Ctrl+Alt+K")
                    && desktop.toolsShortcut() == toolsBaseline && !desktop.dragRequired(),
                "A tools/direct shortcut collision changed either saved binding");

        initiallyEnabled = false;
        {
            DesktopIntegration disabled;
            require(!disabled.enabled() && disabled.shortcut() == desktop.shortcut() && !disabled.dragRequired(),
                    "Reloading preferences lost the saved standalone shortcut");
            const auto disabledBegin = enableCalls.size();
            disabled.setShortcutCaptureActive(true);
            require(disabled.setActivation(QStringLiteral("F8"), false),
                    "An already disabled adapter could not save a valid shortcut");
            require(disabled.setToolsActivation(QStringLiteral("Alt")),
                    "An already disabled adapter could not save its tools gesture");
            disabled.setShortcutCaptureActive(false);
            require(!disabled.enabled() && disabled.shortcut() == QStringLiteral("F8")
                        && disabled.toolsShortcut() == QStringLiteral("Alt") && !enabledSince(disabledBegin),
                    "Saving or cancelling unexpectedly enabled an originally disabled adapter");
        }
        initiallyEnabled = true;
        // Restore the working instance's settings before checking both rollback
        // paths; neither failed transaction may alter this saved configuration.
        require(desktop.setActivation(QStringLiteral("Ctrl+Alt+K"), false),
                "Cannot restore the preference baseline for conflict checks");
        require(desktop.setToolsActivation(toolsBaseline), "Cannot restore the tools preference baseline");
        const auto priorShortcut = desktop.shortcut();
        const bool priorMode = desktop.dragRequired();
        blockedShortcut = QStringLiteral("Ctrl+Alt+J");
        require(!desktop.setActivation(blockedShortcut, false)
                    && desktop.enabled() && desktop.shortcut() == priorShortcut
                    && desktop.dragRequired() == priorMode
                    && desktop.shortcutError().contains(QStringLiteral("fixture shortcut conflict"))
                    && savedShortcut() == priorShortcut && savedDragRequired() == priorMode,
                "A platform conflict did not roll back the live and saved working shortcut");

        desktop.setShortcutCaptureActive(true);
        require(!desktop.setActivation(blockedShortcut, false)
                    && !desktop.enabled() && desktop.shortcut() == priorShortcut
                    && desktop.dragRequired() == priorMode && savedShortcut() == priorShortcut,
                "A recorder conflict persisted the failed candidate or enabled global input");
        desktop.setShortcutCaptureActive(false);
        require(desktop.enabled() && desktop.shortcut() == priorShortcut,
                "Closing the failed recorder did not resume its previous working shortcut");
        require(!desktop.setToolsActivation(blockedShortcut)
                    && desktop.enabled() && desktop.toolsShortcut() == toolsBaseline
                    && desktop.shortcut() == priorShortcut && desktop.dragRequired() == priorMode
                    && savedToolsShortcut() == toolsBaseline && savedToolsDragRequired()
                    && desktop.shortcutError().contains(QStringLiteral("fixture shortcut conflict")),
                "A tools platform conflict failed to restore both live and saved shortcuts");
        desktop.setShortcutCaptureActive(true);
        require(!desktop.setToolsActivation(blockedShortcut)
                    && !desktop.enabled() && desktop.toolsShortcut() == toolsBaseline
                    && desktop.shortcut() == priorShortcut && savedToolsShortcut() == toolsBaseline,
                "A tools recorder conflict persisted a failed binding or activated input");
        desktop.setShortcutCaptureActive(false);
        require(desktop.enabled(), "Closing a failed tools recorder did not resume the previous pair");
        blockedShortcut.clear();
        require(desktop.resetShortcut() && desktop.shortcut() == QStringLiteral("Shift")
                    && desktop.dragRequired() && desktop.enabled()
                    && savedShortcut() == QStringLiteral("Shift") && savedDragRequired(),
                "Reset did not restore and persist the original Shift drag gesture");
        require(desktop.toolsShortcut() == toolsBaseline && savedToolsShortcut() == toolsBaseline
                    && desktop.setToolsActivation(QStringLiteral("Alt"))
                    && desktop.shortcut() == QStringLiteral("Shift") && desktop.dragRequired()
                    && savedToolsShortcut() == QStringLiteral("Alt") && savedToolsDragRequired(),
                "Restoring conversion or tools defaults changed the other shortcut");

        QSettings actual(QSettings::IniFormat, QSettings::UserScope,
                         QStringLiteral("Winux"), QStringLiteral("Maracuja"));
        require(QFileInfo(actual.fileName()).absoluteFilePath().startsWith(temporary.path() + QLatin1Char('/')),
                "The test escaped its private preferences directory");
        actual.setValue(QStringLiteral("activation/shortcut"), QStringLiteral("Alt"));
        actual.setValue(QStringLiteral("activation/dragRequired"), true);
        actual.setValue(QStringLiteral("toolsActivation/shortcut"), QStringLiteral("Alt"));
        actual.sync();
        DesktopIntegration legacy;
        require(legacy.shortcut() == QStringLiteral("Alt") && legacy.dragRequired()
                    && legacy.toolsShortcut() == QStringLiteral("Ctrl+Alt"),
                "A legacy Alt conversion binding was overwritten or collided with the new tools default");
        std::puts("{\"test\":\"desktop-shortcut-transactions\",\"persistence\":true,"
                  "\"invalid_preserves_previous\":true,\"capture_cancel_resume\":true,"
                  "\"disabled_stays_disabled\":true,\"save_stays_suspended\":true,"
                  "\"capture_activation_suppressed\":true,\"platform_conflict_rollback\":true,"
                  "\"independent_tools_persistence\":true,\"tools_conflict_rollback\":true,"
                  "\"two_gesture_duplicate_rejected\":true,\"legacy_alt_preserved\":true,"
                  "\"wheel_appearance_persistence\":true,\"wheel_appearance_validation\":true,"
                  "\"wheel_color_normalization\":true,\"wheel_color_legacy_setters\":true,"
                  "\"wheel_appearance_save_rollback\":true,\"wheel_appearance_no_shortcut_restart\":true,"
                  "\"reset_default\":true,\"global_input_used\":false}");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Desktop shortcut transaction check failed: %s\n", error.what());
        return 1;
    }
}

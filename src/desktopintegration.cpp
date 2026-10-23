#include "desktopintegration.h"
#include "converter.h"
#include "filetoolscontroller.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QVariantMap>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>
#include <QProcessEnvironment>
#include <QDebug>
#include <QKeySequence>
#include <QUuid>
#include <QEventLoop>

namespace {
constexpr auto effectId = "mandarina-shift";
}

DesktopIntegration::DesktopIntegration(QObject *parent) : QObject(parent) {
    loadActivation();
    QDBusConnection::sessionBus().connect("org.kde.kglobalaccel", QString(),
        "org.kde.kglobalaccel.Component", "globalShortcutPressed", this,
        SLOT(globalShortcutPressed(QString,QString,qlonglong)));
    m_startupRetry.setSingleShot(true);
    m_startupRetry.setInterval(1200);
    connect(&m_startupRetry, &QTimer::timeout, this, &DesktopIntegration::start);
    connect(&m_helper, &QProcess::readyReadStandardOutput, this, [this] { readMessages(m_helper,m_messages,false); });
    connect(&m_toolsHelper, &QProcess::readyReadStandardOutput, this, [this] { readMessages(m_toolsHelper,m_toolsMessages,true); });
    const auto helperFinished = [this] {
        if (m_enabled) {
            setEnabled(false);
            m_status = tr("A roda foi encerrada. Reative o controle para tentar novamente.");
            emit changed();
        }
    };
    for (auto *helper : {&m_helper,&m_toolsHelper}) {
        connect(helper, &QProcess::finished, this, helperFinished);
        if (qEnvironmentVariableIsSet("MANDARINA_ISOLATED_TEST"))
            helper->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        else helper->setStandardErrorFile(QProcess::nullDevice());
    }
}
DesktopIntegration::~DesktopIntegration() { stop(); }

void DesktopIntegration::start() {
    if (m_configuring) { m_startupRetry.start(); return; }
    ++m_startupAttempts;
    const auto package = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
        "kwin/effects/mandarina-shift/metadata.json");
    const auto helperPath = QCoreApplication::applicationDirPath() + "/maracuja-gesture-helper";
    m_available = QGuiApplication::platformName().startsWith("wayland") && !package.isEmpty() && QFileInfo::exists(helperPath);
    if (!m_available) {
        m_status = tr("O atalho precisa do KDE Wayland e da integração instalada.");
        emit changed();
        return;
    }
    // The compositor may still be starting when KDE launches autostart apps.
    m_starting = true;
    setEnabled(true);
    m_starting = false;
    if (!m_enabled && m_startupAttempts < 10) m_startupRetry.start();
}

void DesktopIntegration::setEnabled(bool enabled) {
    // An explicit user choice cancels any pending startup retry, including
    // choosing disabled while the initial activation has not succeeded yet.
    if (!m_starting) m_startupRetry.stop();
    if (enabled == m_enabled || (enabled && !m_available)) return;
    QDBusInterface effects("org.kde.KWin", "/Effects", "org.kde.kwin.Effects");
    effects.setTimeout(2000);
    if (enabled) {
        if (!m_hintsLoaded) {
            const auto hintsPath = QStandardPaths::locate(QStandardPaths::GenericDataLocation, "kwin/effects/mandarina-shift/window-hints.js");
            QDBusInterface scripting("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting");
            scripting.setTimeout(2000);
            scripting.call("unloadScript", "mandarina-window-hints");
            const QDBusReply<int> script = scripting.call("loadScript", hintsPath, "mandarina-window-hints");
            if (script.isValid() && script.value() >= 0) {
                QDBusInterface run("org.kde.KWin", QString("/Scripting/Script%1").arg(script.value()), "org.kde.kwin.Script");
                auto reply = run.call("run");
                if (reply.type() == QDBusMessage::ErrorMessage) {
                    QDBusInterface fallback("org.kde.KWin", QString("/%1").arg(script.value()), "org.kde.kwin.Script");
                    reply = fallback.call("run");
                }
                m_hintsLoaded = reply.type() != QDBusMessage::ErrorMessage;
            }
            if (!m_hintsLoaded) {
                scripting.call("unloadScript", "mandarina-window-hints");
                m_status = tr("Não foi possível preparar a roda do Maracujá no KDE.");
                emit changed();
                return;
            }
        }
        for (auto *helper : {&m_helper,&m_toolsHelper}) if (helper->state() == QProcess::NotRunning) {
            (helper == &m_toolsHelper ? m_toolsMessages : m_messages).clear();
            auto environment = QProcessEnvironment::systemEnvironment();
            // Only the isolated virtual-desktop test inspects frame requests;
            // normal desktop sessions keep the helper's diagnostics disabled.
            if (qEnvironmentVariableIsSet("MANDARINA_ISOLATED_TEST"))
                environment.insert("WAYLAND_DEBUG", "client");
            helper->setProcessEnvironment(environment);
            helper->start(QCoreApplication::applicationDirPath() + "/maracuja-gesture-helper",
                helper == &m_toolsHelper ? QStringList{"--tools"} : QStringList{});
            if (!helper->waitForStarted(2000)) {
                stop();
                m_status = tr("Não foi possível iniciar a roda de conversões.");
                emit changed();
                return;
            }
        }
        applyWheelAppearance();
        // Recover an orphaned instance of our transient effect after an
        // abnormal exit, and load the installed version for this session.
        effects.call("unloadEffect", effectId);
        m_shortcutAction.clear();
        m_toolsShortcutAction.clear();
        m_shortcutComponent.clear();
        if (!prepareKdeShortcut()) { emit changed(); return; }
        const QDBusReply<bool> loaded = effects.call("loadEffect", effectId);
        m_enabled = loaded.isValid() && loaded.value();
        // registerShortcut has no result in ScriptedEffect. Confirm the actual
        // binding belongs to this session before claiming it works.
        const QList<QPair<ActivationShortcut,QString>> registrations{{m_activation,m_shortcutAction},{m_toolsActivation,m_toolsShortcutAction}};
        for (int index=0; index<registrations.size() && m_enabled; ++index) {
            const auto &registration=registrations[index];
            if (!registration.first.key) continue;
            QDBusInterface accel("org.kde.kglobalaccel", "/kglobalaccel", "org.kde.KGlobalAccel");
            accel.setTimeout(1500);
            const int combined = QKeyCombination(registration.first.modifiers, Qt::Key(registration.first.key)).toCombined();
            bool owned = false;
            for (int attempt = 0; attempt < 10; ++attempt) {
                const QDBusReply<QStringList> action = accel.call("action", combined);
                if (!action.isValid()) break;
                if (action.isValid() && action.value().size() >= 2 && action.value()[1] == registration.second) {
                    if (index == 0) m_shortcutComponent = action.value()[0];
                    owned = true; break;
                }
                QEventLoop wait;
                QTimer::singleShot(40, &wait, &QEventLoop::quit);
                wait.exec(QEventLoop::ExcludeUserInputEvents);
            }
            if (!owned) { effects.call("unloadEffect", effectId); m_enabled = false; }
        }
        m_status = m_enabled ? (m_activation.requireDrag
            ? tr("Arraste um arquivo, pressione %1 e solte em um formato.").arg(shortcutLabel())
            : tr("Pressione %1 para abrir a roda do Maracujá.").arg(shortcutLabel()))
            : tr("Não foi possível reservar %1 no KDE. Escolha outro atalho.").arg(shortcutLabel());
        if(m_enabled) m_status += tr(" Ferramentas: %1 + arrastar.").arg(toolsShortcutLabel());
        updateBusy();
        if (!m_enabled) stop();
    } else {
        effects.call("unloadEffect", effectId);
        m_shortcutAction.clear();
        m_toolsShortcutAction.clear();
        m_shortcutComponent.clear();
        m_enabled = false;
        m_status = tr("Atalho do Maracujá desativado nesta sessão.");
        updateBusy();
    }
    emit changed();
}

bool DesktopIntegration::prepareKdeShortcut()
{
    for (int index=0; index<2; ++index) {
        const auto &activation = index == 0 ? m_activation : m_toolsActivation;
        if (!activation.key) continue;
        QDBusInterface accel("org.kde.kglobalaccel", "/kglobalaccel", "org.kde.KGlobalAccel");
        accel.setTimeout(1500);
        const int combined = QKeyCombination(activation.modifiers, Qt::Key(activation.key)).toCombined();
        const QDBusReply<bool> available = accel.call("isGlobalShortcutAvailable", combined, QString());
        if (!available.isValid() || !available.value()) {
            m_status = available.isValid()
                ? tr("%1 já está em uso pelo KDE ou por outro aplicativo. Escolha outro atalho.").arg(activation.display())
                : tr("Não foi possível consultar os atalhos do KDE. Tente novamente.");
            return false;
        }
        auto &action = index == 0 ? m_shortcutAction : m_toolsShortcutAction;
        action = (index == 0 ? "_k_session:maracuja-wheel-" : "_k_session:maracuja-tools-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    const auto writer = QStandardPaths::findExecutable("kwriteconfig6");
    if (writer.isEmpty()) { m_status = tr("O configurador de atalhos do KDE não foi encontrado."); return false; }
    const QList<QPair<QString, QString>> values{
        {"TriggerMode", m_activation.requireDrag ? "drag" : "direct"},
        {"TriggerModifiers", QString::number(int(m_activation.modifiers))},
        {"TriggerShortcut", m_activation.key ? shortcut() : QString()},
        {"ShortcutAction", m_shortcutAction},
        {"ToolsEnabled", "true"},
        {"ToolsModifiers", QString::number(int(m_toolsActivation.modifiers))},
        {"ToolsShortcut", m_toolsActivation.key ? toolsShortcut() : QString()},
        {"ToolsShortcutAction", m_toolsShortcutAction}
    };
    for (const auto &value : values) {
        QProcess process;
        process.start(writer, {"--file", "kwinrc", "--group", "Effect-mandarina-shift",
            "--key", value.first, value.second});
        if (!process.waitForFinished(2000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(500); }
            m_status = tr("Não foi possível aplicar a configuração da roda no KDE.");
            return false;
        }
    }
    return true;
}

void DesktopIntegration::globalShortcutPressed(const QString &component, const QString &action, qlonglong timestamp)
{
    Q_UNUSED(timestamp);
    if (!m_shortcutAction.isEmpty() && action == m_shortcutAction && component == m_shortcutComponent)
        activateShortcut();
}

void DesktopIntegration::stop() {
    m_startupRetry.stop();
    if (m_enabled) setEnabled(false);
    for (auto *helper : {&m_helper,&m_toolsHelper}) if (helper->state() != QProcess::NotRunning) {
        helper->closeWriteChannel();
        if (!helper->waitForFinished(500)) {
            helper->terminate();
            if (!helper->waitForFinished(500)) {
                helper->kill();
                helper->waitForFinished(500);
            }
        }
    }
    if (m_hintsLoaded) {
        QDBusInterface scripting("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting");
        scripting.setTimeout(2000);
        scripting.call("unloadScript", "mandarina-window-hints");
        m_hintsLoaded = false;
    }
}

void DesktopIntegration::setConverter(Converter *converter) {
    m_converter = converter;
    connect(converter, &Converter::busyChanged, this, [this] {
        updateBusy();
        if (!m_converter->busy() && m_gestureJob) {
            notifyFinished(m_converter->outputs().size(), !m_converter->errors().isEmpty());
            m_gestureJob = false;
        }
    });
}

void DesktopIntegration::setToolsBusy(bool busy) { m_toolsBusy=busy; updateBusy(); }
void DesktopIntegration::applyWheelAppearance()
{
    const QJsonObject appearance{{"type", "appearance"}, {"transparency", m_wheelTransparency},
                                {"size", m_wheelSize}, {"color", m_wheelColor}};
    send(appearance, false);
    send(appearance, true);
}
void DesktopIntegration::updateBusy() {
    const bool busy = !m_enabled || m_toolsBusy || (m_converter && m_converter->busy());
    send({{"type","busy"},{"value",busy}},false);
    send({{"type","busy"},{"value",busy}},true);
}
void DesktopIntegration::send(const QJsonObject &message, bool tools) {
    auto &process = tools ? m_toolsHelper : m_helper;
    if (process.state() == QProcess::Running)
        process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void DesktopIntegration::readMessages(QProcess &process, QByteArray &messages, bool tools) {
    messages += process.readAllStandardOutput();
    if (messages.size() > 1024 * 1024) { messages.clear(); return; }
    while (messages.contains('\n')) {
        const auto end = messages.indexOf('\n');
        const auto object = QJsonDocument::fromJson(messages.left(end)).object();
        messages.remove(0, end + 1);
        if (!m_converter || !m_enabled) continue;
        const auto type = object.value("type").toString();
        if (qEnvironmentVariableIsSet("MANDARINA_ISOLATED_TEST")) qInfo() << "PRIVATE helper message" << type;
        if (type == "hover") {
            const int sector = object.value("sector").toInt(-1);
            if (sector >= 0 && sector < 16 && !m_toolsBusy && !m_converter->busy())
                playWheelHoverSound();
            continue;
        }
        QVariantList urls;
        for (const auto &path : object.value("paths").toArray())
            urls << QUrl::fromLocalFile(path.toString());
        if (tools) {
            const auto paths=FileToolsController::localFiles(urls);
            const bool allowed=!m_toolsBusy && !m_converter->busy() && !paths.isEmpty();
            auto formats = allowed ? FileToolsController::toolsFormatsForFiles(paths) : QVariantList{};
            // A filtered subset must never authorize photo editing for a mixed
            // incoming selection. The ordinary tools keep their existing route.
            if (urls.size() != 1 && formats.size() == 3) formats.removeLast();
            if (type == "preview") send({{"type","formats"},{"token",object.value("token")},
                {"formats",QJsonArray::fromVariantList(formats)}},true);
            else if (type == "drop" && allowed) {
                const auto id=object.value("format").toString();
                bool valid = false;
                for (const auto &entry : formats)
                    if (entry.toMap().value("id").toString() == id) valid = true;
                if(valid) {
                    if(qEnvironmentVariableIsSet("MANDARINA_ISOLATED_TEST")) qInfo() << "PRIVATE tools drop" << id << paths.size();
                    emit toolsDropRequested(paths,id.mid(5));
                }
            }
            continue;
        }
        if (type == "preview") {
            const auto formats = m_toolsBusy ? QVariantList{} : m_converter->formatsForUrls(urls);
            send({{"type", "formats"}, {"token", object.value("token")}, {"formats", QJsonArray::fromVariantList(formats)}});
        } else if (type == "drop" && !m_converter->busy() && !m_toolsBusy) {
            const auto format = object.value("format").toString();
            bool valid = false;
            for (const auto &entry : m_converter->formatsForUrls(urls))
                if (entry.toMap().value("id").toString() == format) valid = true;
            if (valid) {
                m_gestureJob = true;
                m_converter->convertUrls(urls, format);
                if (!m_converter->busy()) m_gestureJob = false;
            }
        }
        // Dismissing the global wheel has no panel preview to clear.
    }
}

void DesktopIntegration::OpenFiles(const QStringList &paths, bool compact) {
    emit openRequested(paths, compact);
}

void DesktopIntegration::ProbeOpened() {
    if (m_enabled) send({{"type", "arm"}});
}
void DesktopIntegration::ToolsProbeOpened() { if(m_enabled) send({{"type","arm"}},true); }

void DesktopIntegration::Quit() {
    emit quitRequested();
}

void DesktopIntegration::notifyFinished(int outputs, bool errors) {
    auto message = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify");
    message << QStringLiteral("Maracujá") << uint(0) << QStringLiteral("org.winux.Maracuja")
            << (errors ? tr("Confira a conversão no Maracujá") : tr("Conversão concluída"))
            << (errors ? tr("Abra o Maracujá para conferir os detalhes. Seus originais foram preservados.")
                       : tr("%1 resultado(s) criado(s). Os originais foram preservados.").arg(outputs))
            << QStringList{} << QVariantMap{} << 5000;
    QDBusConnection::sessionBus().asyncCall(message);
}

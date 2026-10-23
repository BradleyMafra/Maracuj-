#pragma once

#include <QObject>
#include <QStringList>
#include <QProcess>
#include <QTimer>
#include "shortcutconfig.h"

class Converter;
class QJsonObject;
class WindowsGesture;
class FileToolsController;
class WheelSound;

class DesktopIntegration final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.winux.Mandarina")
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString shortcut READ shortcut NOTIFY changed)
    Q_PROPERTY(QString shortcutLabel READ shortcutLabel NOTIFY changed)
    Q_PROPERTY(bool dragRequired READ dragRequired NOTIFY changed)
    Q_PROPERTY(QString shortcutError READ shortcutError NOTIFY changed)
    Q_PROPERTY(QString toolsShortcut READ toolsShortcut NOTIFY changed)
    Q_PROPERTY(QString toolsShortcutLabel READ toolsShortcutLabel NOTIFY changed)
    Q_PROPERTY(int wheelTransparency READ wheelTransparency NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(int wheelSize READ wheelSize NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(QString wheelColor READ wheelColor NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(bool wheelSoundEnabled READ wheelSoundEnabled NOTIFY wheelAppearanceChanged)
    Q_PROPERTY(QString wheelAppearanceError READ wheelAppearanceError NOTIFY wheelAppearanceChanged)
public:
    explicit DesktopIntegration(QObject *parent = nullptr);
    ~DesktopIntegration() override;
    bool available() const { return m_available; }
    bool enabled() const { return m_enabled; }
    QString status() const { return m_status; }
    QString shortcut() const { return m_activation.portable(); }
    QString shortcutLabel() const { return m_activation.display(); }
    bool dragRequired() const { return m_activation.requireDrag; }
    QString shortcutError() const { return m_shortcutError; }
    QString toolsShortcut() const { return m_toolsActivation.portable(); }
    QString toolsShortcutLabel() const { return m_toolsActivation.display(); }
    int wheelTransparency() const { return m_wheelTransparency; }
    int wheelSize() const { return m_wheelSize; }
    QString wheelColor() const { return m_wheelColor; }
    bool wheelSoundEnabled() const { return m_wheelSoundEnabled; }
    QString wheelAppearanceError() const { return m_wheelAppearanceError; }
    Q_INVOKABLE bool setWheelTransparency(int percent);
    Q_INVOKABLE bool setWheelSize(int percent);
    Q_INVOKABLE bool setWheelColor(const QString &color);
    Q_INVOKABLE bool setWheelSoundEnabled(bool enabled);
    Q_INVOKABLE bool setWheelAppearance(int transparency, int size);
    Q_INVOKABLE bool setWheelAppearance(int transparency, int size, const QString &color);
    Q_INVOKABLE bool setWheelAppearance(int transparency, int size, const QString &color, bool soundEnabled);
    Q_INVOKABLE void playWheelHoverSound();
    Q_INVOKABLE QString shortcutDisplay(const QString &value) const;
    Q_INVOKABLE QString shortcutFromKey(int key, int modifiers) const;
    Q_INVOKABLE QString validateShortcut(const QString &value, bool requireDrag) const;
    Q_INVOKABLE bool setActivation(const QString &value, bool requireDrag);
    Q_INVOKABLE bool resetShortcut();
    Q_INVOKABLE bool setToolsActivation(const QString &value);
    Q_INVOKABLE QString validateToolsShortcut(const QString &value) const;
    Q_INVOKABLE void setShortcutCaptureActive(bool active);
    void start();
    void setConverter(Converter *converter);
    void setToolsBusy(bool busy);
    void setEnabled(bool enabled);
    void stop();
    void notifyFinished(int outputs, bool errors);
public slots:
    Q_SCRIPTABLE void OpenFiles(const QStringList &paths, bool compact);
    Q_SCRIPTABLE void ProbeOpened();
    Q_SCRIPTABLE void ToolsProbeOpened();
    Q_SCRIPTABLE void Quit();
signals:
    void changed();
    void wheelAppearanceChanged();
    void openRequested(const QStringList &paths, bool compact);
    void toolsDropRequested(const QStringList &paths, const QString &tool);
    void quitRequested();
    void notificationRequested(const QString &title, const QString &message);
private:
    void loadActivation();
    void applyWheelAppearance();
    int m_wheelTransparency = 35;
    int m_wheelSize = 100;
    QString m_wheelColor = QStringLiteral("#b99cde");
    bool m_wheelSoundEnabled = true;
    WheelSound *m_wheelSound = nullptr;
    QString m_wheelAppearanceError;
    ActivationShortcut m_activation;
    ActivationShortcut m_toolsActivation{Qt::AltModifier, 0, true};
    bool setActivationImpl(const QString &value, bool requireDrag, bool tools);
    QString m_shortcutError;
    bool m_configuring = false;
    bool m_resumeAfterConfiguration = false;
    void activateShortcut();
#ifndef Q_OS_WIN
    bool prepareKdeShortcut();
    QString m_shortcutAction;
    QString m_shortcutComponent;
    QString m_toolsShortcutAction;
private slots:
    void globalShortcutPressed(const QString &component, const QString &action, qlonglong timestamp);
private:
#endif
#ifdef Q_OS_WIN
    WindowsGesture *m_windowsGesture = nullptr;
#endif
    bool m_available = false;
    bool m_toolsBusy = false;
    bool m_enabled = false;
    QString m_status;
    Converter *m_converter = nullptr;
    QProcess m_helper;
    QProcess m_toolsHelper;
    QByteArray m_messages;
    QByteArray m_toolsMessages;
    bool m_gestureJob = false;
    bool m_hintsLoaded = false;
    QTimer m_startupRetry;
    int m_startupAttempts = 0;
    bool m_starting = false;
    void send(const QJsonObject &message, bool tools = false);
    void updateBusy();
    void readMessages(QProcess &process, QByteArray &messages, bool tools);
};

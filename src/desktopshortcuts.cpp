#include "desktopintegration.h"
#include "wheelsound.h"
#include <QSettings>

namespace {
QSettings preferences()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("Winux"), QStringLiteral("Maracuja"));
}

QString normalizedWheelColor(const QString &color)
{
    if (color.size() != 7 || color.front() != QLatin1Char('#')) return {};
    for (qsizetype index = 1; index < color.size(); ++index) {
        const auto character = color.at(index).unicode();
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f')
              || (character >= 'A' && character <= 'F'))) return {};
    }
    return color.toLower();
}
}

void DesktopIntegration::loadActivation()
{
    auto settings = preferences();
    bool transparencyValid = false;
    const int transparency = settings.value("appearance/wheelTransparency", 35)
                                 .toString().toInt(&transparencyValid);
    m_wheelTransparency = transparencyValid && transparency >= 0 && transparency <= 80
                              ? transparency : 35;
    bool sizeValid = false;
    const int size = settings.value("appearance/wheelSize", 100).toString().toInt(&sizeValid);
    m_wheelSize = sizeValid && size >= 70 && size <= 130 ? size : 100;
    const QString color = normalizedWheelColor(settings.value("appearance/wheelColor").toString());
    m_wheelColor = color.isEmpty() ? QStringLiteral("#b99cde") : color;
    m_wheelSoundEnabled = settings.value("appearance/wheelSoundEnabled", true).toBool();
    ActivationShortcut loaded;
    if (ActivationShortcut::parse(settings.value("activation/shortcut", "Shift").toString(),
                                  settings.value("activation/dragRequired", true).toBool(), &loaded, nullptr))
        m_activation = loaded;
    if (ActivationShortcut::parse(settings.value("toolsActivation/shortcut", "Alt").toString(), true, &loaded, nullptr))
        m_toolsActivation = loaded;
    // A legacy conversion binding may already use Alt. Keep it and choose a
    // distinct tools default until the user configures the second gesture.
    if (m_toolsActivation.portable() == m_activation.portable())
        ActivationShortcut::parse("Ctrl+Alt", true, &m_toolsActivation, nullptr);
}

bool DesktopIntegration::setWheelTransparency(int percent)
{ return setWheelAppearance(percent, m_wheelSize, m_wheelColor); }

bool DesktopIntegration::setWheelSize(int percent)
{ return setWheelAppearance(m_wheelTransparency, percent, m_wheelColor); }

bool DesktopIntegration::setWheelColor(const QString &color)
{ return setWheelAppearance(m_wheelTransparency, m_wheelSize, color); }

bool DesktopIntegration::setWheelSoundEnabled(bool enabled)
{ return setWheelAppearance(m_wheelTransparency, m_wheelSize, m_wheelColor, enabled); }

void DesktopIntegration::playWheelHoverSound()
{
    if (!m_wheelSoundEnabled) return;
    if (!m_wheelSound) m_wheelSound = new WheelSound(this);
    m_wheelSound->play();
}

bool DesktopIntegration::setWheelAppearance(int transparency, int size)
{ return setWheelAppearance(transparency, size, m_wheelColor); }

bool DesktopIntegration::setWheelAppearance(int transparency, int size, const QString &color)
{ return setWheelAppearance(transparency, size, color, m_wheelSoundEnabled); }

bool DesktopIntegration::setWheelAppearance(int transparency, int size, const QString &color, bool soundEnabled)
{
    if (transparency < 0 || transparency > 80) {
        m_wheelAppearanceError = tr("Escolha uma transparência entre 0% e 80%.");
        emit wheelAppearanceChanged();
        return false;
    }
    if (size < 70 || size > 130) {
        m_wheelAppearanceError = tr("Escolha um tamanho entre 70% e 130%.");
        emit wheelAppearanceChanged();
        return false;
    }
    const QString normalizedColor = normalizedWheelColor(color);
    if (normalizedColor.isEmpty()) {
        m_wheelAppearanceError = tr("Escolha uma cor no formato #RRGGBB.");
        emit wheelAppearanceChanged();
        return false;
    }

    auto settings = preferences();
    const QString transparencyKey = QStringLiteral("appearance/wheelTransparency");
    const QString sizeKey = QStringLiteral("appearance/wheelSize");
    const QString colorKey = QStringLiteral("appearance/wheelColor");
    const QString soundKey = QStringLiteral("appearance/wheelSoundEnabled");
    const auto previousTransparency = settings.value(transparencyKey);
    const auto previousSize = settings.value(sizeKey);
    const auto previousColor = settings.value(colorKey);
    const auto previousSound = settings.value(soundKey);
    // QSettings writes the INI file atomically at sync, so the four preferences
    // become visible together. Live renderers change only after it succeeds.
    settings.setValue(transparencyKey, transparency);
    settings.setValue(sizeKey, size);
    settings.setValue(colorKey, normalizedColor);
    settings.setValue(soundKey, soundEnabled);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (previousTransparency.isValid()) settings.setValue(transparencyKey, previousTransparency);
        else settings.remove(transparencyKey);
        if (previousSize.isValid()) settings.setValue(sizeKey, previousSize);
        else settings.remove(sizeKey);
        if (previousColor.isValid()) settings.setValue(colorKey, previousColor);
        else settings.remove(colorKey);
        if (previousSound.isValid()) settings.setValue(soundKey, previousSound);
        else settings.remove(soundKey);
        settings.sync();
        m_wheelAppearanceError = tr("Não foi possível salvar a aparência. A configuração anterior foi preservada.");
        emit wheelAppearanceChanged();
        return false;
    }

    m_wheelTransparency = transparency;
    m_wheelSize = size;
    m_wheelColor = normalizedColor;
    m_wheelSoundEnabled = soundEnabled;
    m_wheelAppearanceError.clear();
    applyWheelAppearance();
    emit wheelAppearanceChanged();
    return true;
}

QString DesktopIntegration::shortcutDisplay(const QString &value) const
{
    ActivationShortcut parsed;
    return ActivationShortcut::parse(value, true, &parsed, nullptr) ? parsed.display() : value;
}

QString DesktopIntegration::shortcutFromKey(int key, int modifiers) const
{
    return ActivationShortcut::fromKey(key, modifiers);
}

QString DesktopIntegration::validateShortcut(const QString &value, bool requireDrag) const
{
    ActivationShortcut parsed;
    QString error;
    ActivationShortcut::parse(value, requireDrag, &parsed, &error);
    if (error.isEmpty() && parsed.portable() == toolsShortcut())
        error = tr("Escolha atalhos diferentes para conversão e ferramentas.");
#ifdef Q_OS_WIN
    if (error.isEmpty() && !requireDrag && parsed.key == Qt::Key_F12)
        error = tr("O Windows reserva F12 para o depurador. Escolha outra tecla sem arraste.");
#endif
    return error;
}

bool DesktopIntegration::setActivation(const QString &value, bool requireDrag)
{ return setActivationImpl(value, requireDrag, false); }

QString DesktopIntegration::validateToolsShortcut(const QString &value) const
{
    ActivationShortcut parsed;
    QString error;
    if (ActivationShortcut::parse(value, true, &parsed, &error) && parsed.portable() == shortcut())
        error = tr("Escolha atalhos diferentes para conversão e ferramentas.");
    return error;
}

bool DesktopIntegration::setToolsActivation(const QString &value)
{ return setActivationImpl(value, true, true); }

bool DesktopIntegration::setActivationImpl(const QString &value, bool requireDrag, bool tools)
{
    m_shortcutError = tools ? validateToolsShortcut(value) : validateShortcut(value, requireDrag);
    ActivationShortcut candidate;
    if (!m_shortcutError.isEmpty() || !ActivationShortcut::parse(value, requireDrag, &candidate, &m_shortcutError)) {
        emit changed();
        return false;
    }
    if (candidate.portable() == (tools ? shortcut() : toolsShortcut())) {
        m_shortcutError = tr("Escolha atalhos diferentes para conversão e ferramentas.");
        emit changed(); return false;
    }
    auto &activation = tools ? m_toolsActivation : m_activation;
    const auto previous = activation;
    const bool wasEnabled = m_enabled;
    const bool shouldEnable = wasEnabled || (m_configuring && m_resumeAfterConfiguration);
    if (wasEnabled) setEnabled(false);
    activation = candidate;
    if (shouldEnable && m_available) {
        setEnabled(true);
        if (!m_enabled) {
            const auto failure = m_status;
            activation = previous;
            if (wasEnabled) setEnabled(true);
            m_shortcutError = failure;
            emit changed();
            return false;
        }
    }
    auto settings = preferences();
    const QString prefix = tools ? "toolsActivation/" : "activation/";
    const auto oldValue = settings.value(prefix + "shortcut");
    const auto oldMode = settings.value(prefix + "dragRequired");
    settings.setValue(prefix + "shortcut", candidate.portable());
    settings.setValue(prefix + "dragRequired", candidate.requireDrag);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (m_enabled) setEnabled(false);
        activation = previous;
        if (oldValue.isValid()) settings.setValue(prefix + "shortcut", oldValue);
        else settings.remove(prefix + "shortcut");
        if (oldMode.isValid()) settings.setValue(prefix + "dragRequired", oldMode);
        else settings.remove(prefix + "dragRequired");
        settings.sync();
        if (wasEnabled) setEnabled(true);
        m_shortcutError = tr("Não foi possível salvar a configuração. O atalho anterior foi preservado.");
        emit changed();
        return false;
    }
    // Recording stays local until the dialog closes, including after a save.
    if (m_configuring && m_enabled) setEnabled(false);
    m_shortcutError.clear();
    emit changed();
    return true;
}

bool DesktopIntegration::resetShortcut() { return setActivation(QStringLiteral("Shift"), true); }

void DesktopIntegration::setShortcutCaptureActive(bool active)
{
    if (active == m_configuring) return;
    if (active) {
        m_resumeAfterConfiguration = m_enabled;
        m_configuring = true;
        setEnabled(false);
    } else {
        m_configuring = false;
        if (m_resumeAfterConfiguration) setEnabled(true);
        m_resumeAfterConfiguration = false;
    }
}

void DesktopIntegration::activateShortcut()
{
    if (m_enabled && !m_activation.requireDrag && !m_configuring && !m_toolsBusy)
        emit openRequested({}, true);
}

#include "desktopintegration.h"
#include "windowsgesture.h"
#include "converter.h"

DesktopIntegration::DesktopIntegration(QObject *parent) : QObject(parent) { loadActivation(); }
DesktopIntegration::~DesktopIntegration() { stop(); }
void DesktopIntegration::setConverter(Converter *converter)
{
    m_converter = converter;
    m_windowsGesture = new WindowsGesture(converter, this);
    applyWheelAppearance();
    connect(m_windowsGesture, &WindowsGesture::changed, this, [this] {
        m_available = m_windowsGesture->available();
        m_enabled = m_windowsGesture->enabled();
        m_status = m_windowsGesture->status();
        emit changed();
    });
    connect(m_windowsGesture, &WindowsGesture::conversionStarted, this, [this] { m_gestureJob = true; });
    connect(m_windowsGesture, &WindowsGesture::activationRequested, this, &DesktopIntegration::activateShortcut);
    connect(m_windowsGesture, &WindowsGesture::toolsRequested, this, &DesktopIntegration::toolsDropRequested);
    connect(m_windowsGesture, &WindowsGesture::wheelSectorHovered, this, &DesktopIntegration::playWheelHoverSound);
    connect(converter, &Converter::busyChanged, this, [this] {
        if (!m_converter->busy() && m_gestureJob) {
            m_gestureJob = false;
            notifyFinished(m_converter->outputs().size(), !m_converter->errors().isEmpty());
        }
    });
}
void DesktopIntegration::start() {
    if (m_configuring) { QTimer::singleShot(1000, this, &DesktopIntegration::start); return; }
    if (m_windowsGesture) {
        if (!m_windowsGesture->prepare()) return;
        if (!m_windowsGesture->setActivations(m_activation.modifiers, m_activation.key, m_activation.requireDrag,
                                             m_toolsActivation.modifiers,m_toolsActivation.key)) {
            m_status = m_windowsGesture->errorString(); emit changed(); return;
        }
        m_windowsGesture->setEnabled(true);
    }
}
void DesktopIntegration::stop() { if (m_windowsGesture) m_windowsGesture->stop(); }
void DesktopIntegration::setEnabled(bool enabled) {
    if (!m_windowsGesture) return;
    if (enabled && !m_windowsGesture->setActivations(m_activation.modifiers, m_activation.key, m_activation.requireDrag,
                                                  m_toolsActivation.modifiers,m_toolsActivation.key)) {
        m_status = m_windowsGesture->errorString(); emit changed(); return;
    }
    m_windowsGesture->setEnabled(enabled);
}
void DesktopIntegration::OpenFiles(const QStringList &paths, bool compact) { emit openRequested(paths, compact); }
void DesktopIntegration::ProbeOpened() {}
void DesktopIntegration::ToolsProbeOpened() {}
void DesktopIntegration::setToolsBusy(bool busy) { m_toolsBusy=busy; if(m_windowsGesture) m_windowsGesture->setExternalBusy(busy); }
void DesktopIntegration::applyWheelAppearance()
{
    if (m_windowsGesture) {
        m_windowsGesture->setWheelTransparency(m_wheelTransparency);
        m_windowsGesture->setWheelSize(m_wheelSize);
        m_windowsGesture->setWheelColor(m_wheelColor);
    }
}
void DesktopIntegration::Quit() { emit quitRequested(); }
void DesktopIntegration::notifyFinished(int outputs, bool errors)
{
    emit notificationRequested(errors ? tr("Confira a conversão no Maracujá") : tr("Conversão concluída"),
        errors ? tr("Abra o Maracujá para conferir os detalhes. Seus originais foram preservados.")
               : tr("%1 resultado(s) criado(s). Os originais foram preservados.").arg(outputs));
}

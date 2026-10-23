#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

// Static, local support details. Construction never changes the clipboard or
// contacts a payment service; the bank remains responsible for confirmation.
class SupportInfo final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString pixPayload READ pixPayload CONSTANT)
    Q_PROPERTY(QString pixKey READ pixKey CONSTANT)
    Q_PROPERTY(QUrl qrSource READ qrSource CONSTANT)
public:
    explicit SupportInfo(QObject *parent = nullptr);
    SupportInfo(const QString &configurationPath, const QUrl &qrSource, QObject *parent = nullptr);

    bool available() const { return m_available; }
    QString pixPayload() const { return m_payload; }
    QString pixKey() const { return m_key; }
    QUrl qrSource() const { return m_qrSource; }
    Q_INVOKABLE bool copyPix() const;

private:
    bool m_available = false;
    QString m_payload, m_key;
    QUrl m_qrSource;
};

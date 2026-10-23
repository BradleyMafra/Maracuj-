#pragma once
#include <QObject>
#include <QLocalServer>
#include <QLockFile>
#include <QStringList>
#include <memory>

class LocalInstance final : public QObject {
    Q_OBJECT
public:
    explicit LocalInstance(QObject *parent = nullptr, const QString &dataDirectory = {});
    bool acquire();
    bool forward(const QStringList &paths, bool compact, bool background, bool quit);
signals:
    void openRequested(const QStringList &paths, bool compact);
    void quitRequested();
private:
    QString m_name;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer m_server;
};

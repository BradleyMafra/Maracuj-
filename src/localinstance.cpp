#include "localinstance.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>

LocalInstance::LocalInstance(QObject *parent, const QString &dataDirectory) : QObject(parent)
{
    const QString directory = dataDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : dataDirectory;
    QDir().mkpath(directory);
    m_lock = std::make_unique<QLockFile>(QDir(directory).filePath("resident.lock"));
    m_name = "Maracuja-" + QString::fromLatin1(QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = m_server.nextPendingConnection()) {
            socket->setReadBufferSize(256 * 1024 + 1);
            auto data = std::make_shared<QByteArray>();
            auto handled = std::make_shared<bool>(false);
            QTimer::singleShot(3000, socket, [socket] { socket->abort(); socket->deleteLater(); });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            const auto receive = [this, socket, data, handled] {
                if (*handled) return;
                data->append(socket->readAll());
                if (data->size() > 256 * 1024) { socket->abort(); return; }
                const auto end = data->indexOf('\n');
                if (end < 0) return;
                *handled = true;
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(data->left(end), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()) { socket->abort(); return; }
                const auto request = document.object();
                const bool quit = request.value("quit").toBool();
                const bool open = !quit && !request.value("background").toBool();
                QStringList paths;
                if (open) {
                    const auto entries = request.value("paths").toArray();
                    if (entries.size() > 1000) { socket->abort(); return; }
                    for (const auto &entry : entries) {
                        if (!entry.isString() || entry.toString().size() > 32768) { socket->abort(); return; }
                        paths.append(entry.toString());
                    }
                }
                socket->write("OK\n");
                socket->waitForBytesWritten(250);
                socket->disconnectFromServer();
                const bool compact = request.value("compact").toBool();
                QTimer::singleShot(0, this, [this, quit, open, paths, compact] {
                    if (quit) emit quitRequested();
                    else if (open) emit openRequested(paths, compact);
                });
            };
            connect(socket, &QLocalSocket::readyRead, socket, receive);
            if (socket->bytesAvailable() > 0) receive();
        }
    });
}
bool LocalInstance::acquire()
{
    if (!m_lock->tryLock(0)) return false;
    // A held per-user lock prevents deleting another live instance's endpoint.
    QLocalServer::removeServer(m_name);
    return m_server.listen(m_name);
}
bool LocalInstance::forward(const QStringList &paths, bool compact, bool background, bool quit)
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(3000)) return false;
    const auto bytes = QJsonDocument(QJsonObject{{"paths", QJsonArray::fromStringList(paths)},
        {"compact", compact}, {"background", background && paths.isEmpty()}, {"quit", quit}}).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(bytes) != bytes.size() || !socket.waitForBytesWritten(2000)) return false;
    return socket.waitForReadyRead(3000) && socket.readAll().startsWith("OK\n");
}

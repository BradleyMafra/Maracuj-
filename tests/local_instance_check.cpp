#include "localinstance.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <thread>
#include <cstdio>

enum class ReplyFixture { Fragmented, Invalid, Closed };

static bool acknowledgementFixture(ReplyFixture fixture)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    // The fixture uses the same private directory-derived endpoint as its
    // client; it cannot reach the installed application's endpoint.
    const QString name = "Maracuja-" + QString::fromLatin1(QCryptographicHash::hash(
        temporary.path().toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
    QLocalServer peer;
    peer.setSocketOptions(QLocalServer::UserAccessOption);
    if (!peer.listen(name)) return false;
    int accepted = 0;
    QObject::connect(&peer, &QLocalServer::newConnection, &peer, [&] {
        while (auto *socket = peer.nextPendingConnection()) {
            ++accepted;
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            auto received = std::make_shared<bool>(false);
            const auto respond = [socket, fixture, received] {
                if (*received || socket->bytesAvailable() == 0) return;
                *received = true;
                socket->readAll();
                if (fixture == ReplyFixture::Closed) {
                    socket->disconnectFromServer();
                } else if (fixture == ReplyFixture::Invalid) {
                    socket->write("NO\n");
                    socket->flush();
                } else {
                    socket->write("O");
                    socket->flush();
                    QTimer::singleShot(25, socket, [socket] {
                        socket->write("K\n");
                        socket->flush();
                    });
                }
            };
            QObject::connect(socket, &QLocalSocket::readyRead, socket, respond);
            respond();
        }
    });
    std::atomic_bool finished{false};
    bool forwarded = false;
    QEventLoop loop;
    std::thread client([&] {
        LocalInstance instance(nullptr, temporary.path());
        forwarded = instance.forward({}, false, true, false);
        finished.store(true);
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (finished.load()) loop.quit(); });
    poll.start(5);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    client.join();
    return accepted == 1 && forwarded == (fixture == ReplyFixture::Fragmented);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        std::fputs("FAIL: private IPC fixture directory could not be created\n", stderr);
        return 2;
    }
    LocalInstance server(nullptr, temporary.path());
    if (!server.acquire()) {
        std::fputs("FAIL: private IPC fixture server did not acquire its endpoint\n", stderr);
        return 2;
    }
    const QStringList files{QStringLiteral("file:///C:/teste/a%C3%A7%C3%A3o.png"), QStringLiteral("file:///tmp/segundo.pdf")};
    int opened = 0;
    bool correct = true, quit = false;
    QObject::connect(&server, &LocalInstance::openRequested, &app, [&](const QStringList &paths, bool compact) {
        ++opened; correct = correct && paths == files && compact;
    });
    QObject::connect(&server, &LocalInstance::quitRequested, &app, [&] { quit = true; });
    std::atomic_bool clientFinished{false};
    bool clientOk = false;
    bool duplicateRejected = false, backgroundForwarded = false, openForwarded = false, quitForwarded = false;
    QEventLoop loop;
    std::thread client([&] {
        LocalInstance duplicate(nullptr, temporary.path());
        duplicateRejected = !duplicate.acquire();
        backgroundForwarded = duplicateRejected && duplicate.forward({}, false, true, false);
        openForwarded = backgroundForwarded && duplicate.forward(files, true, false, false);
        quitForwarded = openForwarded && duplicate.forward({}, false, false, true);
        clientOk = quitForwarded;
        clientFinished.store(true);
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (clientFinished.load() && quit) loop.quit();
    });
    poll.start(10);
    QTimer::singleShot(12000, &loop, &QEventLoop::quit);
    loop.exec(); client.join();
    if (!clientOk || !correct || opened != 1 || !quit) {
        std::fprintf(stderr, "FAIL: private IPC fixture duplicate=%d background=%d open=%d quit-forward=%d "
                     "payload=%d opened=%d quit-signal=%d\n", duplicateRejected, backgroundForwarded,
                     openForwarded, quitForwarded, correct, opened, quit);
        return 1;
    }
    for (const auto fixture : {ReplyFixture::Fragmented, ReplyFixture::Invalid, ReplyFixture::Closed}) {
        if (!acknowledgementFixture(fixture)) {
            std::fprintf(stderr, "FAIL: private IPC acknowledgement fixture %d\n", int(fixture));
            return 1;
        }
    }
    std::puts("PASS: instância única, encaminhamento Unicode, background oculto e confirmação antes de sair");
    return 0;
}

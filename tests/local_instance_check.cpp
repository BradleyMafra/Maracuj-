#include "localinstance.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <thread>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    LocalInstance server(nullptr, temporary.path());
    if (!server.acquire()) return 2;
    const QStringList files{QStringLiteral("file:///C:/teste/a%C3%A7%C3%A3o.png"), QStringLiteral("file:///tmp/segundo.pdf")};
    int opened = 0;
    bool correct = true, quit = false;
    QObject::connect(&server, &LocalInstance::openRequested, &app, [&](const QStringList &paths, bool compact) {
        ++opened; correct = correct && paths == files && compact;
    });
    QObject::connect(&server, &LocalInstance::quitRequested, &app, [&] { quit = true; });
    std::atomic_bool clientFinished{false};
    bool clientOk = false;
    QEventLoop loop;
    std::thread client([&] {
        LocalInstance duplicate(nullptr, temporary.path());
        clientOk = !duplicate.acquire()
            && duplicate.forward({}, false, true, false)
            && duplicate.forward(files, true, false, false)
            && duplicate.forward({}, false, false, true);
        clientFinished.store(true);
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (clientFinished.load() && quit) loop.quit();
    });
    poll.start(10);
    QTimer::singleShot(12000, &loop, &QEventLoop::quit);
    loop.exec(); client.join();
    if (!clientOk || !correct || opened != 1 || !quit) return 1;
    std::puts("PASS: instância única, encaminhamento Unicode, background oculto e confirmação antes de sair");
    return 0;
}

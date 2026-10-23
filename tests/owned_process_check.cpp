#include "ownedprocess.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <functional>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

bool writeMarker(const QString &path, const QByteArray &data)
{
    QSaveFile marker(path);
    const bool written = marker.open(QIODevice::WriteOnly)
        && marker.write(data) == data.size() && marker.commit();
    if (!written)
        std::fprintf(stderr, "Fixture marker %s failed: %s\n", qPrintable(QFileInfo(path).fileName()),
                     qPrintable(marker.errorString()));
    return written;
}

QByteArray readMarker(const QString &path)
{
    QFile marker(path);
    if (!marker.open(QIODevice::ReadOnly) || marker.size() > 128) return {};
    return marker.readAll().trimmed();
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < timeoutMs) {
        if (condition()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(15);
    }
    return condition();
}

void hideConsole(QProcess &process)
{
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#else
    Q_UNUSED(process);
#endif
}

bool stillRunning(qint64 pid)
{
    if (pid <= 1) return false;
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#elif defined(Q_OS_LINUX)
    // The test is a subreaper: it can reap only its own orphaned fixture netos.
    int status = 0;
    if (::waitpid(pid_t(pid), &status, WNOHANG) == pid_t(pid)) return false;
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

int heartbeat(const QDir &directory, const QString &role)
{
    if (!writeMarker(directory.filePath(role + ".started"),
                     QByteArray::number(QCoreApplication::applicationPid()))) return 2;
    QElapsedTimer deadline;
    deadline.start();
    int ticks = 0;
    // A failing parent test cannot leave an unbounded test fixture behind.
    while (deadline.elapsed() < 20000 && !QFileInfo::exists(directory.filePath("stop"))) {
        if (!writeMarker(directory.filePath(role + ".heartbeat"), QByteArray::number(++ticks))) return 2;
        QThread::msleep(30);
    }
    return 0;
}

int childFixture(const QDir &directory, bool exitAfterSpawn)
{
    if (!writeMarker(directory.filePath("child.started"),
                     QByteArray::number(QCoreApplication::applicationPid()))) return 2;
    // Deliberately do not use OwnedProcess here: the neto must inherit the
    // parent's existing session/job, exactly like soffice/Poppler descendants.
    QProcess *grandchild = new QProcess;
    hideConsole(*grandchild);
    grandchild->setProgram(QCoreApplication::applicationFilePath());
    grandchild->setArguments({"--grandchild", directory.absolutePath()});
    grandchild->start(QIODevice::ReadOnly);
    if (!grandchild->waitForStarted(5000)
        || !waitUntil([&] { return QFileInfo::exists(directory.filePath("grandchild.started")); })) {
        grandchild->kill();
        grandchild->waitForFinished(2000);
        delete grandchild;
        return 2;
    }
    if (!writeMarker(directory.filePath("child.ready"), "ready")) return 2;
    if (exitAfterSpawn) {
        // Skip QProcess's destructor, which would kill its direct child and
        // accidentally hide an orphan-descendant cleanup bug in OwnedProcess.
        std::_Exit(0);
    }
    const int result = heartbeat(directory, "child");
    if (!grandchild->waitForFinished(2000)) {
        grandchild->kill();
        grandchild->waitForFinished(2000);
    }
    delete grandchild;
    return result;
}

struct UnrelatedFixture {
    QDir directory;
    QProcess process;
    explicit UnrelatedFixture(const QString &path) : directory(path)
    {
        QDir().mkpath(path);
        hideConsole(process);
        process.setProgram(QCoreApplication::applicationFilePath());
        process.setArguments({"--unrelated", path});
    }
    ~UnrelatedFixture()
    {
        writeMarker(directory.filePath("stop"), "stop");
        if (!process.waitForFinished(2000) && process.state() != QProcess::NotRunning) {
            process.kill(); // Only this directly created unrelated fixture.
            process.waitForFinished(2000);
        }
    }
    bool start()
    {
        process.start(QIODevice::ReadOnly);
        return process.waitForStarted(5000)
            && waitUntil([&] { return QFileInfo::exists(directory.filePath("unrelated.heartbeat")); });
    }
    bool keptRunning()
    {
        QByteArray before;
        // Atomic heartbeat replacement can make a single Windows observer read
        // temporarily unavailable. Require two valid, different samples within
        // the deadline; an empty read is neither process death nor advancement.
        const bool kept = waitUntil([&] {
            if (process.state() != QProcess::Running) return false;
            const auto current = readMarker(directory.filePath("unrelated.heartbeat"));
            if (current.isEmpty()) return false;
            if (before.isEmpty()) { before = current; return false; }
            return current != before;
        }, 1000);
        if (!kept)
            std::fprintf(stderr, "Unrelated fixture failed: state=%d exit=%d heartbeat=%s stderr=%s\n",
                         int(process.state()), process.exitCode(), before.constData(),
                         process.readAllStandardError().constData());
        return kept;
    }
};

bool checkTree(const QString &path, const QString &mode, UnrelatedFixture &unrelated)
{
    QDir().mkpath(path);
    const QDir directory(path);
    qint64 grandchildPid = 0;
    bool parentFinished = false;
    {
        Mandarina::OwnedProcess process;
        process.setProgram(QCoreApplication::applicationFilePath());
        process.setArguments({"--child", path, mode == "orphan" ? "exit" : "stay"});
        if (!process.startOwned(QIODevice::ReadOnly, 5000)) {
            std::fprintf(stderr, "Owned fixture start failed: %s\n", qPrintable(process.errorString()));
            return false;
        }
        if (!waitUntil([&] { return QFileInfo::exists(directory.filePath("child.ready")); })) {
            std::fprintf(stderr, "Owned fixture never ready: state=%d exit=%d stderr=%s\n",
                         int(process.state()), process.exitCode(), process.readAllStandardError().constData());
            writeMarker(directory.filePath("stop"), "stop");
            return false;
        }
        bool valid = false;
        grandchildPid = readMarker(directory.filePath("grandchild.started")).toLongLong(&valid);
        if (!valid || !stillRunning(grandchildPid) || !unrelated.keptRunning()) {
            std::fprintf(stderr, "Owned fixture readiness failed: valid=%d grandchildRunning=%d\n",
                         int(valid), int(stillRunning(grandchildPid)));
            writeMarker(directory.filePath("stop"), "stop");
            return false;
        }
        if (mode == "orphan") {
            parentFinished = waitUntil([&] { return process.state() == QProcess::NotRunning; });
            if (!parentFinished || !stillRunning(grandchildPid)) {
                std::fprintf(stderr, "Owned orphan fixture failed: parentFinished=%d grandchildRunning=%d\n",
                             int(parentFinished), int(stillRunning(grandchildPid)));
                writeMarker(directory.filePath("stop"), "stop");
                return false;
            }
            process.finishOwnedTree();
        } else if (mode == "cancel") {
            process.stopOwned();
            process.finishOwnedTree();
            parentFinished = process.state() == QProcess::NotRunning;
        }
        // The destructor mode intentionally relies on scope exit alone.
    }
    const bool stopped = waitUntil([&] { return !stillRunning(grandchildPid); }, 3000);
    if (!stopped) {
        // Gracefully clean up this fixture by its private stop marker. Never
        // signal an enumerated/global process or a potentially reused PID.
        writeMarker(directory.filePath("stop"), "stop");
        waitUntil([&] { return !stillRunning(grandchildPid); }, 3000);
    }
    const bool kept = unrelated.keptRunning();
    if (!stopped || (mode != "destructor" && !parentFinished) || !kept)
        std::fprintf(stderr, "Owned fixture final state: stopped=%d parentFinished=%d unrelatedKept=%d\n",
                     int(stopped), int(parentFinished), int(kept));
    return stopped && (mode == "destructor" || parentFinished) && kept;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() >= 3) {
        const QDir directory(arguments[2]);
        if (!directory.exists()) return 2;
        if (arguments[1] == "--grandchild") return heartbeat(directory, "grandchild");
        if (arguments[1] == "--unrelated") return heartbeat(directory, "unrelated");
        if (arguments[1] == "--child" && arguments.size() == 4)
            return childFixture(directory, arguments[3] == "exit");
        return 2;
    }
#ifdef Q_OS_LINUX
    // Adopt/reap only descendants of this test process; no system setting changes.
    if (::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) return 2;
#endif
    QTemporaryDir temporary(QDir::tempPath() + "/maracuja-owned-process-XXXXXX");
    if (!temporary.isValid()) return 2;
    UnrelatedFixture unrelated(temporary.filePath("unrelated"));
    if (!unrelated.start()) return 2;
    for (const QString &mode : {QStringLiteral("cancel"), QStringLiteral("orphan"), QStringLiteral("destructor")}) {
        if (!checkTree(temporary.filePath(mode), mode, unrelated)) {
            std::fprintf(stderr, "FAIL: owned process tree (%s) or unrelated fixture preservation\n", qPrintable(mode));
            return 1;
        }
    }
    std::puts("PASS: owned descendant cancellation, orphan cleanup, destructor and unrelated fixture preservation");
    return 0;
}

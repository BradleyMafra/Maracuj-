#include "documentconversion.h"
#include "conversionengine.h"
#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

using namespace Mandarina;
namespace {
bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
void emitEvent(const QJsonObject &object)
{
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}
bool running(qint64 pid)
{
    if (pid <= 1) return false;
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!handle) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return alive;
}
bool stopped(qint64 pid)
{
    QElapsedTimer timer; timer.start();
    while (running(pid) && timer.elapsed() < 2000) QThread::msleep(20);
    return !running(pid);
}
int worker(const QStringList &arguments)
{
    const QDir root(arguments.value(arguments.indexOf("--runtime-root") + 1));
    int count = 0;
    char buffer[64 * 1024 + 2];
    while (true) {
        if (!std::fgets(buffer, int(sizeof(buffer)), stdin)) return 0;
        const QByteArray line(buffer);
        const auto request = QJsonDocument::fromJson(line).object();
        const int id = request.value("id").toInt();
        const auto destination = request.value("output").toString();
        const auto mode = QFileInfo(request.value("input").toString()).completeBaseName();
        ++count;
        QJsonObject result{{"id", id}, {"type", "result"}, {"ok", true}};
        if (mode == "bad-id") result.insert("id", id + 1);
        if (mode == "flood") {
            const QByteArray oversized(70 * 1024, 'x');
            std::fwrite(oversized.constData(), 1, size_t(oversized.size()), stdout);
            std::fflush(stdout);
            return 0;
        }
        if (mode == "slow") {
            QProcess descendant;
            descendant.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
                args->flags |= CREATE_NO_WINDOW;
            });
            descendant.start(QCoreApplication::applicationFilePath(), {"--wait"});
            if (!descendant.waitForStarted()) return 2;
            if (!writeFile(root.filePath("child.pid"), QByteArray::number(descendant.processId()))) return 2;
            emitEvent({{"id", id}, {"type", "progress"}, {"done", 0}, {"total", 1}, {"phase", "text"}});
            QThread::sleep(20);
            descendant.waitForFinished(1000);
        }
        const auto content = QByteArray::number(QCoreApplication::applicationPid()) + ":" + QByteArray::number(count);
        if (!writeFile(destination, content)) result = {{"id", id}, {"type", "result"}, {"ok", false}, {"error", "fixture failure"}};
        if (mode == "crash") return 3;
        if (mode == "error") result = {{"id", id}, {"type", "result"}, {"ok", false}, {"error", "fixture failure"}};
        emitEvent(result);
    }
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.contains("--wait")) { QThread::sleep(20); return 0; }
    if (arguments.contains("--serve")) return worker(arguments);
    if (arguments.contains("--capabilities")) {
        emitEvent({{"ok", true}, {"documents", QJsonArray{"txt", "pdf"}},
                   {"pdf", QJsonArray{"txt"}}, {"image_ocr", QJsonArray{}}});
        return 0;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const QDir root(temporary.path());
    const QString config = root.filePath("runtime-paths.json");
    if (!writeFile(config, QJsonDocument(QJsonObject{{"python", application.applicationFilePath()}}).toJson())
        || !writeFile(root.filePath("document_convert.py"), "synthetic protocol fixture")) return 2;
    // qputenv uses the narrow Windows CRT; UTF-8 corrupts accented test paths.
    if (_wputenv_s(L"MARACUJA_RUNTIME_CONFIG", reinterpret_cast<const wchar_t *>(config.utf16())) != 0) return 2;
    const auto check = [](bool condition, const char *message) {
        if (!condition) std::fprintf(stderr, "%s\n", message);
        return condition;
    };
    std::atomic_bool cancel{false};
    // A cold image conversion must never initialize the document capability cache.
    const QString imageSource = root.filePath("source.png");
    QImage image(24, 24, QImage::Format_RGB32); image.fill(Qt::green);
    if (!image.save(imageSource)) return 2;
    const auto imageResult = convert({{imageSource}, "jpg", temporary.path(), 90, 150}, cancel, {});
    if (!check(imageResult.errors.isEmpty() && imageResult.outputs.size() == 1
               && !Documents::capabilitiesReady(), "Native image conversion probed optional document dependencies")) return 1;
    qint64 firstPid = 0, secondPid = 0;
    {
        Documents::Session session;
        const QString source = root.filePath("good.pdf");
        if (!writeFile(source, "%PDF-synthetic")) return 2;
        for (int index = 1; index <= 64; ++index) {
            const QString output = root.filePath(QString("result-%1.txt").arg(index));
            const auto result = Documents::convert(source, "txt", output, cancel, {}, &session);
            const auto contents = readFile(output).split(':');
            if (result.status != Documents::Outcome::Success)
                std::fprintf(stderr, "Worker fixture request %d: %s\n", index, result.error.toUtf8().constData());
            if (!check(result.status == Documents::Outcome::Success && contents.size() == 2,
                       "Worker conversion failed")) return 1;
            const auto pid = contents.first().toLongLong();
            if (index == 1) firstPid = pid;
            if (index == 33) secondPid = pid;
            if (!check(pid == (index <= 32 ? firstPid : secondPid)
                       && contents.last().toInt() == ((index - 1) % 32) + 1,
                       "Worker reuse/recycling count is wrong")) return 1;
        }
        if (!check(firstPid != secondPid && stopped(firstPid), "Old worker survived recycling")) return 1;
        // Each failure invalidates the worker; a subsequent job can start fresh,
        // while failed requests themselves are never retried/published.
        for (const QString mode : {"bad-id", "crash", "flood", "error"}) {
            const QString source = root.filePath(mode + ".pdf");
            const QString output = root.filePath(mode + ".txt");
            if (!writeFile(source, "%PDF-synthetic")) return 2;
            const auto failed = Documents::convert(source, "txt", output, cancel, {}, &session);
            if (!check(failed.status == Documents::Outcome::Failure && !QFileInfo::exists(output),
                       "Invalid worker result left a partial output")) return 1;
            const auto recovered = Documents::convert(root.filePath("good.pdf"), "txt",
                                                      root.filePath(mode + "-recovered.txt"), cancel, {}, &session);
            if (!check(recovered.status == Documents::Outcome::Success, "Fresh worker recovery failed")) return 1;
        }
        const QString slow = root.filePath("slow.pdf");
        if (!writeFile(slow, "%PDF-synthetic")) return 2;
        const auto cancelled = Documents::convert(slow, "txt", root.filePath("cancelled.txt"), cancel,
            [&](int, int, const QString &) { cancel.store(true); }, &session);
        const qint64 child = readFile(root.filePath("child.pid")).toLongLong();
        if (!check(cancelled.status == Documents::Outcome::Cancelled && child > 1 && stopped(child)
                   && !QFileInfo::exists(root.filePath("cancelled.txt")), "Warm cancellation left output/descendant")) return 1;
        cancel.store(false);
        QFile::remove(root.filePath("child.pid"));
        const auto timeout = session.run({"--input", slow, "--output", root.filePath("timed-out.txt"), "--format", "txt"},
                                         cancel, 300, {});
        const qint64 timedChild = readFile(root.filePath("child.pid")).toLongLong();
        if (!check(timeout.status == Documents::Outcome::Failure && (timedChild == 0 || stopped(timedChild)),
                   "Timed-out worker left a descendant")) return 1;
    }
    if (!check(stopped(secondPid), "Worker survived the batch scope")) return 1;
    qint64 finalPid = 0;
    {
        Documents::Session session;
        const auto result = Documents::convert(root.filePath("good.pdf"), "txt", root.filePath("last.txt"), cancel, {}, &session);
        if (result.status != Documents::Outcome::Success) return 1;
        finalPid = readFile(root.filePath("last.txt")).split(':').first().toLongLong();
    }
    if (!check(stopped(finalPid), "RAII teardown left a successful worker running")) return 1;
    std::puts("Native dependency bypass, reuse, recycling, failures, cancellation, timeout and teardown passed.");
    return 0;
}

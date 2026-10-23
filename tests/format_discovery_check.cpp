#include "converter.h"
#include "documentconversion.h"
#include <QGuiApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
int failure(int line) {
    std::fprintf(stderr, "Format discovery regression at line %d\n", line);
    return 1;
}
bool writeFile(const QString &path, const QByteArray &bytes = "fixture") {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool waitFor(const std::function<bool()> &condition, int timeout = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
    return condition();
}
QStringList ids(const QVariantList &formats) {
    QStringList result;
    for (const auto &format : formats) result << format.toMap().value("id").toString();
    return result;
}
}

int main(int argc, char **argv) {
    // The test executable doubles as a controlled slow capability helper.
    if (argc > 1 && QByteArray(argv[1]) == "-I") {
        QCoreApplication child(argc, argv);
        const auto args = child.arguments();
        const int index = args.indexOf("--runtime-root");
        if (index < 0 || index + 1 >= args.size()) return 2;
        const QDir root(args[index + 1]);
        if (!writeFile(root.filePath("probe-started"))) return 2;
        if (!waitFor([&] { return QFile::exists(root.filePath("probe-release")); })) return 3;
        std::puts("{\"ok\":true,\"documents\":[\"pdf\",\"docx\"],\"pdf\":[\"txt\",\"docx\"],\"image_ocr\":[\"txt\",\"docx\"]}");
        return 0;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const QDir root(temporary.path());
    const auto config = root.filePath("runtime-paths.json");
    if (!writeFile(config, QJsonDocument(QJsonObject{{"python", QCoreApplication::applicationFilePath()}}).toJson())
        || !writeFile(root.filePath("document_convert.py"))
        || !writeFile(root.filePath("note.txt"), "local fixture")) return 2;
    // qputenv uses the CRT code page on Windows; retain accented/Unicode paths.
    if (_wputenv_s(L"MARACUJA_RUNTIME_CONFIG",
            reinterpret_cast<const wchar_t *>(config.utf16())) != 0) return 2;
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    const auto source = root.filePath("image.png");
    if (!image.save(source)) return 2;
    // Discovery must not occupy the conversion pool, including a one-core PC.
    QThreadPool::globalInstance()->setMaxThreadCount(1);
    QElapsedTimer constructorTimer;
    constructorTimer.start();
    Converter converter;
    struct ReleaseProbe {
        QString path;
        ~ReleaseProbe() { writeFile(path); }
    } release{root.filePath("probe-release")};
    if (constructorTimer.elapsed() >= 1000 || !converter.formatsLoading()) return failure(__LINE__);
    if (!waitFor([&] { return QFile::exists(root.filePath("probe-started")); })) return failure(__LINE__);
    const auto sourceUrl = QUrl::fromLocalFile(source);
    converter.addUrls({sourceUrl});
    if (!ids(converter.formats()).contains("jpg") || ids(converter.formats()).contains("txt")) return failure(__LINE__);
    converter.convertTo("jpg");
    if (!waitFor([&] { return !converter.busy(); }, 2000)
        || !converter.errors().isEmpty() || converter.outputs().size() != 1
        || !QFile::exists(converter.outputs().first())
        || Mandarina::Documents::capabilitiesReady()) return failure(__LINE__);
    const auto filesBefore = converter.files();
    const auto outputsBefore = converter.outputs();
    const auto selectedBefore = converter.selectedFormat();
    // A pending document preview must populate without another user selection.
    converter.previewUrls({QUrl::fromLocalFile(root.filePath("note.txt"))});
    if (converter.inputKind() != "document" || !converter.formats().isEmpty()) return failure(__LINE__);
    if (!writeFile(release.path) || !waitFor([&] { return !converter.formatsLoading(); })) return failure(__LINE__);
    if (!ids(converter.formats()).contains("pdf") || !ids(converter.formats()).contains("docx")
        || converter.files() != filesBefore || converter.outputs() != outputsBefore) return failure(__LINE__);
    converter.clearDragPreview();
    converter.setSelectedFormat(selectedBefore);
    if (!ids(converter.formats()).contains("txt") || !ids(converter.formats()).contains("docx")) return failure(__LINE__);
    std::puts("PASS: native conversion while discovery is blocked, isolated pool, pending preview completion, preserved files and outputs");
    return 0;
}

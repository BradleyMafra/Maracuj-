#include "filetoolsengine.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>

using namespace Mandarina::FileTools;
namespace {
int failed = 0, checks = 0;
void check(bool good, const QString &message) {
    ++checks;
    if (!good) { ++failed; QTextStream(stderr) << "FAIL: " << message << '\n'; }
}
QByteArray content(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QByteArray digest(const QString &path) {
    return QCryptographicHash::hash(content(path), QCryptographicHash::Sha256);
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), QStringLiteral("private fixture directory"));
    const QString imagePath = temporary.filePath(QStringLiteral("photo.png"));
    QImage image(80, 60, QImage::Format_ARGB32);
    image.fill(qRgba(45, 85, 125, 150));
    image.setText(QStringLiteral("Author"), QStringLiteral("Before"));
    check(image.save(imagePath), QStringLiteral("generate PNG fixture"));
    const QByteArray originalDigest = digest(imagePath);
    const QString explicitDirectory = temporary.filePath(QStringLiteral("saídas à parte"));
    check(QDir().mkpath(explicitDirectory), QStringLiteral("create a destination distinct from the sources"));
    std::atomic_bool cancel{false};
    auto inspected = inspect(imagePath, cancel);
    check(inspected.errors.isEmpty() && !inspected.metadata.isEmpty(), QStringLiteral("inspect image through private Python helper"));
    bool titleEditable = false, dimensionsReadOnly = false;
    for (const auto &value : inspected.metadata) {
        const auto item = value.toMap();
        if (item.value(QStringLiteral("key")) == QStringLiteral("text:Author"))
            titleEditable = item.value(QStringLiteral("editable")).toBool();
        if (item.value(QStringLiteral("key")) == QStringLiteral("image:width"))
            dimensionsReadOnly = !item.value(QStringLiteral("editable")).toBool() && !item.value(QStringLiteral("removable")).toBool();
    }
    check(titleEditable && dimensionsReadOnly, QStringLiteral("editable descriptive field and protected structure"));
    Request edit{{imagePath}, QStringLiteral("metadata"),
                 {{QStringLiteral("changes"), QVariantMap{{QStringLiteral("text:Author"), QStringLiteral("After")}}}}, explicitDirectory};
    auto edited = run(edit, cancel);
    check(edited.errors.isEmpty() && edited.outputs.size() == 1, QStringLiteral("metadata edit creates result"));
    if (!edited.outputs.isEmpty()) {
        QImage result(edited.outputs.first());
        check(result.text(QStringLiteral("Author")) == QStringLiteral("After"), QStringLiteral("edited metadata persisted"));
        check(result == image, QStringLiteral("pixel data retained"));
        check(QFileInfo(edited.outputs.first()).absolutePath() == explicitDirectory,
              QStringLiteral("metadata respects an explicit destination distinct from the source"));
    }
    check(digest(imagePath) == originalDigest, QStringLiteral("metadata edit preserves original bytes"));
    auto second = run(edit, cancel);
    check(second.errors.isEmpty() && second.outputs.size() == 1 && edited.outputs != second.outputs,
          QStringLiteral("existing output never replaced"));
    Request bad = edit;
    bad.options = {{QStringLiteral("changes"), QVariantMap{{QStringLiteral("image:width"), QStringLiteral("1")}}}};
    auto rejected = run(bad, cancel);
    check(rejected.outputs.isEmpty() && !rejected.errors.isEmpty(), QStringLiteral("structural metadata edit rejected"));
    Request beside = edit;
    beside.outputDirectory.clear();
    auto defaultEdit = run(beside, cancel);
    check(defaultEdit.errors.isEmpty() && defaultEdit.outputs.size() == 1
              && QFileInfo(defaultEdit.outputs.first()).absolutePath() == QFileInfo(imagePath).absolutePath(),
          QStringLiteral("metadata defaults to a copy beside its original"));
    const auto beforeInvalid = QDir(temporary.path()).entryList(QDir::Files);
    Request invalidDirectory = edit;
    invalidDirectory.outputDirectory = imagePath;
    auto invalidResult = run(invalidDirectory, cancel);
    check(invalidResult.outputs.isEmpty() && !invalidResult.errors.isEmpty()
              && QDir(temporary.path()).entryList(QDir::Files) == beforeInvalid
              && digest(imagePath) == originalDigest,
          QStringLiteral("a file used as explicit destination fails without falling back beside the original"));
    const QString binary = temporary.filePath(QStringLiteral("sample.bin"));
    QFile file(binary);
    check(file.open(QIODevice::WriteOnly) && file.write(QByteArray(16000, 'a')) == 16000, QStringLiteral("generate generic fixture"));
    file.close();
    Request zip{{binary}, QStringLiteral("compress"), {{QStringLiteral("preset"), QStringLiteral("strong")}}, explicitDirectory};
    int progress = 0;
    auto archive = run(zip, cancel, [&](int, int, const QString &) { ++progress; });
    check(archive.errors.isEmpty() && archive.outputs.size() == 1, QStringLiteral("generic file compression creates ZIP"));
    if (!archive.outputs.isEmpty()) {
        check(content(archive.outputs.first()).startsWith("PK\003\004"), QStringLiteral("ZIP result signature"));
        check(archive.details.first().toMap().value(QStringLiteral("savedPercent")).toDouble() > 0,
              QStringLiteral("actual compression reported"));
        check(QFileInfo(archive.outputs.first()).absolutePath() == explicitDirectory,
              QStringLiteral("ZIP compression respects the explicit destination"));
    }
    check(progress >= 2, QStringLiteral("progress completion reported"));
    Request defaultZip = zip;
    defaultZip.outputDirectory.clear();
    auto besideArchive = run(defaultZip, cancel);
    check(besideArchive.errors.isEmpty() && besideArchive.outputs.size() == 1
              && QFileInfo(besideArchive.outputs.first()).absolutePath() == QFileInfo(binary).absolutePath(),
          QStringLiteral("ZIP compression defaults to beside its original"));
    cancel.store(true);
    auto cancelled = run(zip, cancel);
    check(cancelled.cancelled && cancelled.outputs.isEmpty(), QStringLiteral("pre-cancelled work produces no output"));
    check(digest(imagePath) == originalDigest, QStringLiteral("all operations preserve source"));
    QTextStream(stdout) << checks << " file tools engine checks, " << failed << " failures\n";
    return failed ? 1 : 0;
}

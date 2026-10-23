#include "photoeditorcontroller.h"
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

namespace {
bool require(bool condition, const char *message) {
    if (!condition) std::fprintf(stderr, "photo_controller_check: %s\n", message);
    return condition;
}
bool waitFor(const std::function<bool()> &condition) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 15000) QTest::qWait(10);
    return condition();
}
QByteArray digest(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
QImage preview(PhotoEditorController &editor, QQuickImageProvider *provider) {
    const QString id = editor.previewSource().path().mid(1);
    return provider->requestImage(id, nullptr, {});
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    const QString source = directory.filePath(QStringLiteral("foto ação.png"));
    const QString other = directory.filePath("other.png");
    QImage image(1600, 900, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) row[x] = qRgba(40 + x % 150, 50 + y % 140, 80 + (x + y) % 80, 255);
    }
    if (!image.save(source) || !image.save(other)) return 2;
    const auto originalHash = digest(source);
    PhotoEditorController editor;
    std::unique_ptr<QQuickImageProvider> provider(editor.createImageProvider());
    int panels = 0;
    QObject::connect(&editor, &PhotoEditorController::panelRequested, [&] { ++panels; });
    if (!require(!editor.ready() && !editor.busy() && !editor.rendering() && !editor.dirty()
            && editor.previewSource().isEmpty() && editor.exportFormats().contains("png")
            && editor.controls().size() == 18, "invalid initial state")) return 1;
    editor.setExternalBusy(true);
    if (!require(!editor.openFile(source) && panels == 0, "another active operation did not block opening")) return 1;
    editor.setExternalBusy(false);
    if (!require(!editor.openFile(directory.filePath("missing.png")) && panels == 0,
                 "invalid image opened a window")) return 1;
    if (!require(editor.openFile(source) && editor.busy() && panels == 1,
                 "opening did not start an asynchronous load")) return 1;
    if (!require(waitFor([&] { return !editor.busy(); }) && editor.ready() && editor.error().isEmpty(),
                 "image loading failed")) return 1;
    const auto neutral = preview(editor, provider.get());
    if (!require(!neutral.isNull() && neutral.size() == QSize(1280, 720)
            && !editor.dirty() && !editor.rendering(), "preview is missing or not bounded")) return 1;
    if (!require(editor.openFile(source) && panels == 2 && !editor.busy(),
                 "reopening the same image reset it")) return 1;
    editor.setAdjustment("exposure", std::numeric_limits<double>::quiet_NaN());
    editor.setAdjustment("invalid", 2);
    if (!require(!editor.dirty() && !editor.rendering(), "invalid control changed the draft")) return 1;
    editor.setAdjustment("brightness", 12);
    if (!require(editor.dirty() && editor.rendering(), "an adjustment did not schedule live preview")) return 1;
    if (!require(waitFor([&] { return !editor.rendering(); }) && editor.error().isEmpty()
            && preview(editor, provider.get()) != neutral, "live preview did not change")) return 1;
    // Race a heavier preview with several new requests. The published image
    // must match the final controls, never a cancelled earlier worker.
    editor.setAdjustment("denoise", 65);
    QTest::qWait(110);
    editor.setAdjustment("exposure", -2);
    editor.setAdjustment("brightness", -15);
    editor.setAdjustment("denoise", 0);
    editor.setAdjustment("exposure", 0.5);
    if (!require(waitFor([&] { return !editor.rendering(); }) && editor.error().isEmpty(),
                 "latest preview did not finish")) return 1;
    std::atomic_bool cancel{false}; QString error;
    const auto expected = Mandarina::Photo::apply(neutral, editor.adjustments(), cancel, error);
    if (!require(error.isEmpty() && preview(editor, provider.get()) == expected,
                 "an obsolete worker replaced the latest preview")) return 1;
    const auto retainedInput = editor.inputPath();
    const auto retainedDraft = editor.adjustments();
    if (!require(!editor.openFile(other) && editor.inputPath() == retainedInput
            && editor.adjustments() == retainedDraft && editor.dirty(),
                 "opening another image discarded unsaved adjustments")) return 1;
    editor.setOutputDirectory("");
    editor.setExportFormat("png");
    editor.saveCopy();
    if (!require(editor.busy(), "save did not start")) return 1;
    const auto frozenDraft = editor.adjustments();
    editor.setAdjustment("brightness", 90);
    editor.closeEditor();
    if (!require(editor.ready() && editor.adjustments() == frozenDraft,
                 "export did not protect its live draft")) return 1;
    if (!require(waitFor([&] { return !editor.busy(); }) && editor.error().isEmpty()
            && !editor.outputPath().isEmpty() && !editor.dirty(), "export did not complete")) return 1;
    const auto output = editor.outputPath();
    if (!require(QFileInfo(output).absolutePath() == directory.path() && output != source
            && QImage(output).size() == image.size() && digest(source) == originalHash,
                 "export did not preserve original dimensions, destination or source")) return 1;
    editor.resetAll();
    editor.saveCopy();
    if (!require(!editor.busy(), "export started with a pending preview")) return 1;
    if (!require(waitFor([&] { return !editor.rendering(); }) && preview(editor, provider.get()) == neutral,
                 "reset did not restore the neutral preview")) return 1;
    editor.closeEditor();
    if (!require(!editor.ready() && editor.previewSource().isEmpty() && editor.originalSource().isEmpty()
            && preview(editor, provider.get()).isNull(), "closing retained decoded images")) return 1;
    if (!require(editor.openFile(other) && waitFor([&] { return !editor.busy(); }) && editor.ready(),
                 "editor did not reopen with another photo")) return 1;
    editor.setAdjustment("grain", 90);
    QTest::qWait(95);
    editor.closeEditor();
    if (!require(waitFor([&] { return !editor.rendering(); }) && !editor.ready()
            && preview(editor, provider.get()).isNull() && digest(source) == originalHash,
                 "a stale preview repopulated a closed editor")) return 1;
    if (!require(editor.openFile(source) && editor.busy(), "cannot prepare cancelled loading")) return 1;
    editor.cancel();
    if (!require(waitFor([&] { return !editor.busy(); }) && !editor.ready(), "loading did not cancel")) return 1;
    if (!require(editor.openFile(source) && waitFor([&] { return !editor.busy(); }) && editor.ready(),
                 "same-photo retry after cancelled loading did not reload")) return 1;
    editor.closeEditor();
    std::puts("PASS: asynchronous bounded previews, latest adjustment wins, draft preservation, full-resolution copy, reset and image release");
    return 0;
}

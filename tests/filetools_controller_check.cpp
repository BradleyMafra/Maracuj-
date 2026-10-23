// Exercise the real asynchronous controller with generated private fixtures.
// No desktop integration, global input, file picker or user file is accessed.
#include "filetoolscontroller.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read a private fixture or output");
    return file.readAll();
}

QByteArray digest(const QString &path)
{
    return QCryptographicHash::hash(contents(path), QCryptographicHash::Sha256);
}

void finish(FileToolsController &controller)
{
    if (!controller.busy()) return;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &FileToolsController::busyChanged, &loop, [&] {
        if (!controller.busy()) loop.quit();
    });
    deadline.start(8000);
    loop.exec();
    if (controller.busy()) {
        controller.cancel();
        throw std::runtime_error("The private controller operation exceeded its eight-second deadline");
    }
}

QVariantMap field(const FileToolsController &controller, const QString &key)
{
    for (const auto &value : controller.metadata()) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("key")).toString() == key) return row;
    }
    return {};
}

bool pairedBusy(const QVector<bool> &states, qsizetype start)
{
    return states.size() == start + 2 && states.at(start) && !states.at(start + 1);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create the private controller fixture directory");
        const QDir root(temporary.path());
        const QString source = root.filePath(QStringLiteral("foto ação.png"));
        QImage generated(80, 60, QImage::Format_RGB32);
        generated.fill(qRgb(92, 48, 132));
        generated.setText(QStringLiteral("Title"), QStringLiteral("Original title"));
        generated.setText(QStringLiteral("Author"), QStringLiteral("Original author"));
        require(generated.save(source, "PNG"), "Cannot generate the private tagged PNG");
        const auto originalHash = digest(source);
        const QString binary = root.filePath(QStringLiteral("arquivo genérico.bin"));
        QFile binaryFile(binary);
        require(binaryFile.open(QIODevice::WriteOnly)
                    && binaryFile.write(QByteArray(128 * 1024, 'm')) == 128 * 1024,
                "Cannot generate the private arbitrary-file fixture");
        binaryFile.close();
        const auto binaryHash = digest(binary);

        const QVariantList selection{QUrl::fromLocalFile(source), QUrl::fromLocalFile(source),
                                     QUrl(QStringLiteral("https://example.invalid/file.png")),
                                     QUrl::fromLocalFile(root.path()),
                                     QUrl::fromLocalFile(root.filePath(QStringLiteral("missing.bin")))};
        require(FileToolsController::localFiles(selection) == QStringList{QFileInfo(source).canonicalFilePath()}
                    && FileToolsController::localFiles({}).isEmpty(),
                "The local selector accepted remote URLs, directories, missing files or duplicate entries");

        {
            FileToolsController editing;
            const auto formats = editing.editFormats();
            require(formats.size() == 3
                        && formats.at(0).toMap().value(QStringLiteral("id")) == QStringLiteral("tool:compress")
                        && formats.at(1).toMap().value(QStringLiteral("id")) == QStringLiteral("tool:metadata")
                        && formats.at(2).toMap().value(QStringLiteral("id")) == QStringLiteral("tool:photo"),
                    "The editing wheel did not expose compression, metadata and photo editing");
            require(FileToolsController::toolsFormatsForFiles({source}).size() == 3
                        && FileToolsController::toolsFormatsForFiles({binary}).size() == 2
                        && FileToolsController::toolsFormatsForFiles({source, binary}).size() == 2
                        && FileToolsController::toolsFormatsForFiles({source, source}).size() == 2,
                    "The native photo tool was offered for an unsupported or multiple-file selection");
            int editingPanels = 0;
            QStringList photoRequests;
            QObject::connect(&editing, &FileToolsController::panelRequested, &app, [&] { ++editingPanels; });
            QObject::connect(&editing, &FileToolsController::photoRequested, &app,
                             [&](const QString &path) { photoRequests << path; });
            const auto filesBeforeOpening = root.entryList(QDir::Files);
            require(editing.openUrls({QUrl::fromLocalFile(binary)}, QStringLiteral("compress"))
                        && editingPanels == 1 && !editing.busy() && editing.outputs().isEmpty()
                        && editing.files().size() == 1 && editing.action() == QStringLiteral("compress")
                        && root.entryList(QDir::Files) == filesBeforeOpening && digest(binary) == binaryHash,
                    "Opening arbitrary files from the editing wheel started processing before confirmation");
            const auto selectedFiles = editing.files();
            const auto selectedAction = editing.action();
            const auto selectedStatus = editing.status();
            require(editing.openUrls({QUrl::fromLocalFile(source)}, QStringLiteral("photo"))
                        && photoRequests == QStringList{QFileInfo(source).canonicalFilePath()}
                        && editingPanels == 1 && editing.files() == selectedFiles
                        && editing.action() == selectedAction && editing.status() == selectedStatus
                        && !editing.busy() && editing.outputs().isEmpty()
                        && root.entryList(QDir::Files) == filesBeforeOpening && digest(source) == originalHash,
                    "Opening a photo changed file-tool state or wrote an output before confirmation");
            require(!editing.openUrls({}, QStringLiteral("photo"))
                        && !editing.openUrls({QUrl::fromLocalFile(binary)}, QStringLiteral("photo"))
                        && !editing.openUrls({QUrl::fromLocalFile(source), QUrl::fromLocalFile(binary)}, QStringLiteral("photo"))
                        && !editing.openUrls({QUrl::fromLocalFile(source), QUrl::fromLocalFile(source)}, QStringLiteral("photo"))
                        && !editing.openUrls({QUrl::fromLocalFile(source), QUrl("https://example.invalid/file.png")}, QStringLiteral("photo"))
                        && photoRequests.size() == 1 && editingPanels == 1 && editing.files() == selectedFiles,
                    "An invalid, duplicate or mixed photo request partially replaced the active tool");
            editing.openFiles({source}, QStringLiteral("photo"));
            editing.openFiles({source, binary}, QStringLiteral("photo"));
            require(photoRequests == QStringList{QFileInfo(source).canonicalFilePath(), QFileInfo(source).canonicalFilePath()}
                        && editingPanels == 1 && editing.files() == selectedFiles && editing.action() == selectedAction,
                    "The native photo route fell through to compression or accepted multiple files");
            require(!editing.openUrls({QUrl::fromLocalFile(source)}, QStringLiteral("invalid"))
                        && !editing.openUrls({QUrl::fromLocalFile(source), QUrl("https://example.invalid/file.bin")}, QStringLiteral("metadata"))
                        && !editing.openUrls({QUrl::fromLocalFile(root.path())}, QStringLiteral("compress"))
                        && editingPanels == 1 && editing.files() == selectedFiles && !editing.busy(),
                    "An invalid editing request replaced the current tool selection or action");
            require(editing.openUrls({QUrl::fromLocalFile(binary)}, QStringLiteral("metadata"))
                        && editingPanels == 2 && editing.busy(),
                    "The metadata wheel route did not start its read-only inspection");
            require(!editing.openUrls({QUrl::fromLocalFile(source)}, QStringLiteral("compress")) && editingPanels == 2,
                    "A busy editing request replaced the pending inspection");
            finish(editing);
            require(editing.errors().isEmpty() && !editing.metadata().isEmpty()
                        && editing.outputs().isEmpty() && !editing.metadataDirty()
                        && root.entryList(QDir::Files) == filesBeforeOpening && digest(binary) == binaryHash,
                    "Reading arbitrary-file information wrote a result before confirmation");
            for (const auto &item : editing.metadata()) {
                const auto row = item.toMap();
                require(!row.value(QStringLiteral("editable")).toBool() && !row.value(QStringLiteral("removable")).toBool(),
                        "Arbitrary-file structure was exposed as editable metadata");
            }
        }

        FileToolsController controller;
        require(controller.outputDirectory().isEmpty(), "File tools did not default to beside the original");
        QVector<bool> busyStates;
        int panels = 0;
        int separatePhotoWindows = 0;
        QObject::connect(&controller, &FileToolsController::busyChanged, &app,
                         [&] { busyStates.append(controller.busy()); });
        QObject::connect(&controller, &FileToolsController::panelRequested, &app, [&] { ++panels; });
        QObject::connect(&controller, &FileToolsController::photoRequested, &app,
                         [&](const QString &) { ++separatePhotoWindows; });
        controller.openFiles({source}, QStringLiteral("metadata"));
        require(panels == 1 && controller.busy() && controller.currentFile() == 0,
                "Opening metadata did not request one panel and start its asynchronous inspection");
        // Re-entry during an active worker must not replace the selection or
        // queue a second operation behind the first one.
        controller.openFiles({binary}, QStringLiteral("compress"));
        controller.setAction(QStringLiteral("compress"));
        require(!controller.openUrls({QUrl::fromLocalFile(source)}, QStringLiteral("photo"))
                    && separatePhotoWindows == 0,
                "A photo request bypassed an active metadata inspection");
        require(panels == 1 && controller.action() == QStringLiteral("metadata")
                    && controller.files().size() == 1
                    && controller.files().first().toMap().value(QStringLiteral("path")).toString() == QFileInfo(source).canonicalFilePath(),
                "A busy controller replaced the inspected selection or action");
        finish(controller);
        require(controller.errors().isEmpty() && controller.outputs().isEmpty()
                    && pairedBusy(busyStates, 0) && !controller.metadataDirty()
                    && field(controller, QStringLiteral("text:Author")).value(QStringLiteral("editable")).toBool()
                    && field(controller, QStringLiteral("text:Title")).value(QStringLiteral("editable")).toBool(),
                "Asynchronous inspection failed, published an output or lost editable descriptive fields");

        controller.editMetadata(QStringLiteral("text:Author"), QStringLiteral("Edited author"));
        controller.editMetadata(QStringLiteral("text:Title"), QStringLiteral("Edited title"));
        require(controller.metadataDirty()
                    && field(controller, QStringLiteral("text:Author")).value(QStringLiteral("draft")).toString() == QStringLiteral("Edited author")
                    && field(controller, QStringLiteral("text:Title")).value(QStringLiteral("draft")).toString() == QStringLiteral("Edited title")
                    && controller.outputs().isEmpty() && digest(source) == originalHash,
                "Metadata drafts changed the original or were not retained until confirmation");
        const auto draftRows = controller.metadata();
        const auto draftFiles = controller.files();
        require(controller.openUrls({QUrl::fromLocalFile(source)}, QStringLiteral("photo"))
                    && separatePhotoWindows == 1 && panels == 1
                    && controller.metadata() == draftRows && controller.metadataDirty()
                    && controller.files() == draftFiles && controller.action() == QStringLiteral("metadata")
                    && controller.currentFile() == 0 && !controller.busy(),
                "Opening the separate photo editor discarded the pending metadata draft");
        const auto blockedStatus = controller.status();
        const auto blockedBusyEvents = busyStates.size();
        const auto filesBeforeBlocked = root.entryList(QDir::Files);
        controller.setBlocked(true);
        require(controller.blocked() && !controller.busy(),
                "An external-operation gate was reported as this controller's own worker");
        controller.compress(QStringLiteral("strong"), QStringLiteral("zip"), 0, 0, 0);
        controller.saveMetadata();
        controller.inspect(0);
        controller.setAction(QStringLiteral("compress"));
        controller.editMetadata(QStringLiteral("text:Title"), QStringLiteral("Must remain blocked"));
        controller.removeMetadata(QStringLiteral("text:Title"), true);
        controller.removeAllMetadata(true);
        controller.openFiles({binary}, QStringLiteral("compress"));
        controller.openFiles({source}, QStringLiteral("photo"));
        controller.addUrls({QUrl::fromLocalFile(binary)});
        require(!controller.openUrls({QUrl::fromLocalFile(source)}, QStringLiteral("photo"))
                    && !controller.openUrls({QUrl::fromLocalFile(binary)}, QStringLiteral("metadata"))
                    && !controller.busy() && busyStates.size() == blockedBusyEvents
                    && panels == 1 && separatePhotoWindows == 1
                    && controller.files() == draftFiles && controller.metadata() == draftRows
                    && controller.metadataDirty() && controller.currentFile() == 0
                    && controller.action() == QStringLiteral("metadata")
                    && controller.status() == blockedStatus && controller.outputs().isEmpty()
                    && root.entryList(QDir::Files) == filesBeforeBlocked
                    && digest(source) == originalHash,
                "Another controller's active operation did not block file-tool writes, inspections or state replacement");
        controller.setBlocked(false);
        require(!controller.blocked() && !controller.busy() && controller.metadata() == draftRows,
                "Releasing the external-operation gate lost the pending draft");
        const auto editBegin = busyStates.size();
        controller.saveMetadata();
        require(controller.busy(), "Confirming metadata did not start asynchronous work");
        finish(controller);
        require(controller.errors().isEmpty() && controller.outputs().size() == 1
                    && pairedBusy(busyStates, editBegin) && !controller.metadataDirty(),
                "Confirmed metadata edits did not finish with one output and a settled draft");
        const QString editedPath = controller.outputs().first();
        const QImage edited(editedPath);
        require(!edited.isNull() && edited.text(QStringLiteral("Author")) == QStringLiteral("Edited author")
                    && edited.text(QStringLiteral("Title")) == QStringLiteral("Edited title")
                    && editedPath != source && digest(source) == originalHash
                    && QFileInfo(editedPath).absolutePath() == QFileInfo(source).absolutePath(),
                "Confirmed edits were not written to a separate copy with the original preserved");

        const QString absentDirectory = root.filePath(QStringLiteral("pasta ainda não criada"));
        controller.setOutputDirectory(QUrl::fromLocalFile(absentDirectory).toString());
        controller.inspect(0);
        finish(controller);
        require(controller.errors().isEmpty() && controller.outputs().isEmpty()
                    && !QFileInfo::exists(absentDirectory) && controller.outputDirectory() == absentDirectory,
                "Read-only inspection created or required the configured output directory");
        const QString explicitDirectory = root.filePath(QStringLiteral("saídas à parte"));
        require(QDir().mkpath(explicitDirectory), "Cannot create the separate output fixture directory");
        controller.setOutputDirectory(QUrl::fromLocalFile(explicitDirectory).toString());
        require(controller.outputDirectory() == explicitDirectory,
                "The selected local-file URL did not preserve the explicit output directory");
        controller.editMetadata(QStringLiteral("text:Author"), QStringLiteral("Must not survive Remove All"));
        controller.removeAllMetadata(true);
        require(controller.metadataDirty()
                    && field(controller, QStringLiteral("text:Author")).value(QStringLiteral("removed")).toBool(),
                "Remove All did not mark the pending editable field for removal");
        controller.saveMetadata();
        finish(controller);
        require(controller.errors().isEmpty() && controller.outputs().size() == 1,
                "Remove All failed to save a new metadata copy");
        require(QFileInfo(controller.outputs().first()).absolutePath() == explicitDirectory
                    && controller.outputDirectory() == explicitDirectory,
                "Saving metadata ignored or reset the explicitly selected destination");
        const QImage stripped(controller.outputs().first());
        require(!stripped.isNull() && stripped.text(QStringLiteral("Author")).isEmpty()
                    && stripped.text(QStringLiteral("Title")).isEmpty() && digest(source) == originalHash,
                "A queued edit survived Remove All or metadata removal changed the original");

        controller.openFiles({binary}, QStringLiteral("compress"));
        require(panels == 2 && !controller.busy() && controller.outputs().isEmpty(),
                "Selecting compression started work before explicit confirmation");
        const auto compressionBegin = busyStates.size();
        controller.compress(QStringLiteral("balanced"), QStringLiteral("zip"), 0, 0, 0);
        require(controller.busy(), "Confirming ZIP did not start asynchronous work");
        finish(controller);
        require(controller.errors().isEmpty() && controller.outputs().size() == 1
                    && pairedBusy(busyStates, compressionBegin)
                    && contents(controller.outputs().first()).startsWith("PK\003\004")
                    && QFileInfo(controller.outputs().first()).absolutePath() == explicitDirectory
                    && controller.outputDirectory() == explicitDirectory
                    && digest(binary) == binaryHash && digest(source) == originalHash,
                "Generic ZIP compression failed or changed one of the private originals");

        controller.setOutputDirectory(QStringLiteral(" \t\n"));
        require(controller.outputDirectory().isEmpty(), "Whitespace did not restore the original-side destination");
        controller.compress(QStringLiteral("balanced"), QStringLiteral("zip"), 0, 0, 0);
        finish(controller);
        require(controller.errors().isEmpty() && controller.outputs().size() == 1
                    && QFileInfo(controller.outputs().first()).absolutePath() == QFileInfo(binary).absolutePath()
                    && digest(binary) == binaryHash,
                "Compression did not save beside its source after resetting the output directory");

        const auto beforeCancel = root.entryList(QDir::Files);
        const auto cancelBegin = busyStates.size();
        controller.compress(QStringLiteral("strong"), QStringLiteral("zip"), 0, 0, 0);
        require(controller.busy(), "The cancellation fixture did not start an owned operation");
        controller.cancel();
        finish(controller);
        require(pairedBusy(busyStates, cancelBegin) && controller.outputs().isEmpty()
                    && controller.progress() == 0 && root.entryList(QDir::Files) == beforeCancel
                    && digest(binary) == binaryHash,
                "Immediate cancellation left a busy worker, a published output or changed the original");
        std::puts("{\"test\":\"filetools-controller-async\",\"inspect_async\":true,"
                  "\"metadata_draft_until_confirmed\":true,\"metadata_saved_to_copy\":true,"
                  "\"remove_all_overrides_pending_edit\":true,\"generic_zip_confirmed\":true,"
                  "\"busy_reentry_rejected\":true,\"cancel_settles_without_output\":true,"
                  "\"original_side_default\":true,\"explicit_destination_preserved\":true,"
                  "\"reset_destination_beside_original\":true,\"inspection_does_not_create_destination\":true,"
                  "\"only_local_regular_files\":true,\"originals_sha256_unchanged\":true}");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "File tools controller check failed: %s\n", error.what());
        return 1;
    }
}

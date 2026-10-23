#include "converter.h"
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

namespace {
bool waitForFormats(Converter &converter) {
    if (!converter.formatsLoading()) return true;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&converter, &Converter::formatsChanged, &loop, [&] {
        if (!converter.formatsLoading()) loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(10000);
    if (converter.formatsLoading()) loop.exec();
    return !converter.formatsLoading();
}
QStringList formatIds(const QVariantList &formats) {
    QStringList ids;
    for (const auto &format : formats) ids << format.toMap().value("id").toString();
    return ids;
}

QVariantMap panelState(const Converter &converter) {
    return {{"files", converter.files()}, {"formats", converter.formats()},
            {"inputKind", converter.inputKind()}, {"selectedFormat", converter.selectedFormat()},
            {"dragPreview", converter.dragPreview()}, {"outputDirectory", converter.outputDirectory()},
            {"outputFolderUrl", converter.outputFolderUrl()}, {"quality", converter.property("quality")},
            {"dpi", converter.property("dpi")}};
}

QVariantMap completeState(const Converter &converter) {
    auto state = panelState(converter);
    state.insert("busy", converter.busy());
    state.insert("progress", converter.progress());
    state.insert("status", converter.status());
    state.insert("errors", converter.errors());
    state.insert("outputs", converter.outputs());
    return state;
}

bool gestureIsolationCheck(QTemporaryDir &temporary, const QString &image, const QString &text,
                           const QString &pdf, const QVariantList &many) {
    const auto check = [](bool condition, const char *message) {
        if (!condition) std::fprintf(stderr, "Global gesture regression: %s\n", message);
        return condition;
    };
    const QString externalDirectory = temporary.filePath("gesture-inputs");
    const QString explicitDirectory = temporary.filePath("gesture-outputs");
    if (!QDir().mkpath(externalDirectory) || !QDir().mkpath(explicitDirectory)) return false;
    const QString externalImage = externalDirectory + "/external-image.png";
    const QString secondPdf = externalDirectory + "/second.pdf";
    const QString unsupported = externalDirectory + "/unsupported.bin";
    if (!QFile::copy(image, externalImage) || !QFile::copy(pdf, secondPdf)) return false;
    { QFile file(unsupported); if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7) return false; }
    QFile original(externalImage);
    if (!original.open(QIODevice::ReadOnly)) return false;
    const QByteArray originalBytes = original.readAll();
    original.close();
    const QVariantList imageUrls{QUrl::fromLocalFile(externalImage)};
    const QVariantList documentUrls{QUrl::fromLocalFile(text)};
    const QVariantList singlePdfUrls{QUrl::fromLocalFile(pdf)};
    const QVariantList multiplePdfUrls{QUrl::fromLocalFile(pdf), QUrl::fromLocalFile(secondPdf)};
    Converter imageWheel, documentWheel, pdfWheel;
    if (!waitForFormats(imageWheel) || !waitForFormats(documentWheel) || !waitForFormats(pdfWheel)) return false;
    imageWheel.addUrls(imageUrls);
    documentWheel.addUrls(documentUrls);
    pdfWheel.addUrls(singlePdfUrls);
    const auto expectedImageFormats = imageWheel.formats();
    const auto expectedDocumentFormats = documentWheel.formats();
    const auto expectedPdfFormats = pdfWheel.formats();
    pdfWheel.addUrls({QUrl::fromLocalFile(secondPdf)});
    const auto expectedMultiplePdfFormats = pdfWheel.formats();
    if (!check(!formatIds(expectedPdfFormats).contains("pdf")
               && formatIds(expectedMultiplePdfFormats).contains("pdf"),
               "PDF merge choices must depend on the external drag's distinct file count")) return false;

    const QList<QVariantList> invalidSelections{
        {}, {QUrl::fromLocalFile(unsupported)}, {QUrl::fromLocalFile(temporary.path())},
        {QUrl("https://example.invalid/file.png")},
        {imageUrls.first(), QUrl::fromLocalFile(temporary.filePath("missing.png"))},
        {imageUrls.first(), QUrl::fromLocalFile(unsupported)},
        {imageUrls.first(), QUrl::fromLocalFile(text)}, many};
    // Exercise empty, image and document panels, an independent panel-local
    // preview, and an explicit output folder. Global discovery must not emit
    // panel notifications, even when it is dismissed without a drop.
    for (int scenario = 0; scenario < 5; ++scenario) {
        int panelSignals = 0, allSignals = 0;
        Converter panel;
        if (!waitForFormats(panel)) return false;
        if (scenario == 1 || scenario == 3) panel.addUrls({QUrl::fromLocalFile(image)});
        if (scenario == 2 || scenario == 4) panel.addUrls(documentUrls);
        if (scenario == 3 && !panel.previewUrls(singlePdfUrls)) return false;
        if (scenario == 4) panel.setOutputDirectory(explicitDirectory);
        const auto currentIds = formatIds(panel.formats());
        panel.setSelectedFormat(currentIds.contains("webp") ? "webp"
                                : currentIds.contains("rtf") ? "rtf" : currentIds.last());
        panel.setProperty("quality", 80);
        panel.setProperty("dpi", 300);
        for (auto signal : {&Converter::filesChanged, &Converter::formatsChanged,
                            &Converter::selectedFormatChanged, &Converter::outputDirectoryChanged,
                            &Converter::outputFolderChanged, &Converter::qualityChanged, &Converter::dpiChanged})
            QObject::connect(&panel, signal, &panel, [&] { ++panelSignals; ++allSignals; });
        for (auto signal : {&Converter::busyChanged, &Converter::progressChanged,
                            &Converter::statusChanged, &Converter::resultChanged})
            QObject::connect(&panel, signal, &panel, [&] { ++allSignals; });
        const auto before = completeState(panel);
        if (!check(panel.formatsForUrls(documentUrls) == expectedDocumentFormats
                   && panel.formatsForUrls(imageUrls) == expectedImageFormats
                   && panel.formatsForUrls(singlePdfUrls) == expectedPdfFormats
                   && panel.formatsForUrls(multiplePdfUrls) == expectedMultiplePdfFormats
                   && panel.formatsForUrls({QUrl::fromLocalFile(pdf), QUrl::fromLocalFile(pdf)}) == expectedPdfFormats,
                   "the floating wheel must offer the external file's formats in the established order")) return false;
        for (const auto &invalid : invalidSelections)
            if (!check(panel.formatsForUrls(invalid).isEmpty(), "invalid or mixed drags must be rejected atomically")) return false;
        if (!check(completeState(panel) == before && allSignals == 0,
                   "previewing or dismissing the global wheel changed the main panel")) return false;

        const auto preservedPanel = panelState(panel);
        panel.convertUrls(imageUrls, "jpg");
        if (!check(panel.busy() && panelState(panel) == preservedPanel && panelSignals == 0,
                   "dropping on the floating wheel replaced the panel's files, preview or format")) return false;
        const auto busyState = completeState(panel);
        const int signalsBeforeBusyLookup = allSignals;
        if (!check(panel.formatsForUrls(documentUrls).isEmpty() && completeState(panel) == busyState
                   && allSignals == signalsBeforeBusyLookup,
                   "a second gesture while busy must be rejected without side effects")) return false;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&panel, &Converter::busyChanged, &loop, [&] { if (!panel.busy()) loop.quit(); });
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(5000);
        if (panel.busy()) loop.exec();
        if (!check(!panel.busy() && panel.errors().isEmpty() && panel.outputs().size() == 1,
                   "the external conversion did not complete successfully")) return false;
        const QFileInfo output(panel.outputs().first());
        const QString expectedDirectory = scenario == 4 ? explicitDirectory : externalDirectory;
        if (!check(output.absolutePath() == expectedDirectory && output.suffix() == "jpg"
                   && output.completeBaseName().startsWith("external-image-convertido") && output.size() > 0
                   && panelState(panel) == preservedPanel && panelSignals == 0,
                   "external conversion used the panel's source/format or changed its wheel")) return false;
        const auto completedState = completeState(panel);
        const int completedSignals = allSignals;
        if (!check(panel.formatsForUrls(documentUrls) == expectedDocumentFormats
                   && completeState(panel) == completedState && allSignals == completedSignals,
                   "a later global preview cleared conversion results or changed the panel")) return false;

        const auto directoryBeforeInvalid = QDir(expectedDirectory).entryList(QDir::Files);
        panel.convertUrls({imageUrls.first(), QUrl::fromLocalFile(unsupported)}, "jpg");
        if (!check(!panel.busy() && !panel.errors().isEmpty() && panel.outputs().isEmpty()
                   && panelState(panel) == preservedPanel && panelSignals == 0
                   && QDir(expectedDirectory).entryList(QDir::Files) == directoryBeforeInvalid,
                   "an invalid external drop mutated the panel or created an output")) return false;
        panel.convertUrls(imageUrls, "rtf");
        if (!check(!panel.busy() && !panel.errors().isEmpty() && panel.outputs().isEmpty()
                   && panelState(panel) == preservedPanel && panelSignals == 0
                   && QDir(expectedDirectory).entryList(QDir::Files) == directoryBeforeInvalid,
                   "an unsupported external target started a conversion or changed the panel")) return false;
        if (scenario == 3) {
            panel.clearDragPreview();
            if (!check(!panel.dragPreview() && panel.inputKind() == "image" && panel.files().size() == 1,
                       "clearing the panel's own preview no longer restores its local files")) return false;
        }
    }
    if (!original.open(QIODevice::ReadOnly)) return false;
    return check(original.readAll() == originalBytes, "external gesture altered the original file's bytes");
}
} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const QString source = temporary.filePath(QStringLiteral("foto ação.png"));
    QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::cyan);
    if (!image.save(source)) return 2;
    const QString pdf = temporary.filePath("documento.pdf");
    { QFile f(pdf); if (!f.open(QIODevice::WriteOnly)) return 2; f.write("%PDF-1.4\nfixture para classificar apenas\n"); }
    const auto url = QUrl::fromLocalFile(source);
    Converter converter;
    if (!waitForFormats(converter)) return 1;
    if (!converter.outputDirectory().isEmpty() || !converter.outputFolderUrl().isEmpty()) return 1;
    converter.addUrls({url});
    const QUrl originalFolder = QUrl::fromLocalFile(QFileInfo(source).absolutePath());
    if (converter.files().size() != 1 || converter.formats().isEmpty()
        || converter.outputFolderUrl() != originalFolder || !converter.outputDirectory().isEmpty()) return 1;
    QStringList imageFormats;
    for (const auto &format : converter.formats()) imageFormats << format.toMap().value("id").toString();
    if (!imageFormats.contains("txt") || !imageFormats.contains("docx")) return 1;
    // A rejected mixed drag must not replace the existing wheel's formats.
    if (converter.previewUrls({url, QUrl::fromLocalFile(pdf)}) || converter.dragPreview()
        || converter.inputKind() != "image" || converter.formats().isEmpty()) return 1;
    if (!converter.previewUrls({QUrl::fromLocalFile(pdf)}) || converter.inputKind() != "pdf") return 1;
    converter.clearDragPreview();
    if (converter.inputKind() != "image") return 1;
    const QString text = temporary.filePath("anotacoes.txt");
    { QFile f(text); if (!f.open(QIODevice::WriteOnly)) return 2; f.write("Maracujá: texto de teste.\n"); }
    if (!converter.previewUrls({QUrl::fromLocalFile(text)}) || converter.inputKind() != "document") return 1;
    QStringList documentFormats;
    for (const auto &format : converter.formats()) documentFormats << format.toMap().value("id").toString();
    if (!documentFormats.contains("docx") || !documentFormats.contains("pdf") || documentFormats.contains("pages")) return 1;
    const QString audio = temporary.filePath("faixa.wav");
    { QFile f(audio); if (!f.open(QIODevice::WriteOnly)) return 2; f.write("fixture de classificação; a conversão valida o áudio"); }
    if (!converter.previewUrls({QUrl::fromLocalFile(audio)}) || converter.inputKind() != "audio") return 1;
    if (converter.formats().size() != 8) return 1;
    if (converter.previewUrls({QUrl::fromLocalFile(text), QUrl::fromLocalFile(audio)}) || converter.dragPreview()) return 1;
    if (converter.inputKind() != "image") return 1;
    QVariantList many;
    for (int i = 0; i < 129; ++i) {
        const QString path = temporary.filePath(QString("foto-%1.png").arg(i));
        if (!QFile::copy(source, path)) return 2;
        many << QUrl::fromLocalFile(path);
    }
    if (!gestureIsolationCheck(temporary, source, text, pdf, many)) return 1;
    converter.clearFiles();
    converter.addUrls(many.mid(0, 128));
    if (converter.files().size() != 128 || !converter.errors().isEmpty()) return 1;
    converter.addUrls({many.last()});
    if (converter.files().size() != 128 || converter.errors().isEmpty()) return 1;
    // Bad inputs must be reported before an automatic drop conversion begins.
    converter.dropOnFormat({url, QUrl::fromLocalFile(temporary.filePath("ausente.png"))}, "jpg");
    if (converter.busy() || converter.errors().isEmpty() || converter.files().size() != 1) return 1;
    const auto finish = [&] {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&converter, &Converter::busyChanged, &loop, [&] { if (!converter.busy()) loop.quit(); });
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(5000);
        if (converter.busy()) loop.exec();
        return !converter.busy() && converter.errors().isEmpty() && converter.outputs().size() == 1;
    };
    converter.dropOnFormat({url}, "jpg");
    if (!finish() || QFileInfo(converter.outputs().first()).absolutePath() != QFileInfo(source).absolutePath()
        || converter.outputFolderUrl() != originalFolder || !converter.outputDirectory().isEmpty()) return 1;

    const QString explicitDirectory = temporary.filePath(QStringLiteral("saídas à parte"));
    if (!QDir().mkpath(explicitDirectory)) return 2;
    converter.setOutputDirectory(QUrl::fromLocalFile(explicitDirectory).toString());
    const QUrl explicitFolder = QUrl::fromLocalFile(explicitDirectory);
    if (converter.outputDirectory() != explicitDirectory || converter.outputFolderUrl() != explicitFolder) return 1;
    converter.dropOnFormat({url}, "jpg");
    if (!finish() || QFileInfo(converter.outputs().first()).absolutePath() != explicitDirectory
        || converter.outputDirectory() != explicitDirectory || converter.outputFolderUrl() != explicitFolder) return 1;
    converter.clearFiles();
    if (converter.outputDirectory() != explicitDirectory || converter.outputFolderUrl() != explicitFolder) return 1;
    converter.addUrls({url});

    converter.setOutputDirectory(QStringLiteral(" \t\n"));
    if (!converter.outputDirectory().isEmpty() || converter.outputFolderUrl() != originalFolder) return 1;
    converter.dropOnFormat({url}, "jpg");
    if (!finish() || QFileInfo(converter.outputs().first()).absolutePath() != QFileInfo(source).absolutePath()
        || converter.outputFolderUrl() != originalFolder || !converter.outputDirectory().isEmpty()) return 1;
    converter.clearFiles();
    if (!converter.outputDirectory().isEmpty() || !converter.outputFolderUrl().isEmpty()) return 1;

    // Editing explicitly permits arbitrary local files in the shared selection,
    // without making them eligible for the conversion controller's engine.
    const QString binary = temporary.filePath(QStringLiteral("arquivo genérico.bin"));
    const QByteArray binaryBytes("Conteúdo privado do fixture de edição.\n");
    { QFile f(binary); if (!f.open(QIODevice::WriteOnly) || f.write(binaryBytes) != binaryBytes.size()) return 2; }
    const auto binaryUrl = QUrl::fromLocalFile(binary);
    const auto entriesBeforeEditing = QDir(temporary.path()).entryList(QDir::Files);
    converter.addEditingUrls({binaryUrl, binaryUrl});
    if (converter.files().size() != 1 || converter.busy() || !converter.errors().isEmpty()
        || !converter.outputs().isEmpty() || converter.inputKind() != "unsupported" || !converter.formats().isEmpty()) return 1;
    const auto selectedBeforeValidation = converter.files();
    const auto statusBeforeValidation = converter.status();
    if (!converter.canEditUrls({binaryUrl, url}) || converter.canEditUrls({})
        || converter.canEditUrls({binaryUrl, QUrl("https://example.invalid/file.bin")})
        || converter.canEditUrls({binaryUrl, QUrl::fromLocalFile(temporary.path())})
        || converter.canEditUrls({binaryUrl, QUrl::fromLocalFile(temporary.filePath("missing.bin"))})
        || !converter.canEditUrls(many.mid(0,128)) || converter.canEditUrls(many)
        || converter.files() != selectedBeforeValidation || converter.status() != statusBeforeValidation
        || !converter.errors().isEmpty() || converter.dragPreview()) return 1;
    converter.addEditingUrls({url});
    if (converter.files().size() != 2 || !converter.formats().isEmpty()) return 1;
    converter.start();
    if (converter.busy() || !converter.outputs().isEmpty()
        || QDir(temporary.path()).entryList(QDir::Files) != entriesBeforeEditing) return 1;
    if (converter.previewUrls({binaryUrl}) || converter.dragPreview() || !converter.formats().isEmpty()) return 1;
    converter.dropOnFormat({url, binaryUrl}, "jpg");
    if (converter.busy() || converter.errors().isEmpty() || !converter.outputs().isEmpty()) return 1;
    converter.clearFiles();
    converter.addUrls({binaryUrl});
    if (!converter.files().isEmpty() || converter.errors().isEmpty()) return 1;
    converter.addEditingUrls(many);
    if (converter.files().size() != 128 || converter.errors().isEmpty() || converter.busy()) return 1;
    converter.addEditingUrls({many.last()});
    if (converter.files().size() != 128 || converter.errors().isEmpty()) return 1;
    { QFile f(binary); if (!f.open(QIODevice::ReadOnly) || f.readAll() != binaryBytes) return 1; }
    std::puts("PASS: roda global independente do painel no preview, descarte e conversão; seleção de edição genérica sem conversão implícita, validação local sem efeitos, limites, preview estrito, erros, conversão assíncrona e destino padrão/explicitamente escolhido/restaurado");
    return 0;
}

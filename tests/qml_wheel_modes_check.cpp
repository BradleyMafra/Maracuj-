#include "converter.h"
#include "copyonlydropfilter.h"

#include <QColor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QUrl>
#include <QVariantList>
#include <cstdio>
#include <functional>

// The real converter and QML handle the selection and routing. Tool writes are
// a spy so opening an option can never silently compress/edit a fixture.
class WheelToolsFake final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList formats READ formats CONSTANT)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(QVariantList metadata READ metadata NOTIFY metadataChanged)
    Q_PROPERTY(QString action MEMBER action NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QStringList outputs READ outputs NOTIFY changed)
    Q_PROPERTY(QStringList errors READ errors NOTIFY changed)
    Q_PROPERTY(QVariantList details READ details NOTIFY changed)
    Q_PROPERTY(int currentFile READ currentFile NOTIFY metadataChanged)
    Q_PROPERTY(bool metadataDirty READ metadataDirty NOTIFY draftChanged)
public:
    QString action = QStringLiteral("compress");
    QStringList paths;
    bool working = false, dirty = false;
    int opens = 0, compressions = 0, saves = 0, photoOpens = 0;
    QVariantList formats() const
    {
        return {QVariantMap{{"id", "tool:compress"}, {"label", "COMPRIMIR"}, {"detail", "Reduzir tamanho · ZIP"}},
                QVariantMap{{"id", "tool:metadata"}, {"label", "METADADOS"}, {"detail", "Ver, editar ou remover"}},
                QVariantMap{{"id", "tool:photo"}, {"label", "EDITAR FOTO"}, {"detail", "Luz, cor e detalhes"}}};
    }
    QVariantList files() const
    {
        QVariantList result;
        for (const auto &path : paths) result << QVariantMap{{"path", path}, {"name", QFileInfo(path).fileName()},
            {"size", "Arquivo de teste"}, {"bytes", QFileInfo(path).size()}};
        return result;
    }
    QVariantList metadata() const
    {
        return {QVariantMap{{"key", "title"}, {"label", "Título"}, {"group", "Documento"}, {"value", "Teste"},
            {"draft", "Teste"}, {"editable", true}, {"removable", true}, {"removed", false}}};
    }
    QString status() const { return QStringLiteral("Os originais serão preservados."); }
    bool busy() const { return working; }
    double progress() const { return 0; }
    QStringList outputs() const { return {}; }
    QStringList errors() const { return {}; }
    QVariantList details() const { return {}; }
    int currentFile() const { return paths.isEmpty() ? -1 : 0; }
    bool metadataDirty() const { return dirty; }
    Q_INVOKABLE bool openUrls(const QVariantList &urls, const QString &tool)
    {
        if (working || (tool != QStringLiteral("compress") && tool != QStringLiteral("metadata") && tool != QStringLiteral("photo"))) return false;
        QStringList nextPaths;
        for (const auto &value : urls) {
            const QUrl url = value.canConvert<QUrl>() ? value.toUrl() : QUrl(value.toString());
            const QString path = url.isLocalFile() ? url.toLocalFile() : value.toString();
            if (!QFileInfo(path).isFile()) return false;
            nextPaths << QFileInfo(path).canonicalFilePath();
        }
        if (tool == QStringLiteral("photo")) {
            if (nextPaths.size() != 1 || QImage(nextPaths.first()).isNull()) return false;
            ++photoOpens; emit photoRequested(nextPaths.first()); return true;
        }
        paths = nextPaths;
        action = tool;
        dirty = false;
        ++opens;
        emit filesChanged(); emit metadataChanged(); emit draftChanged(); emit changed(); emit panelRequested();
        return true;
    }
    Q_INVOKABLE void addUrls(const QVariantList &urls) { openUrls(urls, action); }
    Q_INVOKABLE void setAction(const QString &tool) { action = tool; emit changed(); }
    Q_INVOKABLE void inspect(int) { emit metadataChanged(); }
    Q_INVOKABLE void editMetadata(const QString &, const QString &) { emit draftChanged(); }
    Q_INVOKABLE void removeMetadata(const QString &, bool) { emit draftChanged(); }
    Q_INVOKABLE void removeAllMetadata(bool) { emit draftChanged(); }
    Q_INVOKABLE void saveMetadata() { ++saves; }
    Q_INVOKABLE void compress(const QString &, const QString &, int, double, qint64) { ++compressions; }
    Q_INVOKABLE void cancel() {}
    Q_INVOKABLE void openOutputFolder() {}
signals:
    void changed();
    void filesChanged();
    void metadataChanged();
    void busyChanged();
    void draftChanged();
    void panelRequested();
    void photoRequested(const QString &path);
};

namespace {
bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "qml_wheel_modes_check: %s\n", message);
    return condition;
}
bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 2000) QTest::qWait(10);
    return condition();
}
bool invoke(QObject *object, const char *method)
{
    const bool result = object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection);
    QCoreApplication::processEvents();
    return result;
}
QVariantList listValue(const QVariant &value)
{
    return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toList() : value.toList();
}
QStringList entryIds(QQuickWindow *window)
{
    QStringList result;
    for (const auto &entry : listValue(window->property("wheelFormats")))
        result << entry.toMap().value("id").toString();
    return result;
}
QQuickItem *findLabel(QQuickItem *item, const QString &text)
{
    if (!item) return nullptr;
    if (item->isVisible() && item->property("text").toString() == text) return item;
    for (auto *child : item->childItems()) if (auto *found = findLabel(child, text)) return found;
    return nullptr;
}
QPoint itemCenter(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}
QByteArray content(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QStringList selectedPaths(const Converter &converter)
{
    QStringList result;
    for (const auto &entry : converter.files()) result << entry.toMap().value("path").toString();
    return result;
}
bool appMenuNavigationCheck(QQuickWindow *window)
{
    auto *button = window->findChild<QQuickItem *>(QStringLiteral("openAppMenu"));
    auto *menu = window->findChild<QObject *>(QStringLiteral("appMenu"));
    const QList<QQuickItem *> entries{
        window->findChild<QQuickItem *>(QStringLiteral("menuHideWindow")),
        window->findChild<QQuickItem *>(QStringLiteral("menuConfigureShortcuts")),
        window->findChild<QQuickItem *>(QStringLiteral("menuQuitApp"))};
    if (!check(button && menu && entries.at(0) && entries.at(1) && entries.at(2),
               "os controles do menu de opções não foram criados")) return false;
    const auto originalGeometry = window->geometry();
    const auto oneEmphasis = [&](QQuickItem *expected, bool mouseHover, const char *message) {
        bool good = waitFor([&] {
            int count = 0;
            for (auto *entry : entries) if (entry->property("emphasized").toBool()) ++count;
            return count == 1 && expected->property("emphasized").toBool()
                && (!mouseHover || expected->property("hovered").toBool());
        });
        // An outgoing color animation must not leave another row visibly
        // filled after its logical highlight has already moved away.
        if (good) for (auto *entry : entries) {
            auto *background = entry->property("background").value<QQuickItem *>();
            if (background && !entry->property("emphasized").toBool()
                && background->property("color").value<QColor>().alpha() != 0) good = false;
        }
        if (!good) {
            for (auto *entry : entries) {
                const auto center = itemCenter(entry);
                std::fprintf(stderr,
                    "menu fixture %s: emphasized=%d highlighted=%d activeFocus=%d hovered=%d hoverEnabled=%d visible=%d center=(%d,%d) size=(%.1f,%.1f) window=(%d,%d)\n",
                    qPrintable(entry->objectName()), entry->property("emphasized").toBool(),
                    entry->property("highlighted").toBool(), entry->hasActiveFocus(), entry->property("hovered").toBool(),
                    entry->property("hoverEnabled").toBool(), entry->isVisible(), center.x(), center.y(),
                    double(entry->width()), double(entry->height()), window->width(), window->height());
            }
            const auto directory = qEnvironmentVariable("MARACUJA_WHEEL_MODES_PREVIEW_DIR");
            if (!directory.isEmpty() && QDir().mkpath(directory)) {
                window->requestUpdate(); QTest::qWait(80);
                const auto screenshot = window->grabWindow();
                if (!screenshot.isNull()) screenshot.save(QDir(directory).filePath(QStringLiteral("menu-hover-regression.png")));
            }
        }
        return check(good, message);
    };

    // Only this fixture's offscreen window receives input. Never click or
    // press Return on Hide/Quit; moving across them is sufficient to exercise
    // the focus retained by a previously selected MenuItem.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(button));
    bool good = check(waitFor([&] { return menu->property("visible").toBool()
            && menu->property("opened").toBool() && entries.at(0)->width() > 0
            && entries.at(1)->width() > 0 && entries.at(2)->width() > 0
            && itemCenter(entries.at(0)).y() < itemCenter(entries.at(1)).y()
            && itemCenter(entries.at(1)).y() < itemCenter(entries.at(2)).y(); }),
                      "o clique nas reticências não abriu o menu");
    // Popup.visible precedes the Menu/ListView's polish. Wait for its final
    // scene coordinates before moving the synthetic pointer onto an entry.
    window->requestUpdate(); QTest::qWait(150);
    if (good) {
        QTest::keyClick(window, Qt::Key_Down);
        good = oneEmphasis(entries.at(0), false, "Down não selecionou apenas Ocultar janela");
    }
    const char *hoverMessages[]{"hover em Ocultar deixou mais de uma opção realçada",
        "hover em Configurar reteve o realce da opção anteriormente focada",
        "hover em Sair reteve o realce de outra opção"};
    if (good) QTest::mouseMove(window, QPoint(4, window->height() / 2), 10);
    for (qsizetype index = 0; good && index < entries.size(); ++index) {
        QTest::mouseMove(window, itemCenter(entries.at(index)), 10);
        good = oneEmphasis(entries.at(index), true, hoverMessages[index]);
        const auto directory = qEnvironmentVariable("MARACUJA_WHEEL_MODES_PREVIEW_DIR");
        if (good && !directory.isEmpty() && QDir().mkpath(directory)) {
            window->requestUpdate(); QTest::qWait(40);
            const auto image = window->grabWindow();
            good = !image.isNull() && image.save(QDir(directory).filePath("hover-" + entries.at(index)->objectName() + ".png"));
        }
    }
    if (good) {
        QTest::keyClick(window, Qt::Key_Up);
        good = oneEmphasis(entries.at(1), false, "Up não transferiu o único realce de Sair para Configurar");
    }
    if (good) {
        QTest::keyClick(window, Qt::Key_Down);
        good = oneEmphasis(entries.at(2), false, "Down não transferiu o único realce de Configurar para Sair");
    }
    QTest::keyClick(window, Qt::Key_Escape);
    const bool escaped = waitFor([&] { return !menu->property("visible").toBool(); });
    invoke(menu, "close");
    window->contentItem()->forceActiveFocus();
    if (!good || !check(escaped && window->isVisible() && window->geometry() == originalGeometry,
                        "Escape no menu ocultou ou alterou a janela principal")) return false;

    // The Qt version on this host naturally moves focus with hover, so that
    // route alone cannot reproduce every platform's retained-focus state.
    // Exercise the style contract explicitly through three public inputs:
    // one old item keeps keyboard focus, but only the new item is highlighted.
    // This is a constructed state, not a claim of natural Linux reproduction.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(button));
    good = check(waitFor([&] { return menu->property("opened").toBool(); }),
                 "o menu não reabriu para verificar foco retido");
    window->requestUpdate(); QTest::qWait(150);
    if (good) {
        entries.at(0)->forceActiveFocus(Qt::OtherFocusReason);
        entries.at(0)->setProperty("highlighted", false);
        entries.at(1)->setProperty("highlighted", true);
        QCoreApplication::processEvents();
        good = check(entries.at(0)->hasActiveFocus() && !entries.at(0)->property("highlighted").toBool()
                         && entries.at(1)->property("highlighted").toBool(),
                     "o fixture não estabeleceu foco retido e um único destaque público");
        if (good) good = oneEmphasis(entries.at(1), false,
            "a seleção em Configurar realçou também Ocultar por reter foco de teclado");
    }
    // Restore a normal menu selection before closing it; never trigger any
    // entry, including the destructive Quit action, in this fixture.
    for (auto *entry : entries) entry->setProperty("highlighted", false);
    menu->setProperty("currentIndex", -1);
    menu->setProperty("currentIndex", 0);
    QTest::keyClick(window, Qt::Key_Escape);
    const bool closed = waitFor([&] { return !menu->property("visible").toBool(); });
    invoke(menu, "close");
    window->contentItem()->forceActiveFocus();
    return good && check(closed && window->isVisible() && window->geometry() == originalGeometry,
                         "fechar a verificação de foco retido alterou a janela principal");
}
bool presetPreviews(QQuickWindow *window, Converter *converter, const QDir &directory)
{
    auto *quality = window->findChild<QQuickItem *>(QStringLiteral("qualityPreset"));
    auto *resolution = window->findChild<QQuickItem *>(QStringLiteral("resolutionPreset"));
    if (!check(quality && resolution && converter && !converter->files().isEmpty()
                   && converter->outputs().isEmpty() && converter->errors().isEmpty(),
               "o fixture de prévia dos seletores não está disponível")) return false;
    const auto previousFiles = selectedPaths(*converter);
    const auto previousFormat = converter->selectedFormat();
    const auto previousOutput = converter->outputDirectory();
    const int previousQuality = converter->property("quality").toInt();
    const int previousDpi = converter->property("dpi").toInt();
    int conversionStarts = 0;
    const auto connection = QObject::connect(converter, &Converter::busyChanged, window,
        [&] { if (converter->busy()) ++conversionStarts; });
    const auto capture = [&](const QString &name) {
        window->requestUpdate(); QTest::qWait(100);
        const auto image = window->grabWindow();
        return !image.isNull() && image.save(directory.filePath(name));
    };
    const auto openPreset = [&](QQuickItem *control) {
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(control));
        return waitFor([&] { return window->property("presetPopupOpen").toBool(); });
    };
    converter->setProperty("quality", 90);
    bool good = check(openPreset(quality) && capture(QStringLiteral("quality-options-popup.png")),
                      "o seletor de qualidade não abriu sua lista");
    if (good) {
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        good = check(waitFor([&] { return converter->property("quality").toInt() == 80
                                      && !window->property("presetPopupOpen").toBool(); })
                         && conversionStarts == 0 && window->isVisible(),
                     "Down/Enter não selecionou 80% sem iniciar conversão");
    }
    if (good) {
        good = check(openPreset(quality), "o seletor de qualidade não reabriu");
        if (good) {
            QTest::keyClick(window, Qt::Key_Escape);
            good = check(waitFor([&] { return !window->property("presetPopupOpen").toBool(); })
                             && window->isVisible() && converter->property("quality").toInt() == 80
                             && conversionStarts == 0,
                         "Escape no seletor fechou o painel ou alterou a seleção");
        }
    }
    converter->setProperty("quality", 90);

    // Generate a one-page private PDF only for input classification. No
    // conversion/rendering command is called to prepare the DPI selector.
    QTemporaryFile pdf(QDir(QFileInfo(previousFiles.first()).absolutePath())
                           .filePath(QStringLiteral("preset-preview-XXXXXX.pdf")));
    if (good) {
        const QList<QByteArray> objects{
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 120 120] /Contents 4 0 R >>",
            "<< /Length 0 >>\nstream\n\nendstream"};
        QByteArray bytes = "%PDF-1.4\n";
        QList<qint64> offsets;
        for (qsizetype index = 0; index < objects.size(); ++index) {
            offsets << bytes.size();
            bytes += QByteArray::number(qint64(index + 1)) + " 0 obj\n" + objects.at(index) + "\nendobj\n";
        }
        const qint64 xref = bytes.size();
        bytes += "xref\n0 5\n0000000000 65535 f \n";
        for (const auto offset : offsets) bytes += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        bytes += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        good = check(pdf.open() && pdf.write(bytes) == bytes.size() && pdf.flush(),
                     "não foi possível preparar o PDF de classificação privado");
        if (good) {
            pdf.close();
            converter->clearFiles();
            converter->addUrls({QUrl::fromLocalFile(pdf.fileName())});
            converter->setSelectedFormat(QStringLiteral("png"));
            converter->setProperty("dpi", 150);
            good = check(waitFor([&] { return resolution->isVisible() && resolution->width() > 0
                                          && converter->inputKind() == QStringLiteral("pdf"); })
                             && capture(QStringLiteral("resolution-preset.png"))
                             && openPreset(resolution) && capture(QStringLiteral("resolution-options-popup.png")),
                         "o seletor de resolução não exibiu o PDF de classificação");
        }
        if (good) {
            QTest::keyClick(window, Qt::Key_Down);
            QTest::keyClick(window, Qt::Key_Return);
            good = check(waitFor([&] { return converter->property("dpi").toInt() == 300
                                          && !window->property("presetPopupOpen").toBool(); })
                             && conversionStarts == 0 && !converter->busy() && window->isVisible(),
                         "Down/Enter não selecionou 300 DPI sem iniciar conversão");
        }
    }

    if (window->property("presetPopupOpen").toBool()) QTest::keyClick(window, Qt::Key_Escape);
    if (converter->busy()) { converter->cancel(); waitFor([&] { return !converter->busy(); }); }
    const bool stayedIdle = conversionStarts == 0 && !converter->busy();
    QObject::disconnect(connection);
    converter->clearFiles();
    QVariantList restoredUrls;
    for (const auto &path : previousFiles) restoredUrls << QUrl::fromLocalFile(path);
    converter->addUrls(restoredUrls);
    converter->setSelectedFormat(previousFormat);
    converter->setOutputDirectory(previousOutput);
    converter->setProperty("quality", previousQuality);
    converter->setProperty("dpi", previousDpi);
    window->contentItem()->forceActiveFocus();
    const auto fixturePath = pdf.fileName();
    const bool removed = fixturePath.isEmpty() || pdf.remove();
    return good && check(stayedIdle && selectedPaths(*converter) == previousFiles
                             && converter->selectedFormat() == previousFormat
                             && converter->outputDirectory() == previousOutput
                             && converter->property("quality").toInt() == previousQuality
                             && converter->property("dpi").toInt() == previousDpi
                             && converter->outputs().isEmpty() && removed && !QFileInfo::exists(fixturePath),
                         "as prévias dos seletores alteraram o estado da conversão");
}
bool preview(QQuickWindow *window, const QString &name)
{
    const QString directory = qEnvironmentVariable("MARACUJA_WHEEL_MODES_PREVIEW_DIR");
    if (directory.isEmpty()) return true;
    if (!QDir().mkpath(directory)) return false;
    window->requestUpdate(); QTest::qWait(100);
    const auto image = window->grabWindow();
    if (image.isNull() || !image.save(QDir(directory).filePath(name))) return false;
    if (name == "wheel-conversion.png") {
        auto *converter = qobject_cast<Converter *>(qmlContext(window)->contextProperty("converter").value<QObject *>());
        if (!converter || converter->files().isEmpty()) return false;
        const QString previous = converter->outputDirectory();
        const QDir sourceDirectory(QFileInfo(converter->files().first().toMap().value("path").toString()).absolutePath());
        // A display-only path inside the private fixture; no directory is
        // created, and the previous destination is restored before continuing.
        converter->setOutputDirectory(sourceDirectory.filePath(QStringLiteral("Cópias revisadas/Documentos e imagens/Uma pasta com nome longo")));
        window->requestUpdate(); QTest::qWait(160);
        const auto destinationImage = window->grabWindow();
        const bool saved = !destinationImage.isNull() && destinationImage.save(QDir(directory).filePath("custom-output-location.png"));
        converter->setOutputDirectory(previous);
        if (!saved) return false;
        if (!presetPreviews(window, converter, QDir(directory))) return false;
    }
    // Optional visual review only: reuse this private fixture for the menu in
    // both window sizes, without adding synthetic input to the real desktop.
    if (name == "wheel-conversion.png" || name == "wheel-modes-compact.png") {
        auto *menu = window->findChild<QObject *>(QStringLiteral("appMenu"));
        if (!invoke(menu, "open")) return false;
        QTest::keyClick(window, Qt::Key_Down);
        window->requestUpdate(); QTest::qWait(180);
        const auto menuImage = window->grabWindow();
        const bool saved = !menuImage.isNull() && menuImage.save(QDir(directory).filePath("menu-" + name));
        QTest::keyClick(window, Qt::Key_Escape);
        invoke(menu, "close");
        if (!saved) return false;
    }
    return true;
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("QT_QUICK_CONTROLS_HOVER_ENABLED", "1");
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir temporary;
    if (!check(temporary.isValid(), "não foi possível criar o fixture privado")) return 1;
    const QDir directory(temporary.path());
    const QString imagePath = directory.filePath(QStringLiteral("foto de teste á.png"));
    QImage image(24, 18, QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#8b64c4")));
    if (!check(image.save(imagePath, "PNG"), "não foi possível gerar o PNG privado")) return 1;
    const QString binaryPath = directory.filePath(QStringLiteral("arquivo genérico á.bin"));
    QFile binary(binaryPath);
    if (!check(binary.open(QIODevice::WriteOnly) && binary.write("Arquivo de teste privado.\n") > 0,
               "não foi possível gerar o arquivo genérico privado")) return 1;
    binary.close();
    const auto originalImage = content(imagePath), originalBinary = content(binaryPath);
    const auto originalFiles = directory.entryList(QDir::Files);

    Converter converter;
    converter.addUrls({QUrl::fromLocalFile(imagePath)});
    int conversionStarts = 0;
    QObject::connect(&converter, &Converter::busyChanged, &app, [&] { if (converter.busy()) ++conversionStarts; });
    WheelToolsFake tools;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("converter", &converter);
    engine.rootContext()->setContextProperty("fileTools", &tools);
    engine.rootContext()->setContextProperty("compactMode", false);
    engine.rootContext()->setContextProperty("residentMode", true);
    engine.rootContext()->setContextProperty("startInBackground", false);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/mandarina/qml/FileToolsWindow.qml")));
    auto *toolsWindow = qobject_cast<QQuickWindow *>(component.create());
    if (!check(toolsWindow, "a janela independente de opções não carregou")) return 1;
    toolsWindow->QObject::setParent(&engine);
    engine.rootContext()->setContextProperty("fileToolsWindow", toolsWindow);
    engine.load(QUrl(QStringLiteral("qrc:/mandarina/qml/Main.qml")));
    if (!check(!engine.rootObjects().isEmpty(), "a interface principal não carregou")) return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!check(window, "a interface não criou uma janela principal")) return 1;
    CopyDropPreferenceFilter copyOnly;
    window->installEventFilter(&copyOnly);
    window->show(); QTest::qWait(100);
    auto *conversionMode = window->findChild<QObject *>(QStringLiteral("modeConversion"));
    auto *editingMode = window->findChild<QObject *>(QStringLiteral("modeEditing"));
    auto *wheel = window->findChild<QQuickItem *>(QStringLiteral("formatWheel"));
    if (!check(conversionMode && editingMode && wheel, "o seletor ou a roda não foi criado")
        || !check(!window->findChild<QObject *>(QStringLiteral("mainAction")),
                  "o botão de conversão removido ainda aparece no painel")) return 1;
    const auto conversionEntries = entryIds(window);
    const QString initialFormat = converter.selectedFormat();
    if (!check(appMenuNavigationCheck(window) && conversionStarts == 0 && tools.opens == 0
                  && selectedPaths(converter) == QStringList{QFileInfo(imagePath).canonicalFilePath()}
                  && directory.entryList(QDir::Files) == originalFiles,
                  "a navegação pelo menu falhou ou modificou a seleção/arquivos do fixture")) return 1;
    if (!check(!window->property("editingMode").toBool() && conversionEntries.contains("png")
                  && !conversionEntries.contains("tool:compress"), "o modo inicial não é Conversão")
        || !check(preview(window, QStringLiteral("wheel-conversion.png")), "a prévia de conversão falhou")) return 1;

    // A tool worker can be active while the main view still displays
    // Conversion. Mode controls and Return may not start competing work.
    tools.working = true;
    emit tools.busyChanged();
    QCoreApplication::processEvents();
    window->requestActivate(); QTest::qWait(20);
    QTest::keyClick(window, Qt::Key_Return); QTest::qWait(30);
    if (!check(window->property("interactionBusy").toBool() && !conversionMode->property("enabled").toBool()
                  && !editingMode->property("enabled").toBool() && conversionStarts == 0 && tools.opens == 0,
                  "a tela de Conversão iniciou trabalho enquanto uma ferramenta estava ocupada")) return 1;
    tools.working = false;
    emit tools.busyChanged();
    QCoreApplication::processEvents();
    if (!check(!window->property("interactionBusy").toBool() && conversionMode->property("enabled").toBool()
                  && editingMode->property("enabled").toBool(),
                  "os seletores de modo não foram restaurados após a ferramenta terminar")) return 1;

    if (!check(invoke(editingMode, "clicked") && window->property("editingMode").toBool(), "Edição não foi selecionada")
        || !check(entryIds(window) == QStringList{"tool:compress", "tool:metadata", "tool:photo"}, "Edição não exibiu as três ferramentas")
        || !check(!window->findChild<QObject *>(QStringLiteral("mainAction")) && conversionStarts == 0 && tools.opens == 0,
                  "trocar para Edição iniciou uma ação ou manteve o botão Converter")
        || !check(preview(window, QStringLiteral("wheel-editing.png")), "a prévia de edição falhou")) return 1;
    auto sendSectorClick = [&](const QString &label) {
        auto *item = findLabel(wheel, label);
        if (!item) return false;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(item));
        QCoreApplication::processEvents();
        return true;
    };
    auto clickSector = [&](const QString &label) {
        return sendSectorClick(label) && waitFor([&] { return toolsWindow->isVisible(); });
    };
    if (!check(clickSector(QStringLiteral("COMPRIMIR")) && tools.opens == 1 && tools.action == "compress"
                  && tools.paths == QStringList{QFileInfo(imagePath).canonicalFilePath()}
                  && toolsWindow->title() == QStringLiteral("Comprimir · Maracujá")
                  && window->property("editingError").toString().isEmpty(),
                  "Comprimir não abriu somente as opções independentes para a seleção")
        || !check(conversionStarts == 0 && tools.compressions == 0 && tools.saves == 0,
                  "abrir opções de compressão iniciou uma escrita")) return 1;
    toolsWindow->close();
    if (!check(waitFor([&] { return !toolsWindow->isVisible(); }), "a janela de opções não fechou")) return 1;
    window->requestActivate(); QTest::qWait(20);
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(30);
    if (!check(conversionStarts == 0 && tools.opens == 1 && !toolsWindow->isVisible(),
               "Return no modo Edição iniciou uma conversão ou ferramenta")) return 1;
    if (!check(clickSector(QStringLiteral("METADADOS")) && tools.opens == 2 && tools.action == "metadata"
                  && toolsWindow->title() == QStringLiteral("Metadados · Maracujá")
                  && conversionStarts == 0 && tools.compressions == 0 && tools.saves == 0,
                  "Metadados não abriu apenas sua janela de opções")) return 1;
    toolsWindow->close(); waitFor([&] { return !toolsWindow->isVisible(); });
    auto *discard = qmlContext(window)->objectForName(QStringLiteral("discardEditDialog"));
    if (!check(discard, "o diálogo de proteção do rascunho não foi criado")) return 1;
    tools.dirty = true;
    emit tools.draftChanged();
    if (!check(sendSectorClick(QStringLiteral("EDITAR FOTO")) && tools.photoOpens == 1
            && tools.opens == 2 && tools.dirty && !discard->property("visible").toBool()
            && selectedPaths(converter) == QStringList{QFileInfo(imagePath).canonicalFilePath()}
            && converter.selectedFormat() == initialFormat && !toolsWindow->isVisible(),
            "Editar foto alterou a seleção, descartou metadados ou abriu compressão")) return 1;
    if (!check(sendSectorClick(QStringLiteral("COMPRIMIR"))
                  && waitFor([&] { return discard->property("visible").toBool(); })
                  && tools.opens == 2 && tools.action == QStringLiteral("metadata") && tools.dirty
                  && !toolsWindow->isVisible(), "trocar a ação descartou metadados sem confirmação")
        || !check(invoke(discard, "reject") && waitFor([&] { return !discard->property("visible").toBool(); })
                  && tools.opens == 2 && tools.dirty && tools.action == QStringLiteral("metadata"),
                  "Cancelar descartou o rascunho ou abriu outra ferramenta")) return 1;
    if (!check(sendSectorClick(QStringLiteral("COMPRIMIR"))
                  && waitFor([&] { return discard->property("visible").toBool(); })
                  && invoke(discard, "discarded") && invoke(discard, "close")
                  && waitFor([&] { return toolsWindow->isVisible(); })
                  && tools.opens == 3 && tools.action == QStringLiteral("compress") && !tools.dirty
                  && tools.compressions == 0 && tools.saves == 0 && conversionStarts == 0,
                  "confirmar Descartar não abriu apenas as opções da nova ação")) return 1;
    toolsWindow->close(); waitFor([&] { return !toolsWindow->isVisible(); });
    if (!check(invoke(conversionMode, "clicked") && !window->property("editingMode").toBool()
                  && entryIds(window) == conversionEntries && converter.selectedFormat() == initialFormat
                  && selectedPaths(converter) == QStringList{QFileInfo(imagePath).canonicalFilePath()},
                  "voltar a Conversão não preservou a seleção e os formatos")) return 1;

    invoke(editingMode, "clicked");
    auto *metadataLabel = findLabel(wheel, QStringLiteral("METADADOS"));
    if (!check(metadataLabel, "o setor de metadados não foi recriado")) return 1;
    const QPoint dropPosition = itemCenter(metadataLabel);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(binaryPath)});
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction;
    QDragEnterEvent enter(dropPosition, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    QDragMoveEvent move(dropPosition, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &move);
    QDropEvent drop(QPointF(dropPosition), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &drop);
    if (!check(enter.isAccepted() && move.isAccepted() && drop.isAccepted()
                  && move.dropAction() == Qt::CopyAction && drop.dropAction() == Qt::CopyAction,
                  "Edição não aceitou o arquivo genérico como cópia")
        || !check(waitFor([&] { return toolsWindow->isVisible(); }) && tools.opens == 4
                  && tools.action == "metadata" && tools.paths == QStringList{QFileInfo(binaryPath).canonicalFilePath()}
                  && selectedPaths(converter) == tools.paths,
                  "o drop genérico não substituiu a seleção e abriu somente a ferramenta escolhida")
        || !check(conversionStarts == 0 && tools.compressions == 0 && tools.saves == 0,
                  "o drop em Edição iniciou uma conversão ou gravação")) return 1;
    toolsWindow->close(); waitFor([&] { return !toolsWindow->isVisible(); });

    QDragEnterEvent moveOnlyEnter(dropPosition, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &moveOnlyEnter);
    QDropEvent moveOnlyDrop(QPointF(dropPosition), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &moveOnlyDrop);
    if (!check(!moveOnlyEnter.isAccepted() && !moveOnlyDrop.isAccepted() && tools.opens == 4,
               "o modo Edição aceitou uma fonte que exige movimento")) return 1;

    // Dropping in the wheel center only adds files; it must not choose a tool.
    QMimeData centerMime;
    centerMime.setUrls({QUrl::fromLocalFile(imagePath)});
    const QPoint center = wheel->mapToScene(QPointF(wheel->width() / 2, wheel->height() / 2)).toPoint();
    QDragEnterEvent centerEnter(center, actions, &centerMime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &centerEnter);
    QDropEvent centerDrop(QPointF(center), actions, &centerMime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &centerDrop);
    if (!check(centerEnter.isAccepted() && centerDrop.isAccepted() && centerDrop.dropAction() == Qt::CopyAction
                  && selectedPaths(converter).contains(QFileInfo(imagePath).canonicalFilePath())
                  && selectedPaths(converter).contains(QFileInfo(binaryPath).canonicalFilePath())
                  && tools.opens == 4 && !toolsWindow->isVisible(),
                  "soltar no centro não apenas adicionou arquivos à seleção compartilhada")) return 1;

    invoke(conversionMode, "clicked");
    if (!check(entryIds(window).isEmpty() && converter.formats().isEmpty()
                  && conversionStarts == 0 && converter.outputs().isEmpty(),
                  "Conversão ofereceu uma saída para a seleção genérica incompatível")) return 1;
    Converter strict;
    strict.addUrls({QUrl::fromLocalFile(binaryPath)});
    if (!check(strict.files().isEmpty() && !strict.errors().isEmpty(), "a entrada estrita de conversão passou a aceitar qualquer arquivo")
        || !check(content(imagePath) == originalImage && content(binaryPath) == originalBinary
                  && directory.entryList(QDir::Files) == originalFiles,
                  "escolher modos ou abrir ferramentas alterou um original ou publicou uma saída")) return 1;
    window->setProperty("compact", true);
    if (!check(waitFor([&] { return window->width() == 550 && window->height() == 610; }),
               "o modo compacto não manteve suas dimensões")
        || !check(conversionMode->property("visible").toBool() && editingMode->property("visible").toBool(),
                  "o seletor não está disponível no modo compacto")
        || !check(preview(window, QStringLiteral("wheel-modes-compact.png")), "a prévia compacta falhou")) return 1;

    std::puts("qml_wheel_modes_check: Conversão/Edição, ações independentes sem execução automática, seleção genérica, CopyOnly e modo compacto");
    return 0;
}

#include "qml_wheel_modes_check.moc"

#include "converter.h"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QVariantList>
#include <cstdio>
#include <functional>

class FloatingFileToolsFake final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(QVariantList metadata READ metadata NOTIFY metadataChanged)
    Q_PROPERTY(QString action MEMBER action NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QStringList outputs READ outputs NOTIFY changed)
    Q_PROPERTY(QStringList errors READ errors NOTIFY changed)
    Q_PROPERTY(QVariantList details READ details NOTIFY changed)
    Q_PROPERTY(int currentFile READ currentFile NOTIFY metadataChanged)
    Q_PROPERTY(bool metadataDirty READ metadataDirty NOTIFY draftChanged)
public:
    QString action = QStringLiteral("compress");
    bool busy = false, readonlyOnly = false;
    int compressions = 0, saves = 0, cancels = 0;
    QVariantList files() const
    {
        if (readonlyOnly) return {QVariantMap{{"name", "notas de teste.txt"}, {"size", "0,8 KB"},
                                             {"bytes", 812}, {"path", "/synthetic/notas de teste.txt"}}};
        return {QVariantMap{{"name", "foto de teste.png"}, {"size", "3,4 MB"},
                            {"bytes", 3565158}, {"path", "/synthetic/foto de teste.png"}}};
    }
    QVariantList metadata() const
    {
        if (readonlyOnly) return {
            QVariantMap{{"key", "fileSize"}, {"label", "Tamanho"}, {"group", "Arquivo"},
                {"value", "812 bytes"}, {"editable", false}, {"removable", false}, {"removed", false}},
            QVariantMap{{"key", "encoding"}, {"label", "Codificação"}, {"group", "Arquivo"},
                {"value", "UTF-8"}, {"editable", false}, {"removable", false}, {"removed", false}}};
        return {QVariantMap{{"key", "title"}, {"label", "Título"}, {"group", "Documento"},
            {"value", "Paisagem"}, {"draft", "Paisagem"}, {"editable", true},
            {"removable", true}, {"removed", false}},
            QVariantMap{{"key", "width"}, {"label", "Largura"}, {"group", "Arquivo"},
            {"value", "1600 px"}, {"editable", false}, {"removable", false}, {"removed", false}}};
    }
    QString status() const { return QStringLiteral("Os originais serão preservados."); }
    double progress() const { return 0; }
    QStringList outputs() const { return {}; }
    QStringList errors() const { return {}; }
    QVariantList details() const { return {}; }
    int currentFile() const { return 0; }
    bool metadataDirty() const { return false; }
    void requestPanel(const QString &tool)
    {
        action = tool;
        emit changed();
        emit panelRequested();
    }
    Q_INVOKABLE void addUrls(const QVariantList &) {}
    Q_INVOKABLE void setAction(const QString &tool) { action = tool; emit changed(); }
    Q_INVOKABLE void inspect(int) { emit metadataChanged(); }
    Q_INVOKABLE void editMetadata(const QString &, const QString &) { emit draftChanged(); }
    Q_INVOKABLE void removeMetadata(const QString &, bool) { emit draftChanged(); }
    Q_INVOKABLE void removeAllMetadata(bool) { emit draftChanged(); }
    Q_INVOKABLE void saveMetadata() { ++saves; }
    Q_INVOKABLE void compress(const QString &, const QString &, int, double, qint64) { ++compressions; }
    Q_INVOKABLE void cancel() { ++cancels; }
    Q_INVOKABLE void openOutputFolder() {}
signals:
    void changed();
    void filesChanged();
    void metadataChanged();
    void draftChanged();
    void busyChanged();
    void panelRequested();
};

namespace {
bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "qml_filetools_window_check: %s\n", message);
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
bool preview(QQuickWindow *window, const QString &name)
{
    const QString directory = qEnvironmentVariable("MARACUJA_FILETOOLS_WINDOW_PREVIEW_DIR");
    if (directory.isEmpty()) return true;
    if (!QDir().mkpath(directory)) return false;
    window->requestUpdate();
    QTest::qWait(100);
    const QImage image = window->grabWindow();
    return !image.isNull() && image.save(QDir(directory).filePath(name));
}
QObject *findVisual(QQuickItem *item, const QString &name)
{
    if (!item) return nullptr;
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
    return nullptr;
}
QVariantList metadataRows(QObject *panel)
{
    QVariant filtered;
    if (!QMetaObject::invokeMethod(panel, "filteredMetadata", Qt::DirectConnection,
                                  Q_RETURN_ARG(QVariant, filtered))) return {};
    return filtered.canConvert<QJSValue>() ? filtered.value<QJSValue>().toVariant().toList() : filtered.toList();
}
}

int main(int argc, char **argv)
{
    // Every synthetic event and screenshot belongs to this offscreen fixture.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    Converter converter;
    FloatingFileToolsFake controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("converter", &converter);
    engine.rootContext()->setContextProperty("fileTools", &controller);
    engine.rootContext()->setContextProperty("compactMode", true);
    engine.rootContext()->setContextProperty("residentMode", true);
    engine.rootContext()->setContextProperty("startInBackground", true);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/mandarina/qml/FileToolsWindow.qml")));
    auto *toolsWindow = qobject_cast<QQuickWindow *>(component.create());
    if (!check(toolsWindow, "a janela flutuante não carregou")) {
        for (const auto &error : component.errors()) std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        return 1;
    }
    toolsWindow->QObject::setParent(&engine);
    engine.rootContext()->setContextProperty("fileToolsWindow", toolsWindow);
    engine.load(QUrl(QStringLiteral("qrc:/mandarina/qml/Main.qml")));
    if (!check(!engine.rootObjects().isEmpty(), "a interface não carregou")) return 1;
    auto *mainWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!check(mainWindow && !mainWindow->isVisible(), "o início em segundo plano exibiu o painel principal")) return 1;
    if (!check(toolsWindow && !toolsWindow->isVisible(), "a janela flutuante não foi criada oculta")) return 1;
    if (!check(toolsWindow->transientParent() == nullptr, "a janela de opções depende da janela principal oculta")) return 1;
    const QSize compactSize = mainWindow->size();
    auto find = [&](const char *name) -> QObject * {
        const QString target = QString::fromLatin1(name);
        if (auto *object = toolsWindow->findChild<QObject *>(target)) return object;
        return findVisual(toolsWindow->contentItem(), target);
    };
    auto *panel = find("fileToolsPanel");
    auto *advanced = find("fileToolsAdvancedToggle");
    auto *technical = find("fileToolsTechnicalToggle");
    if (!check(panel && advanced && technical, "os controles de opções compactas não foram criados")) return 1;

    controller.requestPanel(QStringLiteral("compress"));
    if (!check(waitFor([&] { return toolsWindow->isVisible(); }), "o pedido de compressão não abriu as opções")
        || !check(!mainWindow->isVisible() && mainWindow->property("compact").toBool()
                  && mainWindow->size() == compactSize, "a compressão exibiu ou alterou o painel principal")
        || !check(toolsWindow->title() == QStringLiteral("Comprimir · Maracujá"), "o título não identificou a compressão")
        || !check(toolsWindow->width() >= 400 && toolsWindow->width() <= 500
                  && toolsWindow->height() >= 340 && toolsWindow->height() <= 500,
                  "as opções não abriram em uma janela compacta")
        || !check(toolsWindow->property("preferredWidth").toInt() == 440
                  && toolsWindow->property("preferredHeight").toInt() >= 340
                  && toolsWindow->property("preferredHeight").toInt() <= 500,
                  "a compressão não usa o tamanho minimalista")
        || !check((toolsWindow->flags() & Qt::WindowType_Mask) == Qt::Tool
                  && toolsWindow->flags().testFlag(Qt::FramelessWindowHint), "as opções não usam janela flutuante sem moldura")
        || !check(controller.compressions == 0 && controller.saves == 0, "abrir opções iniciou uma alteração")
        || !check(preview(toolsWindow, QStringLiteral("floating-compress.png")), "não foi possível criar a prévia de compressão")) return 1;
    if (!check(!panel->property("advancedExpanded").toBool() && advanced->property("visible").toBool(),
               "a compressão não iniciou com as opções avançadas recolhidas")
        || !check(invoke(advanced, "clicked") && panel->property("advancedExpanded").toBool(),
                  "as opções avançadas não puderam ser expandidas")
        || !check(invoke(advanced, "clicked") && !panel->property("advancedExpanded").toBool(),
                  "as opções avançadas não puderam ser recolhidas")) return 1;

    toolsWindow->close();
    if (!check(waitFor([&] { return !toolsWindow->isVisible(); }), "fechar opções não ocultou a janela")
        || !check(!mainWindow->isVisible() && mainWindow->size() == compactSize,
                  "fechar opções exibiu ou redimensionou o painel principal")) return 1;

    controller.requestPanel(QStringLiteral("metadata"));
    if (!check(waitFor([&] { return toolsWindow->isVisible(); }), "o pedido de metadados não reabriu as opções")
        || !check(toolsWindow->title() == QStringLiteral("Metadados · Maracujá"), "o título não identificou os metadados")
        || !check(toolsWindow->width() >= 400 && toolsWindow->width() <= 500
                  && toolsWindow->height() >= 340 && toolsWindow->height() <= 500
                  && toolsWindow->property("preferredHeight").toInt() >= 340
                  && toolsWindow->property("preferredHeight").toInt() <= 500,
                  "os metadados não usam o tamanho minimalista")
        || !check(engine.findChildren<QQuickWindow *>(QStringLiteral("fileToolsWindow")).size() == 1,
                  "cada pedido criou outra janela de ferramentas")
        || !check(!mainWindow->isVisible() && controller.saves == 0 && controller.compressions == 0,
                  "ler metadados exibiu o painel ou gravou um arquivo")
        || !check(preview(toolsWindow, QStringLiteral("floating-metadata.png")), "não foi possível criar a prévia de metadados")) return 1;
    if (!check(panel->property("hasEditableMetadata").toBool() && !panel->property("technicalExpanded").toBool()
                  && technical->property("visible").toBool() && metadataRows(panel).size() == 1,
                  "metadados editáveis não recolheram os campos técnicos por padrão")
        || !check(invoke(technical, "clicked") && panel->property("technicalExpanded").toBool()
                  && metadataRows(panel).size() == 2, "expandir detalhes não apresentou os campos técnicos")
        || !check(invoke(technical, "clicked") && !panel->property("technicalExpanded").toBool()
                  && metadataRows(panel).size() == 1, "recolher detalhes não ocultou os campos técnicos")) return 1;
    if (!check(waitFor([&] { return toolsWindow->height() >= 340 && toolsWindow->height() < 450; }),
               "um único campo editável manteve espaço vazio desnecessário")) return 1;
    const int editableMetadataHeight = toolsWindow->height();

    // A TXT with only technical fields needs a small inspection view, without
    // controls implying that metadata can be edited or saved.
    controller.readonlyOnly = true;
    emit controller.filesChanged();
    emit controller.metadataChanged();
    QCoreApplication::processEvents();
    auto *search = find("fileToolsMetadataSearch");
    auto *removeAll = find("fileToolsRemoveAllMetadata");
    auto *run = find("fileToolsRun");
    if (!check(!panel->property("hasEditableMetadata").toBool() && metadataRows(panel).size() == 2
                  && !technical->property("visible").toBool(), "o arquivo somente leitura ocultou suas informações")
        || !check(search && removeAll && run && !search->property("visible").toBool()
                  && !removeAll->property("visible").toBool() && !run->property("visible").toBool(),
                  "o arquivo somente leitura apresentou busca, remoção ou gravação desnecessária")
        || !check(controller.saves == 0 && controller.compressions == 0, "inspecionar dados técnicos iniciou uma escrita")
        || !check(waitFor([&] { return toolsWindow->height() >= 340 && toolsWindow->height() <= 400
                                  && toolsWindow->height() <= editableMetadataHeight; }),
                  "o TXT com duas informações técnicas não reduziu a altura da janela")
        || !check(preview(toolsWindow, QStringLiteral("floating-metadata-readonly.png")),
                  "não foi possível criar a prévia somente leitura")) return 1;
    controller.readonlyOnly = false;
    emit controller.filesChanged();
    emit controller.metadataChanged();
    toolsWindow->close();
    waitFor([&] { return !toolsWindow->isVisible(); });

    // A user may already have the converter open. Opening tools must preserve
    // its size, compact mode and visibility instead of replacing that state.
    mainWindow->show();
    QTest::qWait(30);
    const QRect visibleGeometry = mainWindow->geometry();
    controller.requestPanel(QStringLiteral("compress"));
    if (!check(waitFor([&] { return toolsWindow->isVisible(); }) && mainWindow->isVisible()
                  && mainWindow->geometry() == visibleGeometry && mainWindow->property("compact").toBool(),
                  "abrir opções alterou o painel que já estava aberto")) return 1;
    toolsWindow->close();
    waitFor([&] { return !toolsWindow->isVisible(); });

    mainWindow->showMinimized();
    QTest::qWait(30);
    controller.requestPanel(QStringLiteral("metadata"));
    if (!check(waitFor([&] { return toolsWindow->isVisible(); }) && mainWindow->windowState() == Qt::WindowMinimized,
                  "abrir opções restaurou o painel minimizado")) return 1;
    controller.busy = true;
    emit controller.busyChanged();
    toolsWindow->close();
    QTest::qWait(30);
    if (!check(toolsWindow->isVisible() && controller.cancels == 0,
               "fechar a janela durante a operação descartou o trabalho em andamento")) return 1;
    auto *cancel = toolsWindow->findChild<QObject *>(QStringLiteral("fileToolsCancel"));
    if (!cancel) cancel = findVisual(toolsWindow->contentItem(), QStringLiteral("fileToolsCancel"));
    if (!check(invoke(cancel, "clicked") && controller.cancels == 1,
               "a janela não encaminhou o cancelamento explícito")) return 1;
    controller.busy = false;
    emit controller.busyChanged();
    toolsWindow->close();
    if (!check(waitFor([&] { return !toolsWindow->isVisible(); }), "a janela não fechou após concluir a operação")
        || !check(mainWindow->windowState() == Qt::WindowMinimized && mainWindow->property("compact").toBool(),
                  "fechar ferramentas modificou a janela principal")) return 1;

    std::puts("qml_filetools_window_check: opções independentes, segundo plano, reuso, painel aberto/minimizado e cancelamento explícito");
    return 0;
}

#include "qml_filetools_window_check.moc"

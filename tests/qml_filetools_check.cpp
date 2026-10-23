#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QVariantList>
#include <cstdio>
#include <functional>

class FileToolsFake final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList files MEMBER files NOTIFY filesChanged)
    Q_PROPERTY(QVariantList metadata READ metadata NOTIFY metadataChanged)
    Q_PROPERTY(QString action MEMBER action NOTIFY changed)
    Q_PROPERTY(QString status MEMBER status NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY busyChanged)
    Q_PROPERTY(bool blocked MEMBER blocked NOTIFY changed)
    Q_PROPERTY(double progress MEMBER progress NOTIFY changed)
    Q_PROPERTY(QStringList outputs MEMBER outputs NOTIFY changed)
    Q_PROPERTY(QStringList errors MEMBER errors NOTIFY changed)
    Q_PROPERTY(QVariantList details MEMBER details NOTIFY changed)
    Q_PROPERTY(int currentFile MEMBER currentFile NOTIFY metadataChanged)
    Q_PROPERTY(bool metadataDirty MEMBER metadataDirty NOTIFY draftChanged)
public:
    QVariantList files;
    QString action = "compress", status = "Os originais serão preservados.";
    bool busy = false, blocked = false, metadataDirty = false;
    double progress = 0;
    QStringList outputs, errors;
    QVariantList details;
    int currentFile = -1;
    int compressions = 0, saves = 0, edits = 0, removals = 0, removalsAll = 0, cancels = 0;
    int inspections = 0, folders = 0;
    QString preset, mode, editedKey, editedValue;
    int maxEdge = -1;
    double targetPercent = -1;
    qint64 targetBytes = -1;
    bool removeAll = false;
    QVariantList metadata() const
    {
        QVariantList rows;
        for (auto value : rowsSource) {
            auto row = value.toMap();
            if (removeAll && row.value("removable").toBool()) row["removed"] = true;
            rows << row;
        }
        return rows;
    }
    QVariantList rowsSource {
        QVariantMap{{"key","title"},{"label","Título"},{"group","Documento"},{"value","Paisagem de teste"},
            {"draft","Paisagem de teste"},{"editable",true},{"removable",true},{"removed",false}},
        QVariantMap{{"key","author"},{"label","Autor"},{"group","Documento"},{"value","Autor fictício"},
            {"draft","Autor fictício"},{"editable",true},{"removable",true},{"removed",false}},
        QVariantMap{{"key","width"},{"label","Largura"},{"group","Arquivo"},{"value","1600 px"},
            {"editable",false},{"removable",false},{"removed",false}}
    };
    void requestPanel(const QString &nextAction)
    {
        action = nextAction;
        emit changed();
        emit panelRequested();
    }
    Q_INVOKABLE void addUrls(const QVariantList &) {}
    Q_INVOKABLE void setAction(const QString &value) { action = value; emit changed(); }
    Q_INVOKABLE void inspect(int index) { currentFile = index; ++inspections; metadataDirty = false; emit metadataChanged(); emit draftChanged(); }
    Q_INVOKABLE void editMetadata(const QString &key, const QString &value)
    {
        ++edits; editedKey=key; editedValue=value; metadataDirty=true; emit draftChanged();
    }
    Q_INVOKABLE void removeMetadata(const QString &, bool) { ++removals; metadataDirty=true; emit draftChanged(); }
    Q_INVOKABLE void removeAllMetadata(bool remove)
    {
        ++removalsAll; removeAll=remove; metadataDirty=remove; emit metadataChanged(); emit draftChanged();
    }
    Q_INVOKABLE void saveMetadata() { ++saves; }
    Q_INVOKABLE void compress(const QString &nextPreset, const QString &nextMode, int edge, double percent, qint64 bytes)
    {
        ++compressions; preset=nextPreset; mode=nextMode; maxEdge=edge; targetPercent=percent; targetBytes=bytes;
    }
    Q_INVOKABLE void cancel() { ++cancels; }
    Q_INVOKABLE void openOutputFolder() { ++folders; }
signals:
    void changed();
    void busyChanged();
    void filesChanged();
    void metadataChanged();
    void draftChanged();
    void panelRequested();
};

namespace {
bool check(bool result, const char *message)
{
    if (!result) std::fprintf(stderr, "qml_filetools_check: %s\n", message);
    return result;
}
bool invoke(QObject *object, const char *method)
{
    const bool result=object && QMetaObject::invokeMethod(object,method,Qt::DirectConnection);
    QCoreApplication::processEvents();
    return result;
}
bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed()<2000) QTest::qWait(10);
    return condition();
}
bool preview(QQuickWindow *window, const QString &name)
{
    const auto dir=qEnvironmentVariable("MARACUJA_FILETOOLS_PREVIEW_DIR");
    if (dir.isEmpty()) return true;
    if (!QDir().mkpath(dir)) return false;
    window->requestUpdate(); QTest::qWait(100);
    const auto image=window->grabWindow();
    return !image.isNull() && image.save(QDir(dir).filePath(name));
}
QVariantList metadataRows(QObject *panel)
{
    QVariant filtered;
    if (!QMetaObject::invokeMethod(panel,"filteredMetadata",Qt::DirectConnection,Q_RETURN_ARG(QVariant,filtered))) return {};
    return filtered.canConvert<QJSValue>() ? filtered.value<QJSValue>().toVariant().toList() : filtered.toList();
}
QObject *findVisual(QQuickItem *item, const QString &name)
{
    if (!item) return nullptr;
    if (item->objectName()==name) return item;
    for (auto *child : item->childItems()) if (auto *found=findVisual(child,name)) return found;
    return nullptr;
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen");
    qputenv("QT_QUICK_BACKEND","software");
    qputenv("QT_QUICK_CONTROLS_STYLE","Basic");
    QGuiApplication app(argc,argv);
    FileToolsFake controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("testController",&controller);
    engine.loadData(R"(
        import QtQuick
        import QtQuick.Controls
        ApplicationWindow {
            width: 1010; height: 760; visible: true; color: "#fffcf2"
            FileToolsPanel { controller: testController }
        }
    )",QUrl("qrc:/mandarina/qml/filetools-test.qml"));
    if (!check(!engine.rootObjects().isEmpty(),"a interface não carregou")) return 1;
    auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto find=[&](const char *name) -> QObject * {
        const auto target=QString::fromLatin1(name);
        if (auto *object=window->findChild<QObject *>(target)) return object;
        // Repeater delegates are visual children of their layout, but their
        // QObject ownership need not be below the window on every Qt version.
        return findVisual(window->contentItem(),target);
    };
    auto *panel=find("fileToolsPanel");
    auto *run=find("fileToolsRun");
    auto *close=find("fileToolsClose");
    auto *cancel=find("fileToolsCancel");
    auto *metadataTab=find("fileToolsMetadataTab");
    auto *target=find("fileToolsTargetEnabled");
    auto *targetUnit=find("fileToolsTargetUnit");
    auto *targetValue=find("fileToolsTargetValue");
    if (!check(panel && run && close && cancel && metadataTab && target && targetUnit && targetValue,
               "os controles de ferramentas não foram criados")) return 1;
    controller.requestPanel("compress");
    if (!check(waitFor([&] { return panel->property("visible").toBool(); }),"o pedido não abriu o painel")
        || !check(controller.compressions==0 && controller.saves==0,"abrir o painel iniciou uma alteração")
        || !check(!run->property("enabled").toBool(),"a compressão foi habilitada sem arquivos")) return 1;
    controller.files={QVariantMap{{"name","imagem-teste.png"},{"size","3,4 MB"},{"bytes",3565158},{"path","/synthetic/imagem-teste.png"}}};
    emit controller.filesChanged();
    if (!check(waitFor([&] { return run->property("enabled").toBool(); }),"arquivos não habilitaram a confirmação")) return 1;
    if (!check(preview(window,"filetools-compress.png"),"a prévia de compressão falhou")) return 1;
    invoke(run,"clicked");
    if (!check(controller.compressions==1 && controller.preset=="balanced" && controller.mode=="auto"
        && controller.maxEdge==0 && controller.targetPercent==0 && controller.targetBytes==0,
        "a confirmação não preservou as opções padrão")) return 1;
    target->setProperty("checked",true); targetValue->setProperty("text","1.5");
    invoke(run,"clicked");
    if (!check(controller.targetBytes==1572864 && controller.targetPercent==0,"a meta em MB não foi convertida em bytes")) return 1;
    targetUnit->setProperty("currentIndex",1); targetValue->setProperty("text","20");
    invoke(run,"clicked");
    if (!check(controller.targetPercent==20 && controller.targetBytes==0,"a meta percentual não foi encaminhada")) return 1;
    targetValue->setProperty("text","100");
    if (!check(!run->property("enabled").toBool(),"uma redução de 100% foi aceita")) return 1;
    target->setProperty("checked",false);

    targetValue->setProperty("text","20"); target->setProperty("checked",true);
    controller.blocked = true; emit controller.changed();
    const auto compressionsBeforeBlock = controller.compressions;
    const auto *blockedMessage = find("fileToolsBlockedStatus");
    if (!check(panel->property("blocked").toBool() && !panel->property("working").toBool()
        && !run->property("enabled").toBool() && close->property("enabled").toBool()
        && !cancel->property("visible").toBool() && blockedMessage && blockedMessage->property("visible").toBool(),
        "a operação de outro controlador simulou trabalho próprio ou impediu fechar")) return 1;
    invoke(panel,"startCompression");
    invoke(metadataTab,"clicked");
    if (!check(controller.compressions == compressionsBeforeBlock && controller.action == "compress",
               "um comando do painel iniciou trabalho ou trocou de ferramenta durante outra operação")) return 1;
    controller.blocked = false; emit controller.changed();
    if (!check(run->property("enabled").toBool() && !panel->property("blocked").toBool()
        && target->property("checked").toBool(),
               "a confirmação não foi liberada ao terminar a outra operação")) return 1;
    target->setProperty("checked",false);

    invoke(metadataTab,"clicked");
    emit controller.metadataChanged();
    if (!check(panel->property("metadataMode").toBool(),"a aba de metadados não foi selecionada")
        || !check(!run->property("enabled").toBool(),"salvar foi habilitado antes de editar")) return 1;
    QObject *title=nullptr, *width=nullptr;
    waitFor([&] {
        title=find("fileToolsMetadataEdit_title"); width=find("fileToolsMetadataEdit_width");
        return title && width && title->property("visible").toBool();
    });
    if (!check(title && width,"os campos de metadados não foram criados")
        || !check(title->property("visible").toBool(),"o campo editável não foi apresentado")
        || !check(!width->property("visible").toBool(),"campos técnicos não permaneceram somente leitura")) return 1;
    title->setProperty("text","Título editado"); invoke(title,"textEdited");
    if (!check(controller.edits==1 && controller.editedKey=="title" && controller.editedValue=="Título editado",
               "editar o campo não atualizou o rascunho")
        || !check(run->property("enabled").toBool(),"o rascunho não habilitou salvar a cópia")) return 1;
    if (!check(preview(window,"filetools-metadata.png"),"a prévia de metadados falhou")) return 1;
    controller.blocked = true; emit controller.changed();
    const auto savesBeforeBlock = controller.saves;
    invoke(run,"clicked");
    if (!check(!title->property("enabled").toBool() && !run->property("enabled").toBool()
        && close->property("enabled").toBool() && controller.saves == savesBeforeBlock
        && controller.metadataDirty, "outra operação não protegeu a edição e a confirmação dos metadados")) return 1;
    controller.blocked = false; emit controller.changed();
    invoke(run,"clicked");
    if (!check(controller.saves==1,"salvar cópia não foi encaminhado")) return 1;
    auto *search=find("fileToolsMetadataSearch");
    if (!check(metadataRows(panel).size()==3,"os campos de metadados não foram apresentados")) return 1;
    search->setProperty("text","não-existe-no-fixture"); QCoreApplication::processEvents();
    if (!check(metadataRows(panel).isEmpty(),"a busca não filtrou campos não correspondentes")) return 1;
    search->setProperty("text",QString());
    auto *removeAll=find("fileToolsRemoveAllMetadata");
    removeAll->setProperty("checked",true); invoke(removeAll,"toggled");
    if (!check(controller.removeAll && controller.removalsAll>0,"remover todos não foi encaminhado")) return 1;

    controller.busy=true; emit controller.busyChanged();
    if (!check(panel->property("working").toBool() && !run->property("enabled").toBool()
        && !close->property("enabled").toBool() && cancel->property("visible").toBool(),
        "a operação em andamento não bloqueou confirmação e fechamento")) return 1;
    invoke(cancel,"clicked");
    if (!check(controller.cancels==1,"cancelar a operação não foi encaminhado")) return 1;
    controller.busy=false; emit controller.busyChanged();
    window->resize(550,610);
    if (!check(waitFor([&] { return panel->property("height").toReal()<=578; }),"o painel não coube na roda compacta")
        || !check(preview(window,"filetools-metadata-compact.png"),"a prévia compacta falhou")) return 1;
    panel->setProperty("controller",QVariant::fromValue<QObject *>(nullptr));
    if (!check(!run->property("enabled").toBool(),"o componente não trata controlador ausente")) return 1;
    std::puts("qml_filetools_check: confirmação, metas, edição, busca, remover, cancelar, compacto e controlador ausente");
    return 0;
}

#include "qml_filetools_check.moc"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUrl>
#include <QVariantList>
#include <cmath>
#include <cstdio>
#include <functional>

class PhotoEditorFake final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString fileName MEMBER fileName NOTIFY changed)
    Q_PROPERTY(QString inputPath MEMBER inputPath NOTIFY changed)
    Q_PROPERTY(QString status MEMBER status NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
    Q_PROPERTY(QString outputPath MEMBER outputPath NOTIFY changed)
    Q_PROPERTY(QUrl previewSource READ previewSource NOTIFY previewChanged)
    Q_PROPERTY(QUrl originalSource READ originalSource NOTIFY previewChanged)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY busyChanged)
    Q_PROPERTY(bool cancellable MEMBER cancellable NOTIFY busyChanged)
    Q_PROPERTY(bool rendering MEMBER rendering NOTIFY changed)
    Q_PROPERTY(bool ready MEMBER ready NOTIFY changed)
    Q_PROPERTY(bool dirty MEMBER dirty NOTIFY changed)
    Q_PROPERTY(QVariantMap adjustments READ adjustments NOTIFY changed)
    Q_PROPERTY(QVariantList controls READ controls CONSTANT)
    Q_PROPERTY(QStringList exportFormats READ exportFormats CONSTANT)
    Q_PROPERTY(QString exportFormat MEMBER exportFormat NOTIFY changed)
    Q_PROPERTY(int quality MEMBER quality NOTIFY changed)
public:
    QString fileName = QStringLiteral("Paisagem de teste.png");
    QString inputPath = QStringLiteral("/synthetic/Paisagem de teste.png");
    QString status = QStringLiteral("Ajustes locais · sem enviar sua foto à internet");
    QString error, outputPath;
    QString exportFormat = QStringLiteral("png");
    int quality = 90, changes = 0, saves = 0, closes = 0, resets = 0, openedFolders = 0, cancels = 0;
    bool busy = false, cancellable = false, rendering = false, ready = true, dirty = false;
    QVariantMap values{{"exposure", 0.0}, {"brightness", 0.0}, {"contrast", 0.0},
                       {"highlights", 0.0}, {"shadows", 0.0}, {"whites", 0.0},
                       {"blacks", 0.0}, {"gamma", 0.0}, {"temperature", 0.0},
                       {"tint", 0.0}, {"saturation", 0.0}, {"vibrance", 0.0},
                       {"hue", 0.0}, {"clarity", 0.0}, {"dehaze", 0.0},
                       {"sharpen", 0.0}, {"grain", 0.0}, {"denoise", 0.0}};
    QUrl previewSource() const { return QUrl(QStringLiteral("image://photofixture/preview%1").arg(changes)); }
    QUrl originalSource() const { return QUrl(QStringLiteral("image://photofixture/original")); }
    QVariantMap adjustments() const { return values; }
    QVariantList controls() const
    {
        auto control = [](const char *id, const char *label, const char *group, double min, double max, double step, const char *unit) {
            return QVariantMap{{"id", id}, {"label", QString::fromUtf8(label)}, {"group", group},
                               {"min", min}, {"max", max}, {"step", step}, {"unit", unit}};
        };
        return {control("exposure", "Exposição", "light", -3, 3, 0.05, "EV"),
                control("brightness", "Brilho", "light", -100, 100, 1, ""),
                control("contrast", "Contraste", "light", -100, 100, 1, ""),
                control("highlights", "Realces", "light", -100, 100, 1, ""),
                control("shadows", "Sombras", "light", -100, 100, 1, ""),
                control("whites", "Brancos", "light", -100, 100, 1, ""),
                control("blacks", "Pretos", "light", -100, 100, 1, ""),
                control("gamma", "Gama", "light", -100, 100, 1, ""),
                control("temperature", "Temperatura", "color", -100, 100, 1, ""),
                control("tint", "Matiz verde / magenta", "color", -100, 100, 1, ""),
                control("saturation", "Saturação", "color", -100, 100, 1, ""),
                control("vibrance", "Vivacidade", "color", -100, 100, 1, ""),
                control("hue", "Tonalidade", "color", -180, 180, 1, "°"),
                control("clarity", "Clareza", "detail", -100, 100, 1, ""),
                control("dehaze", "Remover névoa", "detail", -100, 100, 1, ""),
                control("sharpen", "Nitidez", "detail", 0, 100, 1, ""),
                control("grain", "Granulação", "detail", 0, 100, 1, ""),
                control("denoise", "Redução de ruído", "detail", 0, 100, 1, "")};
    }
    QStringList exportFormats() const { return {"png", "jpg", "webp"}; }
    void requestPanel() { emit changed(); emit panelRequested(); }
    Q_INVOKABLE void setAdjustment(const QString &id, double value)
    {
        values.insert(id, value);
        dirty = true;
        ++changes;
        emit changed();
        emit previewChanged();
    }
    Q_INVOKABLE void resetAdjustment(const QString &id)
    {
        values.insert(id, 0.0);
        dirty = false;
        for (auto it = values.cbegin(); it != values.cend(); ++it) dirty |= it.value().toDouble() != 0;
        ++changes; ++resets;
        emit changed(); emit previewChanged();
    }
    Q_INVOKABLE void resetAll()
    {
        for (auto it = values.begin(); it != values.end(); ++it) it.value() = 0.0;
        dirty = false; ++changes; ++resets;
        emit changed(); emit previewChanged();
    }
    Q_INVOKABLE void saveCopy() { ++saves; outputPath = "/synthetic/Paisagem de teste-editada.png"; dirty = false; emit changed(); }
    Q_INVOKABLE void cancel() { ++cancels; }
    Q_INVOKABLE void openOutputFolder() { ++openedFolders; }
    Q_INVOKABLE void closeEditor() { ++closes; dirty = false; ready = false; emit changed(); }
signals:
    void changed();
    void busyChanged();
    void previewChanged();
    void panelRequested();
};

class PhotoFixtureProvider final : public QQuickImageProvider {
public:
    PhotoFixtureProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &) override
    {
        QImage image(480, 300, QImage::Format_RGB32);
        const bool original = id == QStringLiteral("original");
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const bool sun = (x - 340) * (x - 340) + (y - 80) * (y - 80) < 38 * 38;
                const bool hill = y > 175 + 35 * std::sin(x / 93.0);
                image.setPixelColor(x, y, sun ? QColor("#f9d86f") : hill ? QColor("#709a7a")
                    : QColor(original ? 120 : 138, 165 + y / 8, 210 + y / 10));
            }
        if (size) *size = image.size();
        return image;
    }
};

namespace {
bool check(bool value, const char *message)
{
    if (!value) std::fprintf(stderr, "qml_photo_editor_check: %s\n", message);
    return value;
}
bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 2000) QTest::qWait(10);
    return condition();
}
QObject *findVisual(QQuickItem *item, const QString &name)
{
    if (!item) return nullptr;
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
    return nullptr;
}
bool click(QObject *object)
{
    if (!object || !object->property("enabled").toBool()) return false;
    const bool result = QMetaObject::invokeMethod(object, "clicked", Qt::DirectConnection);
    QCoreApplication::processEvents();
    return result;
}
bool screenshot(QQuickWindow *window, const QString &name)
{
    const QString directory = qEnvironmentVariable("MARACUJA_PHOTO_EDITOR_PREVIEW_DIR");
    if (directory.isEmpty()) return true;
    if (!QDir().mkpath(directory)) return false;
    window->requestUpdate(); QTest::qWait(120);
    const QImage image = window->grabWindow();
    return !image.isNull() && image.save(QDir(directory).filePath(name));
}
}

int main(int argc, char **argv)
{
    // All input and screenshots belong to this isolated offscreen window.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    PhotoEditorFake controller;
    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("photofixture"), new PhotoFixtureProvider);
    engine.rootContext()->setContextProperty("photoEditor", &controller);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/mandarina/qml/PhotoEditorWindow.qml")));
    auto *window = qobject_cast<QQuickWindow *>(component.create());
    if (!check(window, "a janela de edição de foto não carregou")) {
        for (const auto &error : component.errors()) std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        return 1;
    }
    window->QObject::setParent(&engine);
    auto find = [&](const char *name) -> QObject * {
        const auto target = QString::fromLatin1(name);
        if (auto *found = window->findChild<QObject *>(target)) return found;
        return findVisual(window->contentItem(), target);
    };
    if (!check(!window->isVisible(), "o editor apareceu antes de uma solicitação")
        || !check(!window->transientParent(), "o editor depende da janela principal")
        || !check(controller.saves == 0 && controller.changes == 0, "criar o editor modificou a foto")) return 1;
    controller.requestPanel();
    if (!check(waitFor([&] { return window->isVisible(); }), "a solicitação não abriu o editor")
        || !check(window->title() == QStringLiteral("Editar foto · Maracujá"), "o título não identificou o editor")
        || !check((window->flags() & Qt::WindowType_Mask) == Qt::Tool
                  && window->flags().testFlag(Qt::FramelessWindowHint), "o editor não usa uma janela flutuante independente")
        || !check(window->width() <= 620 && window->height() <= 730
                  && window->width() >= 470 && window->height() >= 540,
                  "o editor não abriu no tamanho compacto ajustado à tela")
        || !check(QGuiApplication::topLevelWindows().size() == 1, "o editor criou outra janela principal")) return 1;
    QTest::qWait(100);
    auto *exposure = qobject_cast<QQuickItem *>(find("photoAdjustment_exposure"));
    auto *preview = find("photoPreview");
    auto *save = find("photoSaveCopy");
    if (!check(exposure && preview && save && find("photoGroup_color") && find("photoGroup_detail"), "faltam os controles do editor")
        || !check(preview->property("sourceSize").toSize() == QSize(1280, 1280), "a prévia não limita o tamanho de carregamento")
        || !check(screenshot(window, QStringLiteral("photo-editor-light.png")), "a prévia de luz não pôde ser salva")) return 1;
    auto *scroll = qobject_cast<QQuickItem *>(find("photoControlsScroll"));
    auto *flickable = scroll ? qvariant_cast<QObject *>(scroll->property("contentItem")) : nullptr;
    auto *gamma = qobject_cast<QQuickItem *>(find("photoAdjustment_gamma"));
    if (!check(scroll && flickable && gamma
               && flickable->property("contentHeight").toReal() > flickable->property("height").toReal(),
               "os oito ajustes de luz não estão em uma área rolável")) return 1;
    flickable->setProperty("contentY", flickable->property("contentHeight").toReal() - flickable->property("height").toReal());
    QCoreApplication::processEvents();
    if (!check(scroll->mapRectToScene(scroll->boundingRect()).contains(gamma->mapToScene(QPointF(gamma->width() / 2, gamma->height() / 2))),
               "o último ajuste de luz não pôde ser alcançado pela rolagem")) return 1;
    flickable->setProperty("contentY", 0);

    exposure->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Right);
    if (!check(waitFor([&] { return controller.values.value("exposure").toDouble() > 0; }), "o ajuste pelo teclado não atualizou a prévia")
        || !check(controller.saves == 0, "ajustar a prévia salvou um arquivo")) return 1;
    QTest::keyClick(window, Qt::Key_Tab);
    if (!check(window->activeFocusItem() && window->activeFocusItem() != exposure, "Tab não avançou entre os controles")
        || !check(click(find("photoReset_exposure")) && controller.values.value("exposure").toDouble() == 0, "a redefinição individual não restaurou o ajuste")) return 1;
    if (!check(click(find("photoGroup_color")) && find("photoAdjustment_temperature"), "o grupo de cor não apresentou seus ajustes")
        || !check(screenshot(window, QStringLiteral("photo-editor-color.png")), "a prévia de cor não pôde ser salva")
        || !check(click(find("photoGroup_detail")) && find("photoAdjustment_dehaze") && find("photoAdjustment_clarity")
                  && find("photoAdjustment_grain") && find("photoAdjustment_denoise"), "faltam os ajustes de detalhes")
        || !check(screenshot(window, QStringLiteral("photo-editor-detail.png")), "a prévia de detalhes não pôde ser salva")) return 1;
    window->resize(500, 580); QTest::qWait(50);
    auto *saveItem = qobject_cast<QQuickItem *>(save);
    if (!check(saveItem && QRectF(0, 0, window->width(), window->height()).contains(saveItem->mapRectToScene(saveItem->boundingRect())),
               "o rodapé saiu da janela compacta")
        || !check(screenshot(window, QStringLiteral("photo-editor-compact.png")), "a prévia compacta não pôde ser salva")) return 1;
    QMetaObject::invokeMethod(window, "fitToScreen", Qt::DirectConnection); QTest::qWait(50);

    auto *compare = qobject_cast<QQuickItem *>(find("photoCompare"));
    if (!check(compare, "o comparador de original está ausente")) return 1;
    const QPoint comparePoint = compare->mapToScene(QPointF(compare->width() / 2, compare->height() / 2)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, comparePoint);
    if (!check(preview->property("source").toUrl() == controller.originalSource(), "segurar o comparador não mostrou o original")) return 1;
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, comparePoint);
    if (!check(preview->property("source").toUrl() == controller.previewSource(), "soltar o comparador não restaurou a prévia")) return 1;

    controller.rendering = true; emit controller.changed(); QCoreApplication::processEvents();
    if (!check(!save->property("enabled").toBool(), "o editor permite salvar uma prévia ainda pendente")) return 1;
    controller.rendering = false; emit controller.changed(); QCoreApplication::processEvents();
    auto *format = qobject_cast<QQuickItem *>(find("photoExportFormat"));
    if (!check(format, "o seletor de formato está ausente")) return 1;
    format->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Return);
    if (!check(controller.exportFormat == QStringLiteral("jpg"), "o formato selecionado não chegou ao controlador")
        || !check(controller.saves == 0, "escolher o formato iniciou uma exportação")
        || !check(click(save) && controller.saves == 1, "salvar cópia não solicitou a exportação")
        || !check(click(find("photoOpenOutput")) && controller.openedFolders == 1, "abrir pasta não foi encaminhado")) return 1;

    controller.setAdjustment("clarity", 25);
    if (!check(click(find("photoResetAll")) && !controller.dirty, "redefinir todos não restaurou o original")) return 1;
    controller.setAdjustment("clarity", 30);
    if (!check(click(find("photoClose")) && window->isVisible()
               && waitFor([&] { auto *dialog = find("photoDiscardDialog"); return dialog && dialog->property("opened").toBool(); }),
               "fechar descartou ajustes sem confirmação")
        || !check(click(find("photoKeepEditing")) && window->isVisible() && controller.dirty, "continuar não preservou o rascunho")
        || !check(click(find("photoClose")) && click(find("photoDiscardChanges")), "a confirmação de descarte não funcionou")
        || !check(waitFor([&] { return !window->isVisible(); }) && controller.closes == 1, "descartar não ocultou e liberou o editor")) return 1;

    controller.ready = true; controller.requestPanel();
    controller.busy = true; controller.cancellable = false; emit controller.busyChanged(); QCoreApplication::processEvents();
    if (!check(!find("photoCancel")->property("visible").toBool(), "um trabalho externo mostra cancelamento que não pertence ao editor")) return 1;
    controller.cancellable = true; emit controller.busyChanged(); QCoreApplication::processEvents();
    window->close();
    if (!check(window->isVisible(), "fechar interrompeu uma exportação em andamento")
        || !check(click(find("photoCancel")) && controller.cancels == 1 && window->isVisible(),
                  "cancelar não encaminhou o pedido ao trabalho do editor")) return 1;
    controller.busy = false; emit controller.busyChanged(); QCoreApplication::processEvents();
    if (!check(click(find("photoClose")) && !window->isVisible(), "fechar sem ajustes não ocultou o editor")
        || !check(controller.saves == 1, "os fluxos de edição exportaram sem confirmação explícita")) return 1;
    std::puts("qml_photo_editor_check: OK");
    return 0;
}

#include "qml_photo_editor_check.moc"

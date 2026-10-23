#include "converter.h"
#include "copyonlydropfilter.h"

#include <QColor>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <cstdio>

namespace {
bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "qml_drop_check: %s\n", message);
    return condition;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QQuickItem *findLabel(QQuickItem *parent, const QString &text)
{
    for (QQuickItem *child : parent->childItems()) {
        if (child->isVisible() && child->property("text").toString() == text) return child;
        if (auto *label = findLabel(child, text)) return label;
    }
    return nullptr;
}
}

int main(int argc, char **argv)
{
    // Only the test's own scene and temporary image receive synthetic events.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MandarinaDropCheck"));

    QTemporaryDir directory;
    if (!check(directory.isValid(), "não foi possível criar a pasta temporária")) return 1;
    const QString input = directory.filePath(QStringLiteral("foto de teste á.png"));
    QImage image(24, 18, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#8b64c4")));
    if (!check(image.save(input, "PNG"), "não foi possível criar o PNG de teste")) return 1;
    const QByteArray original = readFile(input);
    if (!check(!original.isEmpty(), "o PNG de teste está vazio")) return 1;

    Converter converter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("converter"), &converter);
    engine.rootContext()->setContextProperty(QStringLiteral("compactMode"), true);
    engine.load(QUrl(QStringLiteral("qrc:/mandarina/qml/Main.qml")));
    if (!check(!engine.rootObjects().isEmpty(), "a interface QML não carregou")) return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!check(window != nullptr, "a raiz QML não é uma QQuickWindow")) return 1;
    CopyDropPreferenceFilter copyOnly;
    window->installEventFilter(&copyOnly);
    window->resize(550, 610);
    window->show();
    QTest::qWait(150);
    QCoreApplication::processEvents();

    auto *wheel = qobject_cast<QQuickItem *>(qmlContext(window)->objectForName(QStringLiteral("wheel")));
    if (!check(wheel != nullptr, "a roda QML não foi encontrada")) return 1;
    // Font metrics and layouts differ between the offscreen platform backends.
    // Drop on the visible PNG label in the actual scene instead of fixed pixels.
    auto *pngLabel = findLabel(wheel, QStringLiteral("PNG"));
    if (!check(pngLabel && pngLabel->width() > 0 && pngLabel->height() > 0,
               "o rótulo PNG da roda não foi encontrado")) return 1;

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(input)});
    const QPoint target = pngLabel->mapToScene(QPointF(pngLabel->width() / 2, pngLabel->height() / 2)).toPoint();
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction;

    QDragEnterEvent enter(target, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    // Qt Quick 6.11 forwards acceptance, but not the chosen action, from the
    // first translated enter event. Move and drop do propagate CopyAction.
    if (!check(enter.isAccepted(), "a cena não aceitou o arrasto sintético no setor PNG")
        || !check(enter.dropAction() == Qt::CopyAction, "a entrada nativa negociou movimento")
        || !check(converter.dragPreview(), "a entrada não alcançou a roda de formatos")) return 1;

    QDragMoveEvent move(target, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &move);
    if (!check(move.isAccepted(), "a cena rejeitou o movimento do arrasto aceito")
        || !check(move.dropAction() == Qt::CopyAction, "o movimento negociou uma ação diferente de CopyAction")) return 1;

    QDropEvent drop(QPointF(target), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &drop);
    if (!check(drop.isAccepted(), "a cena não aceitou o drop sintético")
        || !check(drop.dropAction() == Qt::CopyAction, "o drop negociou uma ação diferente de CopyAction")) return 1;

    QElapsedTimer elapsed;
    elapsed.start();
    while (converter.busy() && elapsed.elapsed() < 10000) QTest::qWait(20);
    QCoreApplication::processEvents();
    if (!check(!converter.busy(), "a conversão não terminou dentro do prazo de teste")
        || !check(converter.errors().isEmpty(), "a conversão retornou erro")
        || !check(converter.outputs().size() == 1, "o drop no setor PNG não criou uma saída")
        || !check(readFile(input) == original, "o arquivo original foi removido ou modificado")) return 1;
    const QString output = converter.outputs().first();
    if (!check(output != input && QFileInfo::exists(output), "a saída não é um arquivo separado")
        || !check(QFileInfo(output).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0,
                  "o setor superior não selecionou PNG")
        || !check(!QImage(output).isNull(), "a saída PNG não pôde ser lida")) return 1;

    converter.clearFiles();
    QCoreApplication::processEvents();
    QDragEnterEvent moveOnlyEnter(target, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &moveOnlyEnter);
    if (!check(!moveOnlyEnter.isAccepted(), "a cena aceitou uma fonte que só permite MoveAction")) return 1;
    QDragMoveEvent moveOnlyMove(target, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &moveOnlyMove);
    QDropEvent moveOnlyDrop(QPointF(target), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &moveOnlyDrop);
    QTest::qWait(50);
    if (!check(!moveOnlyMove.isAccepted() && !moveOnlyDrop.isAccepted(), "MoveOnly foi aceito após a entrada rejeitada")
        || !check(!converter.busy() && converter.files().isEmpty() && converter.outputs().isEmpty(),
                  "a fonte MoveOnly alterou a seleção ou iniciou conversão")
        || !check(readFile(input) == original, "o original mudou após rejeitar MoveOnly")) return 1;

    QDragEnterEvent shiftEnter(target, actions, &mime, Qt::LeftButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &shiftEnter);
    QDropEvent shiftDrop(QPointF(target), actions, &mime, Qt::LeftButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &shiftDrop);
    if (!check(!shiftEnter.isAccepted() && !shiftDrop.isAccepted(),
               "o fallback Qt aceitou Shift que pode negociar Move no Wayland")
        || !check(!converter.busy() && converter.files().isEmpty(),
                  "o fallback com Shift iniciou uma conversão")) return 1;

    std::puts("qml_drop_check: cópia, saída PNG separada, original preservado, MoveOnly e Shift inseguros rejeitados");
    return 0;
}

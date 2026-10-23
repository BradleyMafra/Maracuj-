#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <cstdio>
#include <functional>
#include <memory>

class SupportFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available MEMBER available CONSTANT)
    Q_PROPERTY(QString pixPayload MEMBER payload CONSTANT)
    Q_PROPERTY(QString pixKey MEMBER key CONSTANT)
    Q_PROPERTY(QUrl qrSource MEMBER qr CONSTANT)
public:
    bool available = true;
    QString payload = QStringLiteral("public-fixture-copy-only");
    QString key = QStringLiteral("123e4567-e12b-12d1-a456-426655440000");
    QUrl qr;
    int copies = 0;
    bool copyResult = true;
    Q_INVOKABLE bool copyPix()
    {
        ++copies;
        if (copyResult) QGuiApplication::clipboard()->setText(payload);
        return copyResult;
    }
};

namespace {
bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "qml_support_check: %s\n", message);
    return condition;
}

bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (condition()) return true;
        QTest::qWait(10);
    } while (timer.elapsed() < 2000);
    return false;
}

QQuickItem *findVisual(QQuickItem *item, const QString &name)
{
    if (!item) return nullptr;
    if (item->objectName() == name) return item;
    for (QQuickItem *child : item->childItems())
        if (QQuickItem *found = findVisual(child, name)) return found;
    return nullptr;
}

bool click(QQuickWindow *window, QQuickItem *item)
{
    if (!item || !item->isVisible() || !item->isEnabled()) return false;
    const QPoint position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    if (position.x() < 0 || position.y() < 0 || position.x() >= window->width()
        || position.y() >= window->height()) return false;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
    QCoreApplication::processEvents();
    return true;
}

std::unique_ptr<QObject> load(QQmlEngine *engine)
{
    QQmlComponent component(engine);
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls
        ApplicationWindow {
            width: 600; height: 680; visible: true
            SupportDialog {
                objectName: "supportDialog"
                supportBridge: typeof supportInfo !== "undefined" ? supportInfo : null
            }
        }
    )QML", QUrl(QStringLiteral("qrc:/mandarina/qml/SupportFixture.qml")));
    std::unique_ptr<QObject> result(component.create());
    if (!result) {
        for (const QQmlError &error : component.errors())
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
    }
    return result;
}
}

int main(int argc, char **argv)
{
    // All pointer/keyboard/clipboard operations stay in this offscreen fixture.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!check(temporary.isValid(), "diretório temporário indisponível")) return 1;
    const QString qrPath = temporary.filePath(QStringLiteral("fixture.svg"));
    QFile svg(qrPath);
    if (!check(svg.open(QIODevice::WriteOnly), "não abriu o SVG isolado")) return 1;
    svg.write("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1 1\"><path d=\"M0 0h1v1H0z\"/></svg>");
    svg.close();
    SupportFixture support;
    support.qr = QUrl::fromLocalFile(qrPath);
    const QString sentinel = QStringLiteral("isolated-qml-support-check");
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(sentinel);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("supportInfo"), &support);
    std::unique_ptr<QObject> object = load(&engine);
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QObject *dialog = object ? object->findChild<QObject *>(QStringLiteral("supportDialog")) : nullptr;
    if (!check(window && dialog, "diálogo não carregou")
        || !check(!dialog->property("visible").toBool(), "apoio abriu automaticamente")
        || !check(support.copies == 0 && clipboard->text() == sentinel, "carregamento copiou sem permissão")
        || !check(QMetaObject::invokeMethod(dialog, "open"), "diálogo não abriu")) return 1;
    if (!check(waitFor([&] { return dialog->property("visible").toBool(); }), "diálogo permaneceu oculto")) return 1;
    QTest::qWait(100);
    auto *content = qvariant_cast<QQuickItem *>(dialog->property("contentItem"));
    QQuickItem *copy = findVisual(content, QStringLiteral("copySupportPix"));
    QQuickItem *expand = findVisual(content, QStringLiteral("showSupportPixPayload"));
    QQuickItem *payload = findVisual(content, QStringLiteral("supportPixPayload"));
    QQuickItem *qr = findVisual(content, QStringLiteral("supportPixQr"));
    QQuickItem *close = findVisual(content, QStringLiteral("closeSupportDialog"));
    if (!check(copy && expand && payload && qr && close, "controles de apoio ausentes")
        || !check(dialog->property("width").toReal() <= 420, "diálogo ultrapassou largura compacta")
        || !check(!payload->isVisible() && payload->property("readOnly").toBool(), "código não iniciou recolhido e somente leitura")
        || !check(qr->property("source").toUrl() == support.qr, "QR não usa o recurso local")
        || !check(support.copies == 0 && clipboard->text() == sentinel, "abrir apoio copiou sem ação explícita")
        || !check(click(window, expand), "não conseguiu expandir o código Pix")
        || !check(waitFor([&] { return payload->isVisible(); }), "expansor não mostrou o código")
        || !check(support.copies == 0 && clipboard->text() == sentinel, "expansão alterou clipboard")
        || !check(click(window, copy), "botão de cópia não está acessível")
        || !check(support.copies == 1 && clipboard->text() == support.payload, "clique não copiou uma única vez")
        || !check(!dialog->property("copyStatus").toString().isEmpty() && !dialog->property("copyFailed").toBool(),
                  "cópia não apresentou confirmação")) return 1;

    clipboard->setText(sentinel);
    if (!check(click(window, close), "botão fechar não está acessível")
        || !check(waitFor([&] { return !dialog->property("visible").toBool(); }), "fechar não ocultou o diálogo")
        || !check(QMetaObject::invokeMethod(dialog, "open"), "diálogo não reabriu")
        || !check(waitFor([&] { return dialog->property("visible").toBool(); }), "reabertura permaneceu oculta")) return 1;
    QTest::qWait(100);
    if (!check(!payload->isVisible() && dialog->property("copyStatus").toString().isEmpty(),
               "reabrir não limpou expansão e confirmação")
        || !check(clipboard->text() == sentinel && support.copies == 1, "reabertura copiou automaticamente")) return 1;
    support.copyResult = false;
    if (!check(click(window, copy), "cópia após reabertura inacessível")
        || !check(support.copies == 2 && clipboard->text() == sentinel
                      && dialog->property("copyFailed").toBool() && payload->isVisible(),
                  "falha de clipboard não ofereceu seleção manual")) return 1;

    // The header and copy action remain reachable when the body needs scrolling.
    window->resize(360, 420);
    QTest::qWait(100);
    window->requestActivate();
    copy->forceActiveFocus();
    if (!check(dialog->property("width").toReal() <= 328 && dialog->property("height").toReal() <= 388,
               "diálogo não respeitou a janela pequena")
        || !check(click(window, close), "fechar ficou fora da janela pequena")
        || !check(waitFor([&] { return !dialog->property("visible").toBool(); }), "fechamento compacto falhou")
        || !check(QMetaObject::invokeMethod(dialog, "open"), "abertura compacta falhou")
        || !check(waitFor([&] { return dialog->property("visible").toBool(); }), "abertura compacta permaneceu oculta")) return 1;
    QTest::qWait(100);
    copy->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Escape);
    if (!check(waitFor([&] { return !dialog->property("visible").toBool(); }), "Escape não fechou o apoio")
        || !check(support.copies == 2 && clipboard->text() == sentinel, "fechamento alterou clipboard")) return 1;

    // Older fixtures and unavailable configurations can load without a backend.
    QQmlEngine emptyEngine;
    std::unique_ptr<QObject> emptyObject = load(&emptyEngine);
    QObject *emptyDialog = emptyObject ? emptyObject->findChild<QObject *>(QStringLiteral("supportDialog")) : nullptr;
    if (!check(emptyDialog && !emptyDialog->property("available").toBool()
                   && !emptyDialog->property("visible").toBool(), "ausência do backend não falhou de modo seguro")) return 1;
    auto *emptyContent = qvariant_cast<QQuickItem *>(emptyDialog->property("contentItem"));
    QQuickItem *emptyCopy = findVisual(emptyContent, QStringLiteral("copySupportPix"));
    if (!check(emptyCopy && !emptyCopy->isEnabled(), "cópia disponível sem dados validados")) return 1;
    std::puts("qml_support_check: opt-in support, local QR, explicit clipboard, errors, collapse, compact layout and Escape");
    return 0;
}

#include "qml_support_check.moc"

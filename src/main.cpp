#include "converter.h"
#include "documentconversion.h"
#include "copyonlydropfilter.h"
#include "desktopintegration.h"
#include "filetoolscontroller.h"
#include "photoeditorcontroller.h"
#include "windowmovehandle.h"
#include "supportinfo.h"
#ifdef Q_OS_WIN
#include "localinstance.h"
#else
#include <QDBusConnection>
#include <QDBusInterface>
#endif
#include <QGuiApplication>
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QSessionManager>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImageReader>
#include <QIcon>
#include <QDir>
#include <QTimer>
#include <QCursor>
#include <QScreen>
#include <cstdio>

int main(int argc, char **argv) {
    bool cli = false;
    for (int i = 1; i < argc; ++i) if (QByteArray(argv[i]) == "--convert" || QByteArray(argv[i]) == "--list-formats") cli = true;
    if (cli) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setApplicationName("Maracujá");
    app.setApplicationVersion(MARACUJA_VERSION);
    app.setOrganizationName("Winux"); app.setDesktopFileName("org.winux.Mandarina");
    app.setWindowIcon(QIcon(":/mandarina/assets/org.winux.Mandarina.svg"));
    QCommandLineParser parser;
    parser.setApplicationDescription("Conversor local de imagens, PDFs, documentos e áudio para Linux e Windows.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument("arquivos", "Arquivos locais para converter.", "[arquivos…]");
    parser.addOption({"convert", "Converter sem abrir a interface.", "formato"});
    parser.addOption({"output", "Pasta de destino; por padrão, ao lado dos originais.", "pasta"});
    parser.addOption({"quality", "Qualidade das imagens (10 a 100).", "valor", "90"});
    parser.addOption({"dpi", "Resolução do PDF (72 a 300 DPI).", "valor", "150"});
    parser.addOption({"list-formats", "Listar formatos de saída disponíveis."});
    parser.addOption({"wheel", "Abrir apenas a roda compacta para os arquivos selecionados."});
    parser.addOption({"background", "Iniciar em segundo plano, sem mostrar a janela."});
    parser.addOption({"quit", "Encerrar a instância em segundo plano deste usuário."});
    parser.addOption({"no-gesture", "Não carregar o atalho global nesta execução."});
    parser.addOption({"screenshot", "Salvar uma prévia da própria interface e sair (desenvolvimento).", "arquivo"});
    parser.process(app);
    if (parser.isSet("list-formats")) {
        const auto bytes = QJsonDocument(QJsonObject{{"images", QJsonArray::fromStringList(Mandarina::imageOutputFormats())},
            {"images_to_pdf", true}, {"pdf", QJsonArray::fromStringList(Mandarina::outputFormats("pdf"))},
            {"images_ocr", QJsonArray::fromStringList(Mandarina::Documents::imageOcrFormats())},
            {"documents", QJsonArray::fromStringList(Mandarina::outputFormats("document"))},
            {"audio", QJsonArray::fromStringList(Mandarina::outputFormats("audio"))}}).toJson();
        std::fwrite(bytes.constData(), 1, bytes.size(), stdout); return 0;
    }
    if (parser.isSet("convert")) {
        QStringList paths;
        for (const auto &raw : parser.positionalArguments()) {
            const QUrl url = QUrl::fromUserInput(raw, QDir::currentPath(), QUrl::AssumeLocalFile);
            if (!url.isLocalFile()) { std::fprintf(stderr, "Somente arquivos locais são aceitos.\n"); return 2; }
            paths << url.toLocalFile();
        }
        if (paths.isEmpty()) { std::fprintf(stderr, "Selecione ao menos um arquivo.\n"); return 2; }
        std::atomic_bool cancel{false};
        const auto result = Mandarina::convert({paths, parser.value("convert"), parser.value("output"), parser.value("quality").toInt(), parser.value("dpi").toInt()}, cancel, {});
        const auto bytes = QJsonDocument(QJsonObject{{"outputs", QJsonArray::fromStringList(result.outputs)}, {"errors", QJsonArray::fromStringList(result.errors)}, {"cancelled", result.cancelled}}).toJson();
        std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
        return result.errors.isEmpty() && !result.cancelled ? 0 : 1;
    }
    const bool desktopMode = !parser.isSet("screenshot") && !parser.isSet("no-gesture");
    const bool startHidden = desktopMode && parser.isSet("background");
    app.setQuitOnLastWindowClosed(!desktopMode);
#ifdef Q_OS_WIN
    LocalInstance instance;
    if (desktopMode && !instance.acquire()) {
        QStringList paths;
        for (const auto &raw : parser.positionalArguments())
            paths << QUrl::fromUserInput(raw, QDir::currentPath(), QUrl::AssumeLocalFile).toString();
        return instance.forward(paths, parser.isSet("wheel"), startHidden, parser.isSet("quit")) ? 0 : 2;
    }
    if (parser.isSet("quit")) return 0;
#else
    auto session = QDBusConnection::sessionBus();
    if (desktopMode && !session.registerService("org.winux.Mandarina")) {
        if (parser.isSet("quit")) {
            QDBusInterface existing("org.winux.Mandarina", "/Mandarina", "org.winux.Mandarina");
            existing.call("Quit"); return 0;
        }
        // A duplicate login launch must not reveal an already resident app.
        if (startHidden && parser.positionalArguments().isEmpty()) return 0;
        QDBusInterface existing("org.winux.Mandarina", "/Mandarina", "org.winux.Mandarina");
        existing.setTimeout(2000);
        QStringList paths;
        for (const auto &raw : parser.positionalArguments())
            paths << QUrl::fromUserInput(raw, QDir::currentPath(), QUrl::AssumeLocalFile).toString();
        const auto reply = existing.call("OpenFiles", paths, parser.isSet("wheel"));
        return reply.type() == QDBusMessage::ErrorMessage ? 2 : 0;
    }
    if (parser.isSet("quit")) return 0;
#endif
    QQuickStyle::setStyle("Basic");
    Converter converter;
    FileToolsController fileTools;
    PhotoEditorController photoEditor;
    const auto updateToolsDestination = [&converter, &fileTools, &photoEditor] {
        fileTools.setOutputDirectory(converter.outputDirectory());
        photoEditor.setOutputDirectory(converter.outputDirectory());
    };
    updateToolsDestination();
    QObject::connect(&converter, &Converter::outputDirectoryChanged, &fileTools, updateToolsDestination);
    DesktopIntegration desktop;
#ifdef Q_OS_WIN
    QObject::connect(&instance, &LocalInstance::openRequested, &desktop, &DesktopIntegration::OpenFiles);
    QObject::connect(&instance, &LocalInstance::quitRequested, &desktop, &DesktopIntegration::Quit);
#endif
    CopyDropPreferenceFilter copyOnly;
    converter.setInitialFiles(parser.positionalArguments());
    SupportInfo supportInfo;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("converter", &converter);
    engine.rootContext()->setContextProperty("fileTools", &fileTools);
    engine.rootContext()->setContextProperty("photoEditor", &photoEditor);
    engine.addImageProvider("photo", photoEditor.createImageProvider());
    engine.rootContext()->setContextProperty("compactMode", parser.isSet("wheel"));
    engine.rootContext()->setContextProperty("residentMode", desktopMode);
    engine.rootContext()->setContextProperty("startInBackground", startHidden);
    engine.rootContext()->setContextProperty("desktopIntegration", &desktop);
    engine.rootContext()->setContextProperty("supportInfo", &supportInfo);
    WindowMoveHandle toolsMover;
    engine.rootContext()->setContextProperty("fileToolsWindowMover", &toolsMover);
    // A separate QML root has no transient parent. A hidden/minimized main
    // converter must stay that way when the drag wheel opens file options.
    QQmlComponent toolsComponent(&engine, QUrl("qrc:/mandarina/qml/FileToolsWindow.qml"));
    auto *toolsWindow = qobject_cast<QQuickWindow *>(toolsComponent.create());
    if (!toolsWindow) {
        for (const auto &error : toolsComponent.errors()) qWarning("%s", qPrintable(error.toString()));
        return 2;
    }
    toolsWindow->QObject::setParent(&engine);
    toolsMover.setWindow(toolsWindow);
    engine.rootContext()->setContextProperty("fileToolsWindow", toolsWindow);
    WindowMoveHandle photoMover;
    engine.rootContext()->setContextProperty("photoEditorWindowMover", &photoMover);
    QQmlComponent photoComponent(&engine, QUrl("qrc:/mandarina/qml/PhotoEditorWindow.qml"));
    auto *photoWindow = qobject_cast<QQuickWindow *>(photoComponent.create());
    if (!photoWindow) {
        for (const auto &error : photoComponent.errors()) qWarning("%s", qPrintable(error.toString()));
        return 2;
    }
    photoWindow->QObject::setParent(&engine);
    photoMover.setWindow(photoWindow);
    engine.rootContext()->setContextProperty("photoEditorWindow", photoWindow);
    QObject::connect(photoWindow, &QWindow::visibleChanged, photoWindow, [photoWindow](bool visible) {
        if (!visible) return;
        auto *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const auto area = screen->availableGeometry();
        photoWindow->setScreen(screen);
        photoWindow->resize(qMin(photoWindow->property("preferredWidth").toInt(), area.width()),
                            qMin(photoWindow->property("preferredHeight").toInt(), area.height()));
        photoWindow->setPosition(qBound(area.left(), QCursor::pos().x() - photoWindow->width() / 2,
                                       area.left() + area.width() - photoWindow->width()),
                                qBound(area.top(), QCursor::pos().y() - photoWindow->height() / 3,
                                       area.top() + area.height() - photoWindow->height()));
    });
    QObject::connect(&fileTools, &FileToolsController::photoRequested, &photoEditor, &PhotoEditorController::openFile);
    QObject::connect(toolsWindow, &QWindow::visibleChanged, toolsWindow, [toolsWindow](bool visible) {
        if (!visible) return;
        const auto cursor = QCursor::pos();
        auto *screen = QGuiApplication::screenAt(cursor);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const auto area = screen->availableGeometry();
        toolsWindow->setScreen(screen);
        const int preferredWidth = toolsWindow->property("preferredWidth").toInt();
        const int preferredHeight = toolsWindow->property("preferredHeight").toInt();
        const int width = qMin(preferredWidth > 0 ? preferredWidth : toolsWindow->width(), area.width());
        const int height = qMin(preferredHeight > 0 ? preferredHeight : toolsWindow->height(), area.height());
        toolsWindow->resize(width, height);
        toolsWindow->setPosition(qBound(area.left(), cursor.x() - width / 2, area.right() - width + 1),
            qBound(area.top(), cursor.y() - height / 3, area.bottom() - height + 1));
        // Wayland placement is handled by our own KWin window-hints script.
    });
    const auto keepToolsOnScreen = [toolsWindow] {
        if (!toolsWindow->isVisible() || QGuiApplication::platformName().startsWith("wayland")) return;
        auto *screen = toolsWindow->screen();
        if (!screen) return;
        const auto area = screen->availableGeometry();
        toolsWindow->setPosition(qBound(area.left(), toolsWindow->x(), area.left() + qMax(0, area.width() - toolsWindow->width())),
            qBound(area.top(), toolsWindow->y(), area.top() + qMax(0, area.height() - toolsWindow->height())));
    };
    // Expanding details or advanced options can grow an already visible card.
    // Clamp only an overflowing edge, preserving the user's chosen position.
    QObject::connect(toolsWindow, &QWindow::heightChanged, toolsWindow, keepToolsOnScreen);
    QObject::connect(toolsWindow, &QWindow::widthChanged, toolsWindow, keepToolsOnScreen);
    engine.load(QUrl("qrc:/mandarina/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 2;
    auto *mainWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    mainWindow->installEventFilter(&copyOnly);
    bool quitPending = false;
    QObject::connect(&desktop, &DesktopIntegration::quitRequested, &app, [&] {
        if (quitPending) return;
        quitPending = true;
        mainWindow->setProperty("exitRequested", true);
        // A hidden window may have no native surface to close. Quit explicitly
        // after the current conversion has acknowledged cooperative cancel.
        photoEditor.cancel();
        if (converter.busy() || fileTools.busy() || photoEditor.busy()) { converter.cancel(); fileTools.cancel(); }
        else app.quit();
    });
    QObject::connect(&converter, &Converter::busyChanged, &app, [&] {
        if (quitPending && !converter.busy() && !fileTools.busy() && !photoEditor.busy()) app.quit();
    });
    QObject::connect(&fileTools, &FileToolsController::busyChanged, &app, [&] {
        if (quitPending && !converter.busy() && !fileTools.busy() && !photoEditor.busy()) app.quit();
    });
    const auto updateOperationBusy = [&] {
        photoEditor.setExternalBusy(converter.busy() || fileTools.busy());
        fileTools.setBlocked(converter.busy() || photoEditor.cancellable() || photoEditor.rendering());
        desktop.setToolsBusy(fileTools.busy() || photoEditor.busy() || photoEditor.rendering());
        if (quitPending && !converter.busy() && !fileTools.busy() && !photoEditor.busy()) app.quit();
    };
    QObject::connect(&converter, &Converter::busyChanged, &app, updateOperationBusy);
    QObject::connect(&fileTools, &FileToolsController::busyChanged, &app, updateOperationBusy);
    QObject::connect(&photoEditor, &PhotoEditorController::busyChanged, &app, updateOperationBusy);
    QObject::connect(&photoEditor, &PhotoEditorController::changed, &app, updateOperationBusy);
    QObject::connect(&desktop, &DesktopIntegration::toolsDropRequested, &fileTools, &FileToolsController::openFiles);
    QObject::connect(&app, &QGuiApplication::commitDataRequest, mainWindow,
        [mainWindow](QSessionManager &) { mainWindow->setProperty("exitRequested", true); });
    QObject::connect(&desktop, &DesktopIntegration::openRequested, &app,
        [&converter, mainWindow](const QStringList &paths, bool compact) {
            if (!converter.busy()) {
                if (!paths.isEmpty()) {
                    converter.clearFiles();
                    converter.setInitialFiles(paths);
                }
                // Primary conversion gestures always return to the format
                // wheel, even if the full panel was last used for editing.
                if (compact || !paths.isEmpty()) mainWindow->setProperty("editingMode", false);
                mainWindow->setProperty("compact", compact);
            }
            mainWindow->showNormal();
            mainWindow->requestActivate();
        });
    QMenu trayMenu;
    QSystemTrayIcon tray(app.windowIcon());
    if (desktopMode) {
        auto *open = trayMenu.addAction("Abrir Maracujá");
        trayMenu.setDefaultAction(open);
        QObject::connect(open, &QAction::triggered, &desktop, [&desktop] { desktop.OpenFiles({}, false); });
        auto *gesture = trayMenu.addAction("Atalho do Maracujá");
        gesture->setCheckable(true);
        gesture->setEnabled(false);
        QObject::connect(gesture, &QAction::triggered, &desktop, &DesktopIntegration::setEnabled);
        QObject::connect(&desktop, &DesktopIntegration::changed, &tray, [gesture, &desktop, &tray] {
            gesture->setText(desktop.shortcutLabel() + (desktop.dragRequired() ? " + arrastar" : " · abrir roda"));
            gesture->setChecked(desktop.enabled());
            gesture->setEnabled(desktop.available());
            tray.setToolTip("Maracujá · " + desktop.status());
        });
        trayMenu.addSeparator();
        auto *quit = trayMenu.addAction("Sair do Maracujá");
        QObject::connect(quit, &QAction::triggered, &desktop, &DesktopIntegration::Quit);
        tray.setContextMenu(&trayMenu);
        tray.setToolTip("Maracujá · Preparando o atalho");
        QObject::connect(&tray, &QSystemTrayIcon::activated, &desktop,
            [&desktop](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    desktop.OpenFiles({}, false);
            });
        // Qt registers with StatusNotifierWatcher even when the notification
        // area becomes available after this app starts during login.
        tray.show();
#ifndef Q_OS_WIN
        session.registerObject("/Mandarina", &desktop, QDBusConnection::ExportScriptableSlots);
#endif
        QObject::connect(&desktop, &DesktopIntegration::notificationRequested, &tray,
            [&tray](const QString &title, const QString &message) {
                tray.showMessage(title, message, QSystemTrayIcon::Information, 5000);
            });
        desktop.setConverter(&converter);
        QTimer::singleShot(350, &desktop, &DesktopIntegration::start);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &desktop, &DesktopIntegration::stop);
    }
    if (parser.isSet("screenshot")) {
        const QString destination = parser.value("screenshot");
        QTimer::singleShot(1000, &app, [&engine, &app, destination] {
            const auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            app.exit(window && window->grabWindow().save(destination) ? 0 : 3);
        });
    }
    return app.exec();
}

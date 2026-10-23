// Exercise only this process's OLE target with generated files. No DoDragDrop,
// input injection, global key polling or existing desktop window is used.
#include "windowsgesture.h"
#include "converter.h"

#ifndef Q_OS_WIN
#error "windows_gesture_check must only be built for Windows"
#endif
#ifndef MARACUJA_WINDOWS_GESTURE_TESTING
#error "Build this test and windowsgesture.cpp with MARACUJA_WINDOWS_GESTURE_TESTING"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPoint>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QVariantMap>
#include <QWidget>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read the generated fixture");
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

struct DataObject final : IDataObject {
    explicit DataObject(const QStringList &paths)
    {
        for (const auto &path : paths) {
            const auto native = QDir::toNativeSeparators(path).toStdWString();
            files.insert(files.end(), native.begin(), native.end());
            files.push_back(L'\0');
        }
        files.push_back(L'\0');
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDataObject)) return E_NOINTERFACE;
        *object = static_cast<IDataObject *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&references)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG left = InterlockedDecrement(&references);
        if (!left) delete this;
        return ULONG(left);
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override
    {
        return format && format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL)
            && format->dwAspect == DVASPECT_CONTENT && format->lindex == -1 ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
    {
        if (!medium) return E_POINTER;
        *medium = {};
        if (QueryGetData(format) != S_OK) return DV_E_FORMATETC;
        const SIZE_T bytes = sizeof(DROPFILES) + files.size() * sizeof(wchar_t);
        HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!allocation) return E_OUTOFMEMORY;
        auto *memory = static_cast<unsigned char *>(GlobalLock(allocation));
        if (!memory) { GlobalFree(allocation); return E_OUTOFMEMORY; }
        DROPFILES header {};
        header.pFiles = sizeof(DROPFILES);
        header.fWide = TRUE;
        std::memcpy(memory, &header, sizeof(header));
        std::memcpy(memory + sizeof(header), files.data(), files.size() * sizeof(wchar_t));
        GlobalUnlock(allocation);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = allocation;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *format) override
    {
        if (format) format->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **enumeration) override
    {
        if (enumeration) *enumeration = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
    { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **enumeration) override
    {
        if (enumeration) *enumeration = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }
    std::vector<wchar_t> files;
    volatile LONG references = 1;
};

struct DataReference {
    explicit DataReference(const QStringList &paths) : object(new DataObject(paths)) {}
    ~DataReference() { object->Release(); }
    DataObject *object;
};

struct TargetReference {
    explicit TargetReference(IDropTarget *value) : object(value) { if (object) object->AddRef(); }
    ~TargetReference() { if (object) object->Release(); }
    IDropTarget *object;
};

struct ReservedFunctionHotkey {
    explicit ReservedFunctionHotkey(int identity, int excluded = 0) : id(identity)
    {
        for (int candidate = Qt::Key_F11; candidate >= Qt::Key_F1; --candidate) {
            if (candidate == excluded) continue;
            if (RegisterHotKey(nullptr, id, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                               UINT(VK_F1 + candidate - Qt::Key_F1))) {
                key = candidate;
                reserved = true;
                return;
            }
        }
        throw std::runtime_error("No free function-key combination is available for the owned hotkey fixture");
    }
    ~ReservedFunctionHotkey() { release(); }
    void release() { if (reserved) UnregisterHotKey(nullptr, id); reserved = false; }
    int id;
    int key = 0;
    bool reserved = false;
};

POINTL screenPoint(HWND window, double x, double y)
{
    RECT rectangle {};
    require(GetClientRect(window, &rectangle), "Cannot read the owned target geometry");
    POINT point {LONG(std::lround(x * (rectangle.right - rectangle.left) / 460.0)),
                 LONG(std::lround(y * (rectangle.bottom - rectangle.top) / 460.0))};
    require(ClientToScreen(window, &point), "Cannot map the owned target coordinates");
    return POINTL {point.x, point.y};
}

POINTL pngSector(HWND window, const QVariantList &formats)
{
    int index = -1;
    for (int item = 0; item < formats.size(); ++item)
        if (formats.at(item).toMap().value("id").toString() == QStringLiteral("png")) index = item;
    require(index >= 0, "PNG is unavailable for the generated image");
    constexpr double pi = 3.14159265358979323846;
    const double angle = -pi / 2 + index * pi * 2 / double(formats.size());
    return screenPoint(window, 230 + 140 * std::cos(angle), 230 + 140 * std::sin(angle));
}

QRect nativeBounds(HWND window)
{
    RECT rectangle {};
    require(GetWindowRect(window, &rectangle), "Cannot read the owned wheel bounds");
    return QRect(rectangle.left, rectangle.top, rectangle.right - rectangle.left,
                 rectangle.bottom - rectangle.top);
}

QImage renderedWheel(HWND window)
{
    // Render only our own QWidget after its opening animation. No desktop
    // capture is needed to inspect the production painter's alpha channel.
    QElapsedTimer animation;
    animation.start();
    while (animation.elapsed() < 450) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    auto *widget = QWidget::find(reinterpret_cast<WId>(window));
    require(widget && widget->isVisible(), "The owned wheel has no visible widget");
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget->render(&image);
    return image;
}

int scaledAlpha(const QImage &image, double x, double y)
{
    return image.pixelColor(int(std::lround(x * image.width() / 460.0)),
                            int(std::lround(y * image.height() / 460.0))).alpha();
}

struct PanelState {
    explicit PanelState(const Converter &converter)
        : files(converter.files()), formats(converter.formats()), kind(converter.inputKind()),
          selected(converter.selectedFormat()), preview(converter.dragPreview()) {}
    void requireUnchanged(const Converter &converter, const char *message) const
    {
        require(converter.files() == files && converter.formats() == formats
                    && converter.inputKind() == kind && converter.selectedFormat() == selected
                    && converter.dragPreview() == preview, message);
    }
    QVariantList files, formats;
    QString kind, selected;
    bool preview;
};

void enter(IDropTarget *target, IDataObject *source, POINTL point, DWORD effects, DWORD expected)
{
    require(target->DragEnter(source, MK_LBUTTON | MK_SHIFT, point, &effects) == S_OK,
            "OLE DragEnter failed");
    require(effects == expected, "OLE DragEnter negotiated an unsafe or unexpected action");
}

void over(IDropTarget *target, POINTL point, DWORD effects, DWORD expected)
{
    require(target->DragOver(MK_LBUTTON | MK_SHIFT, point, &effects) == S_OK, "OLE DragOver failed");
    require(effects == expected, "OLE DragOver negotiated an unsafe or unexpected action");
}

void drop(IDropTarget *target, IDataObject *source, POINTL point, DWORD effects, DWORD expected)
{
    require(target->Drop(source, MK_SHIFT, point, &effects) == S_OK, "OLE Drop failed");
    require(effects == expected, "OLE Drop negotiated an unsafe or unexpected action");
}

void finishConversion(Converter &converter)
{
    QElapsedTimer deadline;
    deadline.start();
    // The accepted drop is deliberately queued until after OLE has returned.
    while ((converter.outputs().isEmpty() || converter.busy()) && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(!converter.busy() && converter.errors().isEmpty() && converter.outputs().size() == 1,
            "The accepted OLE copy did not produce exactly one converted image");
    require(QFileInfo::exists(converter.outputs().first()), "The OLE conversion output is missing");
}

void writeFixture(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(contents) == contents.size(),
            "Cannot create the isolated discovery fixture");
}

void waitFormats(Converter &converter)
{
    QElapsedTimer timer;
    timer.start();
    while (converter.formatsLoading() && timer.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(!converter.formatsLoading(), "Background format discovery did not finish");
}

void checkColdPendingDrag()
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create the cold OLE fixture directory");
    const QDir root(temporary.path());
    const QString config = root.filePath("runtime-paths.json");
    writeFixture(config, QJsonDocument(QJsonObject{{"python", QCoreApplication::applicationFilePath()}}).toJson());
    writeFixture(root.filePath("document_convert.py"), "synthetic capability fixture");
    // Windows' narrow CRT environment does not interpret UTF-8 paths. Update
    // the wide environment so temporary directories containing accents survive
    // both the Qt lookup and the owned child process's environment snapshot.
    require(_wputenv_s(L"MARACUJA_RUNTIME_CONFIG", reinterpret_cast<const wchar_t *>(config.utf16())) == 0
                && qEnvironmentVariable("MARACUJA_RUNTIME_CONFIG") == config,
            "The private runtime configuration did not preserve its Unicode path");
    const QString source = root.filePath("pending.txt"), removed = root.filePath("removed.txt");
    writeFixture(source, "Cold OLE document");
    writeFixture(removed, "Removed before discovery finishes");
    const QString imagePath = root.filePath("image.png");
    QImage image(32, 24, QImage::Format_RGB32);
    image.fill(Qt::green);
    require(image.save(imagePath, "PNG"), "Cannot create cold OLE image fixture");
    const auto original = digest(imagePath);
    Converter converter;
    converter.setInitialFiles({imagePath});
    converter.setSelectedFormat("jpg");
    const auto files = converter.files();
    const auto originalFormats = converter.formats();
    require(converter.formatsLoading() && converter.formatsForUrls({QUrl::fromLocalFile(source)}).isEmpty(),
            "The cold document fixture was not held pending");
    std::vector<std::unique_ptr<WindowsGesture>> gestures;
    std::vector<std::unique_ptr<TargetReference>> targets;
    std::vector<HWND> windows;
    for (int index = 0; index < 5; ++index) {
        auto gesture = std::make_unique<WindowsGesture>(&converter);
        require(gesture->initializeForTest(), "Cannot initialize a cold owned OLE target");
        gesture->showForTest(QPoint(600, 400));
        windows.push_back(static_cast<HWND>(gesture->nativeWindowForTest()));
        targets.push_back(std::make_unique<TargetReference>(static_cast<IDropTarget *>(gesture->dropTargetForTest())));
        gestures.push_back(std::move(gesture));
    }
    DataReference document({source}), vanished({removed}), photo({imagePath});
    for (int index = 0; index < 4; ++index) {
        enter(targets[index]->object, index == 3 ? vanished.object : document.object,
              screenPoint(windows[index], 230, 90), DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
    }
    // A pending drag survives the ordinary 450 ms probe timeout, but cancelled,
    // replaced or invalidated drags must not revive when capabilities arrive.
    require(targets[1]->object->DragLeave() == S_OK, "Cannot leave the pending OLE target");
    enter(targets[2]->object, photo.object, screenPoint(windows[2], 230, 90), DROPEFFECT_MOVE, DROPEFFECT_NONE);
    require(QFile::remove(removed), "Cannot remove the generated stale drag source");
    constexpr double pi = 3.14159265358979323846;
    const double angle = -pi / 2 + 0.45 * 2 * pi / originalFormats.size();
    const auto frozenSector = screenPoint(windows[4], 230 + 140 * std::cos(angle), 230 + 140 * std::sin(angle));
    enter(targets[4]->object, photo.object, frozenSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
    QElapsedTimer held;
    held.start();
    while (held.elapsed() < 650) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(IsWindowVisible(windows[0]) && converter.formatsLoading(),
            "The cold pending target expired before discovery finished");
    over(targets[0]->object, screenPoint(windows[0], 230, 90), DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
    writeFixture(root.filePath("release-capabilities"), "ready");
    waitFormats(converter);
    require(converter.formats().size() > originalFormats.size(), "The fixture did not add optional OCR sectors");
    require(converter.files() == files && converter.selectedFormat() == "jpg" && !converter.dragPreview(),
            "Cold native discovery replaced the main panel's selection");
    // Recovery occurs in the same OLE operation, with no second DragEnter.
    over(targets[0]->object, screenPoint(windows[0], 230, 90), DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
    drop(targets[0]->object, photo.object, screenPoint(windows[0], 230, 90),
         DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
    for (int index : {1, 2, 3}) {
        require(!IsWindowVisible(windows[index]), "An invalid pending drag revived after discovery");
        over(targets[index]->object, screenPoint(windows[index], 230, 90), DROPEFFECT_COPY, DROPEFFECT_NONE);
    }
    // A wheel that was already valid keeps its original sectors: otherwise this
    // near-boundary pointer would switch from PNG to JPEG as OCR sectors appear.
    over(targets[4]->object, frozenSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
    drop(targets[4]->object, photo.object, frozenSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
    finishConversion(converter);
    require(QFileInfo(converter.outputs().first()).suffix() == "png" && digest(imagePath) == original,
            "Completing discovery moved an active sector or changed the original");
    std::puts("Cold OLE pending recovery, stale rejection and fixed active sectors passed.");
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "windows");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        const auto arguments = app.arguments();
        if (arguments.contains("--capabilities")) {
            const QDir root(arguments.value(arguments.indexOf("--runtime-root") + 1));
            QElapsedTimer timer;
            timer.start();
            while (!QFileInfo::exists(root.filePath("release-capabilities")) && timer.elapsed() < 4000)
                QThread::msleep(10);
            if (!QFileInfo::exists(root.filePath("release-capabilities"))) return 2;
            std::puts("{\"ok\":true,\"documents\":[\"pdf\",\"docx\",\"txt\"],\"pdf\":[],\"image_ocr\":[\"txt\",\"docx\"]}");
            return 0;
        }
        if (arguments.contains("--cold-pending-check")) {
            checkColdPendingDrag();
            return 0;
        }
        QProcess coldCheck;
        coldCheck.setProcessChannelMode(QProcess::ForwardedChannels);
        coldCheck.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
        coldCheck.start(app.applicationFilePath(), {"--cold-pending-check"});
        require(coldCheck.waitForStarted() && coldCheck.waitForFinished(40000)
                    && coldCheck.exitStatus() == QProcess::NormalExit && coldCheck.exitCode() == 0,
                "Cold OLE pending discovery regression failed");
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create the private fixture directory");
        const QDir root(temporary.path());
        require(root.mkdir("outputs"), "Cannot create the private output directory");
        const QString source = root.filePath(QStringLiteral("foto ação com espaços.png"));
        QImage image(128, 96, QImage::Format_RGB32);
        image.fill(QColor("#9d72c6"));
        require(image.save(source, "PNG"), "Cannot write the generated PNG fixture");
        const auto original = digest(source);
        Converter converter;
        converter.setOutputDirectory(root.filePath("outputs"));
        // Keep an unrelated document and format in the main panel. The native
        // image wheel must discover its own formats without temporarily turning
        // that document wheel into an image wheel, even on a cancelled gesture.
        const QString document = root.filePath(QStringLiteral("seleção do painel.txt"));
        {
            QFile text(document);
            require(text.open(QIODevice::WriteOnly) && text.write("Documento do painel.\n") > 0,
                    "Cannot create the independent panel selection");
        }
        converter.setInitialFiles({document});
        waitFormats(converter);
        converter.setSelectedFormat(QStringLiteral("md"));
        require(converter.inputKind() == QStringLiteral("document")
                    && converter.selectedFormat() == QStringLiteral("md"),
                "Cannot prepare the independent document wheel");
        const PanelState panel(converter);
        const QVariantList sourceUrls {QUrl::fromLocalFile(source)};
        const auto nativeFormats = converter.formatsForUrls(sourceUrls);
        require(!nativeFormats.isEmpty() && nativeFormats != panel.formats,
                "The fixture does not distinguish native and main-panel formats");
        panel.requireUnchanged(converter, "Discovering native formats mutated the panel");
        WindowsGesture gesture(&converter);
        int hoverSounds = 0;
        QObject::connect(&gesture, &WindowsGesture::wheelSectorHovered, &app, [&] { ++hoverSounds; });
        require(gesture.toolsShortcutMatchesForTest(Qt::AltModifier, 0)
                    && !gesture.toolsShortcutMatchesForTest(Qt::ShiftModifier | Qt::AltModifier, 0),
                "The default tools trigger does not require exactly Alt");
        require(!gesture.setToolsShortcut(Qt::ShiftModifier, 0)
                    && !gesture.setShortcut(Qt::AltModifier, 0)
                    && !gesture.setToolsShortcut(Qt::NoModifier, 0)
                    && gesture.toolsShortcutMatchesForTest(Qt::AltModifier, 0)
                    && gesture.shortcutMatchesForTest(Qt::ShiftModifier, 0),
                "A conflicting or empty tool gesture replaced one of the working shortcuts");
        require(gesture.setToolsShortcut(Qt::ControlModifier | Qt::AltModifier, 0)
                    && gesture.toolsShortcutMatchesForTest(Qt::ControlModifier | Qt::AltModifier, 0)
                    && !gesture.toolsShortcutMatchesForTest(Qt::AltModifier, 0)
                    && gesture.setToolsShortcut(Qt::AltModifier, 0),
                "The independent tools shortcut did not apply its exact modifiers");
        require(gesture.setActivations(Qt::AltModifier, 0, true, Qt::ShiftModifier, 0)
                    && gesture.shortcutMatchesForTest(Qt::AltModifier, 0)
                    && gesture.toolsShortcutMatchesForTest(Qt::ShiftModifier, 0)
                    && !gesture.setActivations(Qt::AltModifier, 0, true, Qt::AltModifier, 0)
                    && gesture.toolsShortcutMatchesForTest(Qt::ShiftModifier, 0)
                    && gesture.setActivations(Qt::ShiftModifier, 0, true, Qt::AltModifier, 0),
                "A saved shortcut pair could not swap the defaults atomically or rolled back incorrectly");
        require(gesture.shortcutMatchesForTest(Qt::ShiftModifier, 0)
                    && !gesture.shortcutMatchesForTest(Qt::ControlModifier | Qt::ShiftModifier, 0),
                "The default trigger does not require exactly Shift");
        require(gesture.setShortcut(Qt::ControlModifier | Qt::AltModifier, Qt::Key_K),
                "A configured modifier/key combination was rejected");
        require(gesture.shortcutMatchesForTest(Qt::ControlModifier | Qt::AltModifier, Qt::Key_K)
                    && !gesture.shortcutMatchesForTest(Qt::ControlModifier | Qt::AltModifier, Qt::Key_J)
                    && !gesture.shortcutMatchesForTest(Qt::AltModifier, Qt::Key_K)
                    && !gesture.shortcutMatchesForTest(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, Qt::Key_K)
                    && gesture.shortcutVirtualKeyForTest() == 'K',
                "The configured trigger matched the wrong key or modifier state");
        require(!gesture.setShortcut(Qt::NoModifier, 0)
                    && !gesture.setShortcut(Qt::ShiftModifier, Qt::Key_Escape)
                    && !gesture.setShortcut(Qt::KeypadModifier, Qt::Key_5)
                    && gesture.shortcutMatchesForTest(Qt::ControlModifier | Qt::AltModifier, Qt::Key_K),
                "An unsupported trigger replaced the working configuration");
        require(gesture.setShortcut(Qt::NoModifier, Qt::Key_F12)
                    && gesture.shortcutVirtualKeyForTest() == VK_F12
                    && gesture.shortcutMatchesForTest(Qt::NoModifier, Qt::Key_F12),
                "A configured function key does not map to the native Windows key");
        require(gesture.setShortcut(Qt::ShiftModifier, Qt::Key_5)
                    && gesture.shortcutVirtualKeyForTest() == '5',
                "A configured digit does not map to the unshifted native Windows key");
        require(gesture.setShortcut(Qt::ControlModifier | Qt::ShiftModifier, 0)
                    && gesture.shortcutVirtualKeyForTest() == 0
                    && gesture.shortcutMatchesForTest(Qt::ControlModifier | Qt::ShiftModifier, 0),
                "A modifiers-only combination was rejected");
        require(gesture.setShortcut(Qt::ShiftModifier, 0), "Cannot restore the default trigger");
        require(gesture.prepare() && gesture.available() && !gesture.enabled() && !gesture.ownsHotkeyForTest(),
                "Preparing desktop capability unexpectedly activated input or reserved a hotkey");
        require(gesture.initializeForTest() && gesture.available(), "Cannot initialize the owned Windows OLE target");
        const HWND window = static_cast<HWND>(gesture.nativeWindowForTest());
        TargetReference target(static_cast<IDropTarget *>(gesture.dropTargetForTest()));
        require(window && target.object, "The owned OLE target is absent");
        require((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
                "The wheel does not carry the no-activation policy");
        DataReference data({source});

        // A hidden/stale target must never accept a drag behind the user's back.
        enter(target.object, data.object, POINTL{0, 0}, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        panel.requireUnchanged(converter, "A hidden native target changed the panel");

        gesture.showForTest(QPoint(600, 400));
        require(gesture.setShortcut(Qt::ControlModifier | Qt::AltModifier, Qt::Key_K)
                    && gesture.status().contains(QStringLiteral("Ctrl+Alt+K")),
                "Changing the live trigger did not update the gesture status");
        require(!IsWindowVisible(window), "Changing the trigger left an old candidate active");
        enter(target.object, data.object, POINTL{0, 0}, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        require(gesture.setShortcut(Qt::ShiftModifier, 0), "Cannot restore Shift for the OLE copy checks");

        gesture.showForTest(QPoint(600, 400));
        const POINTL center = screenPoint(window, 230, 230);
        enter(target.object, data.object, center, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        require(hoverSounds == 0, "Preparing or opening the wheel signalled a hover sound");
        panel.requireUnchanged(converter, "Native DragEnter replaced the panel's wheel or selection");
        const POINTL sector = pngSector(window, nativeFormats);
        over(target.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        require(hoverSounds == 1, "Entering a valid sector did not signal exactly one hover sound");
        over(target.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        require(hoverSounds == 1, "Remaining in the same sector repeated the hover sound");
        over(target.object, center, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        require(hoverSounds == 1, "Leaving a sector signalled a hover sound");
        over(target.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        require(hoverSounds == 2, "Reentering an actionable sector did not signal its hover sound");
        panel.requireUnchanged(converter, "Native DragOver changed the panel's wheel or selection");
        drop(target.object, data.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        require(hoverSounds == 2, "Dropping or hiding the wheel repeated the hover sound");
        panel.requireUnchanged(converter, "Native Drop changed the panel before returning to OLE");
        finishConversion(converter);
        panel.requireUnchanged(converter, "A completed native conversion replaced the panel's files or wheel");
        require(QImage(converter.outputs().first()).size() == image.size(),
                "The native conversion used the panel's document instead of the dropped image");
        require(digest(source) == original, "An accepted OLE copy changed the original");
        const auto outputs = QDir(root.filePath("outputs")).entryList(QDir::Files);

        gesture.showForTest(QPoint(600, 400));
        enter(target.object, data.object, center, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        drop(target.object, data.object, center, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        require(!converter.busy(), "Dropping in the wheel center started a conversion");
        panel.requireUnchanged(converter, "Cancelling in the native wheel center changed the panel");

        gesture.showForTest(QPoint(600, 400));
        enter(target.object, data.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        require(target.object->DragLeave() == S_OK, "OLE DragLeave failed");
        panel.requireUnchanged(converter, "Native DragLeave changed the panel's wheel or selection");

        // A preview initiated by the panel itself also belongs to the panel.
        // Hiding or replacing the native wheel must not clear that preview.
        require(converter.previewUrls(sourceUrls), "Cannot prepare an owned panel drag preview");
        converter.setSelectedFormat(QStringLiteral("jpg"));
        const PanelState panelPreview(converter);
        gesture.showForTest(QPoint(600, 400));
        enter(target.object, data.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        panelPreview.requireUnchanged(converter, "Native DragEnter changed an existing panel preview");
        enter(target.object, data.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        panelPreview.requireUnchanged(converter, "Replacing a native preview cleared the panel's preview");
        require(target.object->DragLeave() == S_OK, "OLE DragLeave failed with an active panel preview");
        panelPreview.requireUnchanged(converter, "Native DragLeave cleared the panel's own preview");
        converter.clearDragPreview();
        converter.setSelectedFormat(panel.selected);
        panel.requireUnchanged(converter, "Cannot restore the document panel after its own preview");

        const int soundsBeforeRejection = hoverSounds;
        gesture.showForTest(QPoint(600, 400));
        enter(target.object, data.object, sector, DROPEFFECT_MOVE, DROPEFFECT_NONE);
        over(target.object, sector, DROPEFFECT_MOVE, DROPEFFECT_NONE);
        drop(target.object, data.object, sector, DROPEFFECT_MOVE, DROPEFFECT_NONE);
        require(!converter.busy(), "A MOVE-only source started a conversion");
        require(hoverSounds == soundsBeforeRejection, "A rejected MOVE-only drag signalled a hover sound");
        panel.requireUnchanged(converter, "A rejected MOVE-only source changed the panel");

        const QString broken = root.filePath("broken.png");
        QFile invalid(broken);
        require(invalid.open(QIODevice::WriteOnly), "Cannot create the invalid fixture");
        require(invalid.write("not an image") > 0, "Cannot write the invalid fixture");
        invalid.close();
        DataReference mixed({source, broken});
        gesture.showForTest(QPoint(600, 400));
        enter(target.object, mixed.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        over(target.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        drop(target.object, mixed.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        require(!converter.busy(), "A partially unsupported selection started a conversion");
        require(hoverSounds == soundsBeforeRejection, "An invalid file selection signalled a hover sound");
        panel.requireUnchanged(converter, "An invalid native selection changed the panel");
        require(QDir(root.filePath("outputs")).entryList(QDir::Files) == outputs,
                "A rejected or cancelled drop published an output");
        require(digest(source) == original, "A rejected OLE drop changed the original");

        const QString arbitrary = root.filePath(QStringLiteral("arquivo não conversível.bin"));
        QFile binary(arbitrary);
        require(binary.open(QIODevice::WriteOnly) && binary.write("private generated fixture") > 0,
                "Cannot create the arbitrary file-tools fixture");
        binary.close();
        const auto binaryOriginal = digest(arbitrary);
        DataReference arbitraryData({arbitrary, source});
        const auto conversionFiles = converter.files();
        const auto conversionOutputs = converter.outputs();
        int toolsCount = 0;
        QStringList toolsPaths;
        QString toolsAction;
        QObject::connect(&gesture, &WindowsGesture::toolsRequested, &app,
                         [&](const QStringList &paths, const QString &tool) {
            ++toolsCount;
            toolsPaths = paths;
            toolsAction = tool;
        });
        gesture.showToolsForTest(QPoint(600, 400));
        const POINTL compressSector = screenPoint(window, 230, 90);
        const POINTL metadataSector = screenPoint(window, 230, 370);
        enter(target.object, arbitraryData.object, compressSector,
              DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        drop(target.object, arbitraryData.object, compressSector,
             DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        require(toolsCount == 0, "A tool panel opened before the OLE negotiation returned");
        QCoreApplication::processEvents();
        require(toolsCount == 1 && toolsAction == QStringLiteral("compress")
                    && toolsPaths == QStringList({QFileInfo(arbitrary).canonicalFilePath(), QFileInfo(source).canonicalFilePath()})
                    && !converter.busy() && converter.files() == conversionFiles
                    && converter.outputs() == conversionOutputs,
                "A tool drop rejected ordinary files, lost its selection or implicitly converted them");
        gesture.showToolsForTest(QPoint(600, 400));
        enter(target.object, arbitraryData.object, metadataSector, DROPEFFECT_COPY, DROPEFFECT_COPY);
        drop(target.object, arbitraryData.object, metadataSector, DROPEFFECT_COPY, DROPEFFECT_COPY);
        QCoreApplication::processEvents();
        require(toolsCount == 2 && toolsAction == QStringLiteral("metadata"),
                "The second tools sector did not request metadata inspection");
        gesture.showToolsForTest(QPoint(600, 400));
        enter(target.object, arbitraryData.object, compressSector, DROPEFFECT_MOVE, DROPEFFECT_NONE);
        drop(target.object, arbitraryData.object, compressSector, DROPEFFECT_MOVE, DROPEFFECT_NONE);
        QCoreApplication::processEvents();
        require(toolsCount == 2, "A MOVE-only source opened a tool");
        DataReference directoryData({root.path()});
        gesture.showToolsForTest(QPoint(600, 400));
        enter(target.object, directoryData.object, compressSector, DROPEFFECT_COPY, DROPEFFECT_NONE);
        drop(target.object, directoryData.object, compressSector, DROPEFFECT_COPY, DROPEFFECT_NONE);
        QCoreApplication::processEvents();
        require(toolsCount == 2 && digest(arbitrary) == binaryOriginal && digest(source) == original,
                "A tools drop accepted a directory or modified an original fixture");
        panel.requireUnchanged(converter, "The independent tools wheel changed the panel's wheel");
        gesture.showToolsForTest(QPoint(600, 400));
        gesture.setExternalBusy(true);
        require(!IsWindowVisible(window), "An external file operation left a tools probe visible");
        gesture.showToolsForTest(QPoint(600, 400));
        require(!IsWindowVisible(window), "A tools probe opened during another file operation");
        gesture.showForTest(QPoint(600, 400));
        require(!IsWindowVisible(window), "A conversion probe opened during another file operation");
        gesture.setExternalBusy(false);
        gesture.showToolsForTest(QPoint(600, 400));
        enter(target.object, arbitraryData.object, compressSector, DROPEFFECT_COPY, DROPEFFECT_COPY);
        drop(target.object, arbitraryData.object, compressSector, DROPEFFECT_COPY, DROPEFFECT_COPY);
        gesture.setExternalBusy(true);
        QCoreApplication::processEvents();
        require(toolsCount == 2, "A queued tools drop bypassed a newly active file operation");
        gesture.setExternalBusy(false);

        require(!gesture.setActivation(Qt::NoModifier, 0, false)
                    && !gesture.setActivation(Qt::ShiftModifier, 0, false)
                    && !gesture.setActivation(Qt::ControlModifier, Qt::Key_F12, false)
                    && !gesture.setActivation(Qt::NoModifier, Qt::Key_M, false)
                    && !gesture.setActivation(Qt::ShiftModifier, Qt::Key_M, false)
                    && gesture.requiresDragForTest(),
                "An unsafe or modifier-only standalone key replaced the drag gesture");
        const Qt::KeyboardModifiers fixtureModifiers = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
        ReservedFunctionHotkey available(0x3901);
        const int firstKey = available.key;
        available.release();
        require(gesture.setActivation(fixtureModifiers, firstKey, false)
                    && !gesture.requiresDragForTest() && gesture.ownsHotkeyForTest(),
                "A valid standalone hotkey was not registered with Windows");
        int activationCount = 0;
        QObject::connect(&gesture, &WindowsGesture::activationRequested, &app, [&] { ++activationCount; });
        require(gesture.dispatchHotkeyForTest() && activationCount == 1,
                "The adapter did not dispatch its own reserved hotkey message");
        ReservedFunctionHotkey conflict(0x3902, firstKey);
        require(!gesture.setActivation(fixtureModifiers, conflict.key, false)
                    && !gesture.errorString().isEmpty()
                    && gesture.ownsHotkeyForTest()
                    && !gesture.requiresDragForTest()
                    && gesture.shortcutMatchesForTest(fixtureModifiers, firstKey)
                    && gesture.dispatchHotkeyForTest() && activationCount == 2,
                "A hotkey conflict disabled or replaced the previously working activation");
        const int secondKey = conflict.key;
        conflict.release();
        require(gesture.setActivation(fixtureModifiers, secondKey, false)
                    && gesture.errorString().isEmpty()
                    && gesture.shortcutMatchesForTest(fixtureModifiers, secondKey),
                "The standalone shortcut did not switch after the conflict was released");
        gesture.setEnabled(false);
        require(!gesture.enabled() && !gesture.ownsHotkeyForTest()
                    && gesture.setActivation(fixtureModifiers, firstKey, false)
                    && !gesture.enabled() && !gesture.ownsHotkeyForTest(),
                "Editing a suspended shortcut unexpectedly kept global input reserved");
        gesture.setEnabled(true);
        require(gesture.enabled() && gesture.ownsHotkeyForTest() && gesture.errorString().isEmpty(),
                "Resuming capture did not reserve the updated standalone shortcut");
        gesture.showForTest(QPoint(600, 400));
        require(!IsWindowVisible(window), "The standalone mode opened an uninvited file-drop candidate");
        gesture.showToolsForTest(QPoint(600, 400));
        require(IsWindowVisible(window), "The standalone conversion mode disabled the independent tools gesture");
        enter(target.object, arbitraryData.object, compressSector, DROPEFFECT_COPY, DROPEFFECT_COPY);
        drop(target.object, arbitraryData.object, compressSector, DROPEFFECT_COPY, DROPEFFECT_COPY);
        QCoreApplication::processEvents();
        require(toolsCount == 3 && toolsAction == QStringLiteral("compress") && gesture.ownsHotkeyForTest(),
                "A tools gesture interfered with the standalone conversion hotkey");
        // A single supported image adds the third, lower-left tool sector.
        // Its drop merely requests the independent editor after OLE returns.
        const POINTL photoSector = screenPoint(window, 230 - 140 * std::sqrt(3.0) / 2, 300);
        gesture.showToolsForTest(QPoint(600, 400));
        enter(target.object, data.object, photoSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        drop(target.object, data.object, photoSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
        require(toolsCount == 3, "The photo editor opened before the OLE source finished");
        QCoreApplication::processEvents();
        require(toolsCount == 4 && toolsAction == QStringLiteral("photo")
                    && toolsPaths == QStringList{QFileInfo(source).canonicalFilePath()}
                    && !converter.busy() && converter.outputs() == conversionOutputs
                    && digest(source) == original,
                "The single-photo tools sector did not request an editor without implicit export");
        panel.requireUnchanged(converter, "The photo tool changed the main conversion wheel");
        require(gesture.setShortcut(Qt::ShiftModifier, 0)
                    && gesture.requiresDragForTest() && !gesture.ownsHotkeyForTest()
                    && !gesture.dispatchHotkeyForTest(),
                "Restoring drag mode left the standalone hotkey captured");

        // Exercise real native placement, painting and OLE hit testing at both
        // appearance extremes. Reading setters alone would miss a wheel that
        // resized visually while retaining its old physical drop coordinates.
        int smallWidth = 0;
        for (const int size : {70, 130}) {
            int opaqueSurfaceAlpha = 0;
            for (const int transparency : {0, 80}) {
                gesture.setWheelSize(size);
                gesture.setWheelTransparency(transparency);
                gesture.showForTest(QPoint(600, 400));
                const QRect bounds = nativeBounds(window);
                if (size == 70) smallWidth = bounds.width();
                else require(bounds.width() > smallWidth, "The larger wheel did not enlarge its native target");
                MONITORINFO monitor {};
                monitor.cbSize = sizeof(monitor);
                require(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor),
                        "Cannot read the owned wheel's work area");
                const QRect workArea(monitor.rcWork.left, monitor.rcWork.top,
                                     monitor.rcWork.right - monitor.rcWork.left,
                                     monitor.rcWork.bottom - monitor.rcWork.top);
                require(workArea.contains(bounds), "A resized native wheel escaped the monitor work area");
                const POINTL resizedCenter = screenPoint(window, 230, 230);
                const POINTL resizedSector = pngSector(window, nativeFormats);
                enter(target.object, data.object, resizedCenter, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
                const auto painted = renderedWheel(window);
                const int surfaceAlpha = scaledAlpha(painted, 230, 55);
                if (transparency == 0) {
                    opaqueSurfaceAlpha = surfaceAlpha;
                    require(surfaceAlpha == 255, "Zero transparency left the native petal translucent");
                } else {
                    require(surfaceAlpha > 1 && surfaceAlpha < opaqueSurfaceAlpha,
                            "The native petal did not become translucent at maximum transparency");
                }
                require(scaledAlpha(painted, 210, 195) == 255 && painted.pixelColor(0, 0).alpha() == 1,
                        "Transparency faded the fruit or removed the native target's transparent hit backing");
                over(target.object, resizedSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
                over(target.object, resizedSector, DROPEFFECT_MOVE, DROPEFFECT_NONE);
                gesture.setWheelSize(size == 70 ? 130 : 70);
                require(nativeBounds(window) == bounds,
                        "Saving wheel size moved an active OLE drop target");
                over(target.object, resizedSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
                const auto previousOutputs = converter.outputs();
                drop(target.object, data.object, resizedSector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
                QCoreApplication::processEvents();
                finishConversion(converter);
                require(converter.outputs() != previousOutputs && QImage(converter.outputs().first()).size() == image.size()
                            && digest(source) == original,
                        "A resized or translucent native target lost its selected conversion or original");
                panel.requireUnchanged(converter, "Changing native appearance changed the panel selection");
                gesture.showForTest(QPoint(600, 400));
                require(size == 70 ? nativeBounds(window).width() > bounds.width()
                                   : nativeBounds(window).width() < bounds.width(),
                        "A deferred native size did not apply on the next gesture");
                require(target.object->DragLeave() == S_OK, "Cannot dismiss the resized native probe");

                gesture.setWheelSize(size);
                gesture.showToolsForTest(QPoint(600, 400));
                const auto resizedPhoto = screenPoint(window, 230 - 140 * std::sqrt(3.0) / 2, 300);
                const int previousToolsCount = toolsCount;
                const auto beforeToolOutputs = converter.outputs();
                enter(target.object, data.object, resizedPhoto, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
                drop(target.object, data.object, resizedPhoto, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY);
                require(toolsCount == previousToolsCount, "A resized tools target opened before OLE returned");
                QCoreApplication::processEvents();
                require(toolsCount == previousToolsCount + 1 && toolsAction == QStringLiteral("photo")
                            && toolsPaths == QStringList{QFileInfo(source).canonicalFilePath()}
                            && !converter.busy() && converter.outputs() == beforeToolOutputs,
                        "A resized or translucent tools target selected the wrong action or implicitly exported");
            }
        }
        gesture.setWheelSize(100);
        gesture.setWheelTransparency(35);

        gesture.stop();
        enter(target.object, data.object, sector, DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_NONE);
        require(!gesture.enabled(), "Stopping the adapter left it enabled");
        std::puts("{\"test\":\"windows-ole-in-process\",\"copy_with_shift\":true,\"move_only_rejected\":true,"
                  "\"center_cancels\":true,\"mixed_invalid_rejected\":true,\"hidden_target_rejected\":true,"
                  "\"configurable_trigger_matcher\":true,\"live_configuration_invalidates_candidate\":true,"
                  "\"prepare_without_activation\":true,"
                  "\"standalone_hotkey_dispatch\":true,\"hotkey_conflict_preserves_previous\":true,"
                  "\"suspended_capture_edit_and_resume\":true,"
                  "\"switching_back_releases_hotkey\":true,"
                  "\"independent_tools_alt_gesture\":true,\"tools_regular_files_copy_only\":true,"
                  "\"tools_open_after_drop_without_conversion\":true,\"tools_work_with_direct_conversion\":true,"
                  "\"photo_tool_opens_after_copy_drop_without_export\":true,"
                  "\"tools_pair_swap_atomic\":true,\"external_busy_suspends_probes_and_queued_drops\":true,"
                  "\"native_formats_independent_from_panel\":true,\"native_enter_leave_preserve_panel_preview\":true,"
                  "\"native_drop_preserves_panel_files_format_and_wheel\":true,"
                  "\"native_appearance_rendered_alpha\":true,\"native_size_scaled_copy_drop\":true,"
                  "\"native_size_deferred_during_drop\":true,\"native_tools_appearance_copy_drop\":true,"
                  "\"original_sha256_unchanged\":true,\"global_input_polled\":false,\"input_injected\":false}");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Windows OLE fixture check failed: %s\n", error.what());
        return 1;
    }
}

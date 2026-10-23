#include "windowsgesture.h"
#include "converter.h"
#include "photoengine.h"
#include "wheelappearance.h"

#ifndef Q_OS_WIN
#error "windowsgesture.cpp must only be built for Windows"
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
#include <QAbstractNativeEventFilter>
#include <QCursor>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRadialGradient>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

namespace {

constexpr int WheelExtent = 460; // Device-independent drawing coordinates.
constexpr double Center = WheelExtent / 2.0;
constexpr double OuterRadius = 198.0;
constexpr double InnerRadius = 82.0;
constexpr double Pi = 3.14159265358979323846;
constexpr UINT MaxFiles = 128;
constexpr SIZE_T MaxDropBytes = 1024 * 1024;
constexpr Qt::KeyboardModifiers SupportedModifiers = Qt::ShiftModifier | Qt::ControlModifier
    | Qt::AltModifier | Qt::MetaModifier;

int nativeKey(int key)
{
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
        return key; // Windows virtual keys use the same unshifted A-Z/0-9 codes.
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) return VK_F1 + key - Qt::Key_F1;
    return 0;
}

bool validShortcut(Qt::KeyboardModifiers modifiers, int key)
{
    return !(modifiers & ~SupportedModifiers) && (key ? nativeKey(key) != 0 : modifiers != Qt::NoModifier);
}

bool validStandalone(Qt::KeyboardModifiers modifiers, int key)
{
    return validShortcut(modifiers, key) && key
        && ((key >= Qt::Key_F1 && key <= Qt::Key_F12)
            || bool(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)));
}

UINT nativeModifiers(Qt::KeyboardModifiers modifiers)
{
    UINT native = MOD_NOREPEAT;
    if (modifiers.testFlag(Qt::ShiftModifier)) native |= MOD_SHIFT;
    if (modifiers.testFlag(Qt::ControlModifier)) native |= MOD_CONTROL;
    if (modifiers.testFlag(Qt::AltModifier)) native |= MOD_ALT;
    if (modifiers.testFlag(Qt::MetaModifier)) native |= MOD_WIN;
    return native;
}

int nextHotkeyId()
{
    // IDs belong to this GUI thread. Allocate fresh pairs for test adapters too;
    // two slots allow reserving a replacement before releasing the current key.
    static int next = 0x4d00;
    const int id = next;
    next += 2;
    return id;
}

QString shortcutLabel(Qt::KeyboardModifiers modifiers, int key)
{
    QStringList parts;
    if (modifiers.testFlag(Qt::ControlModifier)) parts.append(QStringLiteral("Ctrl"));
    if (modifiers.testFlag(Qt::AltModifier)) parts.append(QStringLiteral("Alt"));
    if (modifiers.testFlag(Qt::ShiftModifier)) parts.append(QStringLiteral("Shift"));
    if (modifiers.testFlag(Qt::MetaModifier)) parts.append(QStringLiteral("Win"));
    if (key) parts.append(QKeySequence(QKeyCombination(Qt::NoModifier, Qt::Key(key))).toString(QKeySequence::NativeText));
    return parts.join(QLatin1Char('+'));
}

bool keyDown(int key)
{
    // This adapter only passes the fixed modifier/button set or the configured
    // trigger key. It never scans other keys. Ignore the shared low state bit.
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

QPointF pointOnCircle(double radius, double angle)
{
    return {radius * std::cos(angle), radius * std::sin(angle)};
}

QPainterPath petalPath(double start, double end)
{
    const double outerCorner = 8.0 / OuterRadius;
    const double innerCorner = 8.0 / InnerRadius;
    const auto degrees = [](double radians) { return radians * 180.0 / Pi; };
    QPainterPath path;
    path.moveTo(pointOnCircle(InnerRadius + 8, start));
    path.lineTo(pointOnCircle(OuterRadius - 8, start));
    path.quadTo(pointOnCircle(OuterRadius, start), pointOnCircle(OuterRadius, start + outerCorner));
    path.arcTo(QRectF(-OuterRadius, -OuterRadius, OuterRadius * 2, OuterRadius * 2),
               -degrees(start + outerCorner), -degrees(end - start - outerCorner * 2));
    path.quadTo(pointOnCircle(OuterRadius, end), pointOnCircle(OuterRadius - 8, end));
    path.lineTo(pointOnCircle(InnerRadius + 8, end));
    path.quadTo(pointOnCircle(InnerRadius, end), pointOnCircle(InnerRadius, end - innerCorner));
    path.arcTo(QRectF(-InnerRadius, -InnerRadius, InnerRadius * 2, InnerRadius * 2),
               -degrees(end - innerCorner), degrees(end - start - innerCorner * 2));
    path.quadTo(pointOnCircle(InnerRadius, start), pointOnCircle(InnerRadius + 8, start));
    path.closeSubpath();
    return path;
}

double easeOut(double progress)
{
    const double inverse = 1.0 - std::clamp(progress, 0.0, 1.0);
    return 1.0 - inverse * inverse * inverse;
}

// Querying CF_HDROP here reads the OLE drag object, not the system clipboard.
// The source's memory is copied into QStrings and released before returning.
bool localFiles(IDataObject *object, QVariantList &urls, bool tools)
{
    if (!object) return false;
    FORMATETC format {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (object->QueryGetData(&format) != S_OK) return false;
    STGMEDIUM medium {};
    if (FAILED(object->GetData(&format, &medium))) return false;
    struct Release final {
        STGMEDIUM *medium;
        ~Release() { ReleaseStgMedium(medium); }
    } release {&medium};
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return false;
    const SIZE_T bytes = GlobalSize(medium.hGlobal);
    if (bytes < sizeof(DROPFILES) || bytes > MaxDropBytes) return false;
    const auto *memory = static_cast<const unsigned char *>(GlobalLock(medium.hGlobal));
    if (!memory) return false;
    struct Unlock final {
        HGLOBAL memory;
        ~Unlock() { GlobalUnlock(memory); }
    } unlock {medium.hGlobal};
    DROPFILES description {};
    std::memcpy(&description, memory, sizeof(description));
    if (description.pFiles < sizeof(DROPFILES) || description.pFiles >= bytes) return false;
    bool terminated = false;
    if (description.fWide) {
        if (description.pFiles % alignof(wchar_t)) return false;
        const auto *characters = reinterpret_cast<const wchar_t *>(memory + description.pFiles);
        const SIZE_T capacity = (bytes - description.pFiles) / sizeof(wchar_t);
        for (SIZE_T index = 0; index + 1 < capacity; ++index) {
            if (!characters[index] && !characters[index + 1]) { terminated = true; break; }
        }
    } else {
        const auto *characters = memory + description.pFiles;
        const SIZE_T capacity = bytes - description.pFiles;
        for (SIZE_T index = 0; index + 1 < capacity; ++index) {
            if (!characters[index] && !characters[index + 1]) { terminated = true; break; }
        }
    }
    if (!terminated) return false;
    const HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
    if (count < 1 || count > MaxFiles) return false;
    QVariantList incoming;
    QString kind;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length < 1 || length > 32767) return false;
        std::wstring buffer(size_t(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, buffer.data(), length + 1) != length) return false;
        const QFileInfo file(QString::fromWCharArray(buffer.data(), int(length)));
        if (!file.isAbsolute() || !file.isFile() || !file.isReadable()) return false;
        const auto path = file.canonicalFilePath();
        if (path.isEmpty()) return false;
        if (!tools) {
            const auto current = Mandarina::sourceKind(path);
            if (current == QStringLiteral("unsupported") || (!kind.isEmpty() && kind != current)) return false;
            kind = current;
        }
        const QVariant url = QUrl::fromLocalFile(path);
        if (!incoming.contains(url)) incoming.append(url);
    }
    if (incoming.isEmpty()) return false;
    urls = incoming;
    return true;
}

UINT windowDpi(HWND window)
{
    // Resolve dynamically to keep the adapter buildable against different SDKs.
    using GetDpiForWindowFunction = UINT (WINAPI *)(HWND);
    static const auto query = reinterpret_cast<GetDpiForWindowFunction>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (query) {
        const UINT dpi = query(window);
        if (dpi) return dpi;
    }
    const auto *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    return screen ? UINT(std::lround(screen->devicePixelRatio() * 96.0)) : 96u;
}

} // namespace

struct WindowsGesture::Private : QAbstractNativeEventFilter {
    struct Wheel;
    struct Target;

    explicit Private(WindowsGesture *owner, Converter *controller);
    ~Private();
    bool initialize();
    void shutdown(bool notify);
    void enable(bool value);
    bool configureActivation(Qt::KeyboardModifiers modifiers, int key, bool requireDrag);
    bool configureTools(Qt::KeyboardModifiers modifiers, int key);
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    bool registerHotkey(Qt::KeyboardModifiers modifiers, int key, int &registeredId);
    void releaseHotkey();
    QString activeStatus() const;
    bool triggerDown(Qt::KeyboardModifiers expected, int native) const;
    bool triggerMatches(Qt::KeyboardModifiers modifiers, int key) const;
    void poll();
    void showCandidate(POINT cursor, bool tools = false);
    void hideWheel();
    bool preview(const QVariantList &incoming);
    void refreshPendingFormats();
    void updateHover(POINTL cursor);
    int sectorAt(QPointF position) const;
    void animate();
    void paint(QPainter &painter, QSizeF size);
    DWORD effect(DWORD allowed) const;
    HRESULT enter(IDataObject *object, POINTL cursor, DWORD *allowed);
    HRESULT over(POINTL cursor, DWORD *allowed);
    HRESULT drop(IDataObject *object, POINTL cursor, DWORD *allowed);

    WindowsGesture *q;
    QPointer<Converter> converter;
    std::unique_ptr<Wheel> wheel;
    Target *target = nullptr;
    HWND window = nullptr;
    QTimer polling;
    QTimer candidateTimeout;
    QTimer releaseTimeout;
    QTimer animation;
    QElapsedTimer openingClock;
    QElapsedTimer hoverClock;
    bool oleInitialized = false;
    bool registered = false;
    bool available = false;
    bool enabled = false;
    bool externalBusy = false;
    bool inputPollingAllowed = true;
    bool requireDrag = true;
    bool nativeFilterInstalled = false;
    int hotkeyId = 0;
    const int hotkeyBase = nextHotkeyId();
    bool lastLeft = false;
    bool lastTrigger = false;
    bool lastToolsTrigger = false;
    Qt::KeyboardModifiers shortcutModifiers = Qt::ShiftModifier;
    int shortcutKey = 0;
    int shortcutNativeKey = 0;
    Qt::KeyboardModifiers toolsModifiers = Qt::AltModifier;
    int toolsKey = 0;
    int toolsNativeKey = 0;
    bool toolsMode = false;
    bool haveOrigin = false;
    bool moved = false;
    bool attemptedThisPress = false;
    bool validDrag = false;
    POINT origin {};
    QString status;
    QString error;
    QVariantList urls;
    QVariantList formats;
    QVariantList pendingUrls;
    POINTL pendingCursor {};
    int hovered = -1;
    int wheelTransparency = 35;
    int wheelSize = 100;
    WheelAppearance::Rgb wheelColor = WheelAppearance::DefaultColor;
};

struct WindowsGesture::Private::Wheel final : QWidget {
    explicit Wheel(Private *implementation)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                           | Qt::WindowDoesNotAcceptFocus), d(implementation)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_NativeWindow);
        setFocusPolicy(Qt::NoFocus);
        setAcceptDrops(false); // No Qt event handler is allowed to accept drops.
        setWindowTitle(QStringLiteral("MaracujaShiftWheel"));
        resize(WheelExtent, WheelExtent);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        d->paint(painter, size());
    }

    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override
    {
        const auto *event = static_cast<const MSG *>(message);
        if (event && event->message == WM_MOUSEACTIVATE) {
            *result = MA_NOACTIVATE;
            return true;
        }
        return QWidget::nativeEvent(type, message, result);
    }

    Private *d;
};

struct WindowsGesture::Private::Target final : IDropTarget {
    explicit Target(Private *implementation) : d(implementation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDropTarget)) return E_NOINTERFACE;
        *object = static_cast<IDropTarget *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&references)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG remaining = InterlockedDecrement(&references);
        if (!remaining) delete this;
        return ULONG(remaining);
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *object, DWORD, POINTL cursor, DWORD *effect) override
    {
        if (!effect) return E_POINTER;
        if (!d) { *effect = DROPEFFECT_NONE; return S_OK; }
        return d->enter(object, cursor, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL cursor, DWORD *effect) override
    {
        if (!effect) return E_POINTER;
        if (!d) { *effect = DROPEFFECT_NONE; return S_OK; }
        return d->over(cursor, effect);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        if (d) d->hideWheel();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *object, DWORD, POINTL cursor, DWORD *effect) override
    {
        if (!effect) return E_POINTER;
        if (!d) { *effect = DROPEFFECT_NONE; return S_OK; }
        return d->drop(object, cursor, effect);
    }

    void detach() { d = nullptr; }
    Private *d;
    volatile LONG references = 1;
};

WindowsGesture::Private::Private(WindowsGesture *owner, Converter *controller)
    : q(owner), converter(controller), polling(owner), candidateTimeout(owner),
      releaseTimeout(owner), animation(owner)
{
    polling.setInterval(75);
    polling.setTimerType(Qt::CoarseTimer);
    candidateTimeout.setSingleShot(true);
    candidateTimeout.setInterval(450);
    releaseTimeout.setSingleShot(true);
    releaseTimeout.setInterval(150);
    animation.setInterval(16);
    QObject::connect(&polling, &QTimer::timeout, q, [this] { poll(); });
    QObject::connect(&candidateTimeout, &QTimer::timeout, q, [this] {
        if (!validDrag) hideWheel();
    });
    QObject::connect(&releaseTimeout, &QTimer::timeout, q, [this] {
        if (!keyDown(VK_LBUTTON)) hideWheel();
    });
    QObject::connect(&animation, &QTimer::timeout, q, [this] { animate(); });
    if (converter) QObject::connect(converter, &Converter::busyChanged, q, [this] {
        if (converter && converter->busy()) hideWheel();
    });
    if (converter) QObject::connect(converter, &Converter::formatsChanged, q, [this] {
        refreshPendingFormats();
    });
}

WindowsGesture::Private::~Private() { shutdown(false); }

bool WindowsGesture::Private::initialize()
{
    if (registered) return true;
    if (!converter || !qApp || QThread::currentThread() != qApp->thread()) {
        status = WindowsGesture::tr("Não foi possível iniciar a roda de arquivos nesta sessão.");
        error = status;
        emit q->changed();
        return false;
    }
    if (!nativeFilterInstalled) {
        qApp->installNativeEventFilter(this);
        nativeFilterInstalled = true;
    }
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) {
        status = WindowsGesture::tr("A integração de arraste do Windows não pôde ser iniciada.");
        error = status;
        emit q->changed();
        return false;
    }
    oleInitialized = true; // Both S_OK and S_FALSE require our OleUninitialize.
    wheel = std::make_unique<Wheel>(this);
    window = reinterpret_cast<HWND>(wheel->winId());
    LONG_PTR styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    styles |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    styles &= ~LONG_PTR(WS_EX_APPWINDOW);
    SetWindowLongPtrW(window, GWL_EXSTYLE, styles);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // Qt's Windows QPA registers an OLE target for all top-level Tool windows,
    // even when QWidget::acceptDrops is false. Replace that registration only on
    // this dedicated HWND; Qt retains and later destroys its own unused object.
    // Our registration is revoked before QWidget destroys the native window.
    RevokeDragDrop(window);
    target = new Target(this);
    const HRESULT registration = RegisterDragDrop(window, target);
    if (FAILED(registration)) {
        target->detach();
        target->Release();
        target = nullptr;
        wheel.reset();
        window = nullptr;
        OleUninitialize();
        oleInitialized = false;
        status = WindowsGesture::tr("Não foi possível preparar a roda para receber arquivos.");
        error = status;
        emit q->changed();
        return false;
    }
    registered = available = true;
    return true;
}

void WindowsGesture::Private::shutdown(bool notify)
{
    polling.stop();
    releaseHotkey();
    if (nativeFilterInstalled && qApp) qApp->removeNativeEventFilter(this);
    nativeFilterInstalled = false;
    enabled = false;
    hideWheel();
    if (target) target->detach();
    if (registered && window) RevokeDragDrop(window);
    registered = false;
    if (target) target->Release();
    target = nullptr;
    wheel.reset();
    window = nullptr;
    if (oleInitialized) OleUninitialize();
    oleInitialized = false;
    available = false;
    status = WindowsGesture::tr("Roda com %1 encerrada nesta sessão.")
        .arg(shortcutLabel(shortcutModifiers, shortcutKey));
    if (notify) emit q->changed();
}

void WindowsGesture::Private::enable(bool value)
{
    if (value && !initialize()) return;
    if (enabled == value) return;
    if (value && !requireDrag) {
        int id = 0;
        if (!registerHotkey(shortcutModifiers, shortcutKey, id)) {
            status = error;
            emit q->changed();
            return;
        }
        hotkeyId = id;
    }
    if (value) error.clear();
    enabled = value;
    hideWheel();
    haveOrigin = moved = attemptedThisPress = false;
    lastLeft = inputPollingAllowed && keyDown(VK_LBUTTON);
    lastTrigger = lastLeft && requireDrag && triggerDown(shortcutModifiers, shortcutNativeKey);
    lastToolsTrigger = lastLeft && triggerDown(toolsModifiers, toolsNativeKey);
    if (enabled) {
        if (inputPollingAllowed) polling.start();
        status = activeStatus();
    } else {
        polling.stop();
        releaseHotkey();
        status = WindowsGesture::tr("Roda com %1 desativada nesta sessão.")
            .arg(shortcutLabel(shortcutModifiers, shortcutKey));
    }
    emit q->changed();
}

QString WindowsGesture::Private::activeStatus() const
{
    return (requireDrag
        ? WindowsGesture::tr("Arraste um arquivo, pressione %1 e solte em um formato.")
        : WindowsGesture::tr("Pressione %1 para abrir o Maracujá."))
        .arg(shortcutLabel(shortcutModifiers, shortcutKey))
        + WindowsGesture::tr(" Ferramentas: %1 + arraste.").arg(shortcutLabel(toolsModifiers, toolsKey));
}

bool WindowsGesture::Private::registerHotkey(Qt::KeyboardModifiers modifiers, int key, int &registeredId)
{
    if (!qApp || QThread::currentThread() != qApp->thread()) {
        error = WindowsGesture::tr("O atalho precisa ser configurado pela janela do Maracujá.");
        return false;
    }
    const int id = hotkeyId == hotkeyBase ? hotkeyBase + 1 : hotkeyBase;
    if (!RegisterHotKey(nullptr, id, nativeModifiers(modifiers), UINT(nativeKey(key)))) {
        error = WindowsGesture::tr("Não foi possível reservar %1. Esse atalho pode estar em uso pelo Windows ou por outro aplicativo.")
            .arg(shortcutLabel(modifiers, key));
        return false;
    }
    registeredId = id;
    return true;
}

void WindowsGesture::Private::releaseHotkey()
{
    if (hotkeyId) UnregisterHotKey(nullptr, hotkeyId);
    hotkeyId = 0;
}

bool WindowsGesture::Private::configureActivation(Qt::KeyboardModifiers modifiers, int key, bool drag)
{
    if (!drag && key == Qt::Key_F12) {
        error = WindowsGesture::tr("O Windows reserva F12 para o depurador. Escolha outra tecla para o atalho sem arraste.");
        return false;
    }
    if (!validShortcut(modifiers, key) || (!drag && !validStandalone(modifiers, key))) {
        error = WindowsGesture::tr("Escolha modificadores com uma letra ou número, ou uma tecla F1 a F12. No modo sem arraste, letras e números precisam de Ctrl, Alt ou Win.");
        return false;
    }
    if (modifiers == toolsModifiers && key == toolsKey) {
        error = WindowsGesture::tr("Escolha atalhos diferentes para converter e abrir ferramentas durante o arraste.");
        return false;
    }
    error.clear();
    if (shortcutModifiers == modifiers && shortcutKey == key && requireDrag == drag) return true;

    // Probe complete drag combinations too, but never keep a drag-only hotkey
    // registered: ordinary typing must continue reaching the focused program.
    // A standalone replacement stays registered while the previous one is still
    // owned, so a failure cannot silently disable the user's working shortcut.
    int replacement = 0;
    const bool sameOwnedKey = hotkeyId && shortcutModifiers == modifiers && shortcutKey == key;
    // F12 is reserved to debuggers by Windows and must never be registered. It
    // can still be observed as the user-selected key during a real file drag.
    if (key && key != Qt::Key_F12 && !sameOwnedKey && !registerHotkey(modifiers, key, replacement)) return false;
    if (replacement && (drag || !enabled)) {
        UnregisterHotKey(nullptr, replacement);
        replacement = 0;
    }
    hideWheel();
    releaseHotkey();
    hotkeyId = replacement;
    shortcutModifiers = modifiers;
    shortcutKey = key;
    shortcutNativeKey = nativeKey(key);
    requireDrag = drag;
    // A configuration edit cannot authorize a candidate from an earlier drag.
    // Do not read global input here: the test path also edits the shortcut.
    haveOrigin = moved = attemptedThisPress = lastLeft = lastTrigger = lastToolsTrigger = false;
    polling.stop();
    if (enabled && inputPollingAllowed) polling.start();
    status = enabled
        ? activeStatus()
        : WindowsGesture::tr("Roda com %1 desativada nesta sessão.")
              .arg(shortcutLabel(shortcutModifiers, shortcutKey));
    emit q->changed();
    return true;
}

bool WindowsGesture::Private::configureTools(Qt::KeyboardModifiers modifiers, int key)
{
    if (!validShortcut(modifiers, key)) {
        error = WindowsGesture::tr("Escolha modificadores ou uma combinação com letra, número ou F1 a F12 para as ferramentas.");
        return false;
    }
    if (modifiers == shortcutModifiers && key == shortcutKey) {
        error = WindowsGesture::tr("Escolha atalhos diferentes para converter e abrir ferramentas.");
        return false;
    }
    error.clear();
    if (toolsModifiers == modifiers && toolsKey == key) return true;
    // A drag gesture is only observed during a left-button drag. Probe a full
    // combination for conflicts without reserving it after this call returns.
    int probe = 0;
    if (key && key != Qt::Key_F12 && !registerHotkey(modifiers, key, probe)) return false;
    if (probe) UnregisterHotKey(nullptr, probe);
    hideWheel();
    toolsModifiers = modifiers;
    toolsKey = key;
    toolsNativeKey = nativeKey(key);
    haveOrigin = moved = attemptedThisPress = lastLeft = lastTrigger = lastToolsTrigger = false;
    if (enabled) status = activeStatus();
    emit q->changed();
    return true;
}

bool WindowsGesture::Private::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType != QByteArrayLiteral("windows_generic_MSG")
        && eventType != QByteArrayLiteral("windows_dispatcher_MSG")) return false;
    const auto *event = static_cast<const MSG *>(message);
    if (!event || event->message != WM_HOTKEY || !hotkeyId || int(event->wParam) != hotkeyId) return false;
    if (result) *result = 0;
    if (enabled && !requireDrag) emit q->activationRequested();
    return true;
}

bool WindowsGesture::Private::triggerMatches(Qt::KeyboardModifiers modifiers, int key) const
{
    return modifiers == shortcutModifiers && (!shortcutKey || key == shortcutKey);
}

bool WindowsGesture::Private::triggerDown(Qt::KeyboardModifiers expected, int native) const
{
    Qt::KeyboardModifiers modifiers;
    if (keyDown(VK_SHIFT)) modifiers |= Qt::ShiftModifier;
    if (keyDown(VK_CONTROL)) modifiers |= Qt::ControlModifier;
    if (keyDown(VK_MENU)) modifiers |= Qt::AltModifier;
    if (keyDown(VK_LWIN) || keyDown(VK_RWIN)) modifiers |= Qt::MetaModifier;
    if (modifiers != expected) return false;
    return !native || keyDown(native);
}

void WindowsGesture::Private::poll()
{
    if (!inputPollingAllowed || !enabled || !converter || externalBusy) return;
    const bool left = keyDown(VK_LBUTTON);
    // Key state is relevant only during a left-button drag. A normal desktop
    // typing session is never sampled for trigger keys by this adapter.
    const bool trigger = left && requireDrag && triggerDown(shortcutModifiers, shortcutNativeKey);
    const bool toolsTrigger = left && triggerDown(toolsModifiers, toolsNativeKey);
    POINT cursor {};
    if (!GetCursorPos(&cursor)) return;
    if (!left) {
        haveOrigin = moved = attemptedThisPress = false;
        if (wheel && wheel->isVisible()) {
            if (!validDrag) hideWheel();
            else if (lastLeft) releaseTimeout.start(); // Let the OLE Drop run first.
        }
    } else {
        releaseTimeout.stop();
        if (!lastLeft) {
            origin = cursor;
            haveOrigin = true;
            moved = attemptedThisPress = false;
        }
        if (haveOrigin) {
            const qint64 dx = qint64(cursor.x) - origin.x;
            const qint64 dy = qint64(cursor.y) - origin.y;
            moved = moved || dx * dx + dy * dy >= 8 * 8;
        }
        const bool toolsEdge = toolsTrigger && !lastToolsTrigger;
        const bool conversionEdge = trigger && !lastTrigger;
        if (haveOrigin && moved && (toolsEdge || conversionEdge) && !attemptedThisPress
            && !converter->busy() && !externalBusy && wheel && !wheel->isVisible()) {
            attemptedThisPress = true;
            showCandidate(cursor, toolsEdge);
        }
    }
    lastLeft = left;
    lastTrigger = trigger;
    lastToolsTrigger = toolsTrigger;
}

void WindowsGesture::Private::showCandidate(POINT cursor, bool tools)
{
    if (!window || !wheel || !enabled || (!requireDrag && !tools) || !converter || converter->busy() || externalBusy) return;
    hideWheel();
    toolsMode = tools;
    // Qt's Windows application is per-monitor DPI aware. Native coordinates are
    // kept native throughout placement (including negative monitor origins).
    // Put the hidden HWND wholly inside the cursor's monitor before asking for
    // its DPI. Merely placing its top-left at the cursor could select the next
    // monitor when the cursor is near a boundary with a different scale.
    MONITORINFO monitor {};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT area = monitor.rcWork;
    const int logicalExtent = WheelAppearance::sizeExtent(wheelSize);
    const int probeExtent = std::max(1, std::min({logicalExtent,
        int(area.right - area.left), int(area.bottom - area.top)}));
    SetWindowPos(window, HWND_TOPMOST, int(area.left), int(area.top), probeExtent, probeExtent,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const UINT dpi = windowDpi(window);
    const int normalExtent = MulDiv(logicalExtent, int(dpi), 96);
    const int extent = std::max(1, std::min({normalExtent,
        int(area.right - area.left), int(area.bottom - area.top)}));
    const int x = std::clamp(int(cursor.x) - extent / 2, int(area.left), int(area.right) - extent);
    const int y = std::clamp(int(cursor.y) - extent / 2, int(area.top), int(area.bottom) - extent);
    wheel->show();
    SetWindowPos(window, HWND_TOPMOST, x, y, extent, extent,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    wheel->update();
    candidateTimeout.start(450);
}

void WindowsGesture::Private::hideWheel()
{
    candidateTimeout.stop();
    releaseTimeout.stop();
    animation.stop();
    validDrag = false;
    hovered = -1;
    urls.clear();
    formats.clear();
    pendingUrls.clear();
    openingClock.invalidate();
    hoverClock.invalidate();
    if (wheel) wheel->hide();
}

bool WindowsGesture::Private::preview(const QVariantList &incoming)
{
    if (!enabled || (!requireDrag && !toolsMode) || !wheel || !wheel->isVisible() || !converter || converter->busy() || externalBusy)
        return false;
    QVariantList offered;
    if (toolsMode) {
        // enter/drop already validate every item through the bounded CF_HDROP
        // reader. File tools can inspect or archive formats the converter does
        // not recognize; selecting a tool only opens its settings panel.
        offered = {
            QVariantMap {{QStringLiteral("id"), QStringLiteral("tool:compress")},
                         {QStringLiteral("label"), WindowsGesture::tr("Comprimir")},
                         {QStringLiteral("detail"), WindowsGesture::tr("Reduzir tamanho")}},
            QVariantMap {{QStringLiteral("id"), QStringLiteral("tool:metadata")},
                         {QStringLiteral("label"), WindowsGesture::tr("Metadados")},
                         {QStringLiteral("detail"), WindowsGesture::tr("Ver ou editar")}}
        };
        if (incoming.size() == 1 && Mandarina::Photo::supportsPath(incoming.first().toUrl().toLocalFile()))
            offered << QVariantMap {{QStringLiteral("id"), QStringLiteral("tool:photo")},
                                    {QStringLiteral("label"), WindowsGesture::tr("Editar foto")},
                                    {QStringLiteral("detail"), WindowsGesture::tr("Luz, cor e detalhes")}};
    } else {
        // The desktop wheel owns its preview. Asking which formats these files
        // support must not replace the panel's files, wheel or selection.
        offered = converter->formatsForUrls(incoming);
    }
    if (offered.isEmpty() || offered.size() > 16) return false;
    urls = incoming;
    formats = offered;
    pendingUrls.clear();
    validDrag = true;
    candidateTimeout.stop();
    releaseTimeout.stop();
    openingClock.start();
    hoverClock.invalidate();
    if (wheel) wheel->update();
    animation.start();
    return true;
}

void WindowsGesture::Private::refreshPendingFormats()
{
    if (pendingUrls.isEmpty() || validDrag) return;
    // Only a validated OLE drag can wait for background discovery. Keep a
    // completed wheel's sectors fixed until it closes, even if OCR/audio
    // discovery finishes while the pointer is over an existing sector.
    const auto incoming = pendingUrls;
    if (preview(incoming)) updateHover(pendingCursor);
    else if (!converter || !converter->formatsLoading()) hideWheel();
}

int WindowsGesture::Private::sectorAt(QPointF position) const
{
    if (!validDrag || formats.isEmpty()) return -1;
    const QPointF offset = position - QPointF(Center, Center);
    const double distance = std::hypot(offset.x(), offset.y());
    if (distance < InnerRadius || distance > OuterRadius) return -1;
    const double step = Pi * 2 / double(formats.size());
    const double angle = std::fmod(std::atan2(offset.y(), offset.x()) + Pi / 2 + step / 2 + Pi * 2, Pi * 2);
    return int(std::floor(angle / step)) % int(formats.size());
}

void WindowsGesture::Private::updateHover(POINTL cursor)
{
    POINT client {cursor.x, cursor.y};
    RECT bounds {};
    if (!window || !ScreenToClient(window, &client) || !GetClientRect(window, &bounds)
        || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    // OLE supplies physical screen pixels. Convert through the actual client
    // rectangle, not a global DPR, so mixed-DPI/negative-coordinate screens work.
    const QPointF local(double(client.x) * WheelExtent / double(bounds.right - bounds.left),
                        double(client.y) * WheelExtent / double(bounds.bottom - bounds.top));
    const int sector = sectorAt(local);
    if (sector != hovered) {
        hovered = sector;
        hoverClock.start();
        animation.start();
        if (wheel) wheel->update();
        if (sector >= 0 && validDrag && enabled && converter && !converter->busy() && !externalBusy)
            emit q->wheelSectorHovered();
    }
}

DWORD WindowsGesture::Private::effect(DWORD allowed) const
{
    // Do not derive the action from shortcut modifiers. The destination mask is COPY
    // only; a source that offers just MOVE is rejected without touching files.
    return enabled && (requireDrag || toolsMode) && validDrag && converter && !converter->busy() && !externalBusy && hovered >= 0
        && (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

HRESULT WindowsGesture::Private::enter(IDataObject *object, POINTL cursor, DWORD *allowed)
{
    const DWORD sourceEffects = *allowed;
    *allowed = DROPEFFECT_NONE;
    // Reject stale OLE callbacks after a probe timeout/disable, and do not let a
    // previous drag's cached preview authorize a different incoming data object.
    validDrag = false;
    hovered = -1;
    urls.clear();
    formats.clear();
    pendingUrls.clear();
    animation.stop();
    if (!enabled || !wheel || !wheel->isVisible()) return S_OK;
    candidateTimeout.start(450);
    QVariantList incoming;
    if (!(sourceEffects & DROPEFFECT_COPY) || !localFiles(object, incoming, toolsMode)) return S_OK;
    if (!preview(incoming)) {
        if (!toolsMode && converter && !converter->busy() && !externalBusy && converter->formatsLoading()) {
            pendingUrls = incoming;
            pendingCursor = cursor;
            // The normal 450 ms probe timeout is too short for a cold tool
            // discovery. OLE leave/drop, disabling and busy changes still
            // cancel this pending target immediately; this is a final bound.
            candidateTimeout.start(25000);
        }
        return S_OK;
    }
    updateHover(cursor);
    *allowed = effect(sourceEffects);
    return S_OK;
}

HRESULT WindowsGesture::Private::over(POINTL cursor, DWORD *allowed)
{
    const DWORD sourceEffects = *allowed;
    pendingCursor = cursor;
    refreshPendingFormats();
    updateHover(cursor);
    *allowed = effect(sourceEffects);
    return S_OK;
}

HRESULT WindowsGesture::Private::drop(IDataObject *object, POINTL cursor, DWORD *allowed)
{
    const DWORD sourceEffects = *allowed;
    *allowed = DROPEFFECT_NONE;
    pendingCursor = cursor;
    refreshPendingFormats();
    updateHover(cursor);
    if (effect(sourceEffects) != DROPEFFECT_COPY || hovered >= formats.size()) {
        hideWheel();
        return S_OK;
    }
    QVariantList current;
    if (!localFiles(object, current, toolsMode) || current != urls) {
        hideWheel();
        return S_OK;
    }
    const auto chosen = formats.at(hovered).toMap().value(QStringLiteral("id")).toString();
    const auto selected = urls;
    const bool tools = toolsMode;
    hideWheel();
    if (chosen.isEmpty()) return S_OK;
    *allowed = DROPEFFECT_COPY;
    // Finish the source's OLE negotiation before starting conversion. The queued
    // callback is owned by this adapter and cannot outlive it or its converter.
    const QPointer<WindowsGesture> owner(q);
    const QPointer<Converter> controller(converter);
    QTimer::singleShot(0, q, [owner, controller, selected, chosen, tools] {
        if (!owner || !owner->enabled() || !controller || controller->busy() || owner->d->externalBusy) return;
        if (tools) {
            if (chosen != QStringLiteral("tool:compress") && chosen != QStringLiteral("tool:metadata")
                && chosen != QStringLiteral("tool:photo")) return;
            QStringList paths;
            for (const auto &url : selected) paths.append(url.toUrl().toLocalFile());
            if (chosen == QStringLiteral("tool:photo")
                && (paths.size() != 1 || !Mandarina::Photo::supportsPath(paths.first()))) return;
            emit owner->toolsRequested(paths, chosen.mid(5));
            return;
        }
        controller->convertUrls(selected, chosen);
        // Validation can still fail if a file disappears before this callback.
        // Only mark a gesture job after the controller has really started it;
        // its worker-completion notification is queued on this same GUI thread.
        if (controller->busy()) emit owner->conversionStarted();
    });
    return S_OK;
}

void WindowsGesture::Private::animate()
{
    if (!wheel || !validDrag) { animation.stop(); return; }
    wheel->update();
    const bool opening = openingClock.isValid() && openingClock.elapsed() < 420;
    const bool hovering = hoverClock.isValid() && hoverClock.elapsed() < 180;
    if (!opening && !hovering) animation.stop();
}

void WindowsGesture::Private::paint(QPainter &painter, QSizeF size)
{
    // An alpha-1 backing remains visually transparent while allowing Windows'
    // layered-window hit test to deliver the initial OLE DragEnter. Fully alpha-0
    // pixels would make the probe invisible to drag targeting as well.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(QRectF(QPointF(), size), QColor(0, 0, 0, 1));
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (!validDrag || formats.isEmpty()) return;
    painter.setRenderHint(QPainter::Antialiasing);
    const double drawingScale = std::min(size.width(), size.height()) / WheelExtent;
    painter.translate(size.width() / 2, size.height() / 2);
    painter.scale(drawingScale, drawingScale);
    const double opening = openingClock.isValid() ? easeOut(double(openingClock.elapsed()) / 420.0) : 1;
    const double hover = hoverClock.isValid() ? easeOut(double(hoverClock.elapsed()) / 180.0) : 1;
    painter.setOpacity(opening);
    painter.scale(0.92 + opening * 0.08, 0.92 + opening * 0.08);

    const auto tintColor = [this](int red, int green, int blue, int alpha = 255) {
        const auto tinted = WheelAppearance::tint(
            {double(red) / 255.0, double(green) / 255.0, double(blue) / 255.0}, wheelColor);
        return QColor::fromRgbF(tinted.red, tinted.green, tinted.blue, double(alpha) / 255.0);
    };
    const auto glassColor = [this, &tintColor](int red, int green, int blue, int referenceAlpha) {
        // Change each surface from opaque to faint while preserving the
        // existing gradient, chosen hue and stronger selection at the default
        // 35 percent. Tinting never changes the transparency preference.
        QColor color = tintColor(red, green, blue);
        color.setAlphaF(WheelAppearance::fillAlpha(double(referenceAlpha) / 255.0, wheelTransparency));
        return color;
    };
    const auto drawReadableText = [&painter, this](const QRectF &rect, int flags,
                                                const QString &text, const QColor &color) {
        painter.save();
        const double haloAlpha = std::max(WheelAppearance::labelHaloAlpha(wheelTransparency),
                                         WheelAppearance::colorLabelHalo(wheelColor));
        if (haloAlpha > 0) {
            // A narrow light halo keeps opaque letters readable on a dark or
            // busy desktop and with deep surface tints, without adding an
            // opaque plate behind the text.
            QColor halo(Qt::white);
            halo.setAlphaF(haloAlpha);
            painter.setPen(halo);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy) painter.drawText(rect.translated(dx * 0.9, dy * 0.9), flags, text);
        }
        painter.setPen(color);
        painter.drawText(rect, flags, text);
        painter.restore();
    };

    painter.setPen(Qt::NoPen);
    for (int ring = 12; ring >= 1; --ring) {
        QColor shadow = tintColor(65, 30, 85);
        shadow.setAlphaF(WheelAppearance::decorationAlpha(1.0 / 255.0, wheelTransparency));
        painter.setBrush(shadow);
        painter.drawEllipse(QRectF(-OuterRadius - ring, -OuterRadius - ring + 8,
                                  (OuterRadius + ring) * 2, (OuterRadius + ring) * 2));
    }
    QRadialGradient halo(QPointF(), OuterRadius + 14);
    halo.setColorAt(0.83, tintColor(235, 219, 246, 0));
    QColor haloTint = tintColor(200, 165, 224);
    haloTint.setAlphaF(WheelAppearance::decorationAlpha(70.0 / 255.0, wheelTransparency));
    halo.setColorAt(0.92, haloTint);
    halo.setColorAt(1, tintColor(235, 219, 246, 0));
    painter.setBrush(halo);
    painter.drawEllipse(QPointF(), OuterRadius + 14, OuterRadius + 14);

    const int count = int(formats.size());
    const bool dense = count > 8;
    const double step = Pi * 2 / count;
    for (int index = 0; index < count; ++index) {
        const double middle = -Pi / 2 + index * step;
        const double start = middle - step / 2 + 0.024;
        const double end = middle + step / 2 - 0.024;
        const bool active = index == hovered;
        painter.save();
        if (active) painter.translate(pointOnCircle(hover * 3, middle));
        // Tint the surface itself so the desktop remains visible while labels
        // and the fruit retain their normal opacity above the glass.
        QLinearGradient gradient(-OuterRadius * 0.7, -OuterRadius,
                                 OuterRadius * 0.6, OuterRadius);
        gradient.setColorAt(0, active ? glassColor(239, 218, 255, 191) : glassColor(255, 253, 248, 166));
        gradient.setColorAt(0.46, active ? glassColor(226, 202, 246, 189) : glassColor(239, 247, 255, 158));
        gradient.setColorAt(1, active ? glassColor(201, 173, 232, 198) : glassColor(225, 210, 244, 168));
        painter.setBrush(gradient);
        painter.setPen(QPen(active ? tintColor(126, 73, 167, 190) : tintColor(202, 182, 220, 135), active ? 1.5 : 1));
        painter.drawPath(petalPath(start, end));
        painter.setPen(QPen(active ? QColor("#f3c64d") : QColor(255, 255, 255, 170), active ? 3 : 1, Qt::SolidLine, Qt::RoundCap));
        const int startAngle = int(std::lround(-start * 180.0 / Pi * 16));
        const int span = int(std::lround(-(end - start) * 180.0 / Pi * 16));
        painter.drawArc(QRectF(-OuterRadius + 5, -OuterRadius + 5,
                              (OuterRadius - 5) * 2, (OuterRadius - 5) * 2), startAngle, span);
        const auto format = formats.at(index).toMap();
        const QPointF label = pointOnCircle((OuterRadius + InnerRadius) / 2, middle);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(dense ? 14 : 16);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        drawReadableText(QRectF(label.x() - 40, label.y() - (count < 8 ? 18 : 11), 80, 23),
                         Qt::AlignCenter, format.value(QStringLiteral("label")).toString(),
                         active ? QColor("#422051") : QColor("#33213f"));
        if (count < 8) {
            font.setPixelSize(10);
            font.setWeight(QFont::Normal);
            painter.setFont(font);
            const auto detail = QFontMetrics(font).elidedText(format.value(QStringLiteral("detail")).toString(), Qt::ElideRight, 90);
            drawReadableText(QRectF(label.x() - 45, label.y() + 4, 90, 18), Qt::AlignCenter,
                             detail, QColor("#44314f"));
        }
        painter.restore();
    }

    QLinearGradient core(-InnerRadius * 0.6, -InnerRadius, InnerRadius * 0.5, InnerRadius);
    core.setColorAt(0, glassColor(255, 254, 246, 162));
    core.setColorAt(0.46, glassColor(240, 247, 255, 158));
    core.setColorAt(1, glassColor(235, 220, 250, 164));
    painter.setBrush(core);
    painter.setPen(QPen(tintColor(238, 226, 249, 185), 1));
    painter.drawEllipse(QPointF(), InnerRadius - 7, InnerRadius - 7);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#67388b"));
    painter.drawEllipse(QRectF(-27, -51, 40, 37));
    painter.setBrush(QColor("#8a55b1"));
    painter.drawEllipse(QRectF(-23, -48, 29, 10));
    painter.setBrush(QColor("#8d57a8"));
    painter.drawEllipse(QRectF(-4, -47, 38, 34));
    painter.setBrush(QColor("#f3c64d"));
    painter.drawEllipse(QRectF(0, -43, 30, 26));
    painter.setBrush(QColor("#563b52"));
    for (int seed = 0; seed < 14; ++seed) {
        const double angle = seed * 2.39996323;
        const double radius = 2 + 9 * std::sqrt(double(seed) / 13);
        painter.drawEllipse(QPointF(15 + radius * std::cos(angle), -30 + radius * 0.75 * std::sin(angle)), 1.5, 2.2);
    }
    QFont centerFont(QStringLiteral("Segoe UI"));
    centerFont.setPixelSize(13);
    centerFont.setWeight(QFont::DemiBold);
    painter.setFont(centerFont);
    const auto activeFormat = hovered >= 0 && hovered < formats.size() ? formats.at(hovered).toMap() : QVariantMap{};
    const QString heading = activeFormat.isEmpty()
        ? (toolsMode ? WindowsGesture::tr("Solte na ferramenta") : WindowsGesture::tr("Solte no formato"))
        : activeFormat.value(QStringLiteral("label")).toString();
    drawReadableText(QRectF(-65, -5, 130, 23), Qt::AlignCenter, heading, QColor("#33213f"));
    centerFont.setPixelSize(10);
    centerFont.setWeight(QFont::Normal);
    painter.setFont(centerFont);
    const QString detail = activeFormat.isEmpty() ? WindowsGesture::tr("%1 arquivo(s)\nOriginais preservados").arg(urls.size())
        : activeFormat.value(QStringLiteral("detail")).toString();
    drawReadableText(QRectF(-65, 20, 130, 45), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                     detail, QColor("#44314f"));
}

WindowsGesture::WindowsGesture(Converter *converter, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, converter)) {}
WindowsGesture::~WindowsGesture() = default;
bool WindowsGesture::available() const { return d->available; }
bool WindowsGesture::enabled() const { return d->enabled; }
QString WindowsGesture::status() const { return d->status; }
bool WindowsGesture::prepare()
{
    const bool wasAvailable = d->available;
    const bool ready = d->initialize();
    if (ready && !wasAvailable) emit changed();
    return ready;
}
void WindowsGesture::start() { d->enable(true); }
void WindowsGesture::stop() { d->shutdown(true); }
void WindowsGesture::setEnabled(bool enabled) { d->enable(enabled); }
bool WindowsGesture::setShortcut(Qt::KeyboardModifiers modifiers, int key)
{
    return setActivation(modifiers, key, true);
}
bool WindowsGesture::setActivation(Qt::KeyboardModifiers modifiers, int key, bool requireDrag)
{
    return d->configureActivation(modifiers, key, requireDrag);
}
bool WindowsGesture::setToolsShortcut(Qt::KeyboardModifiers modifiers, int key)
{
    return d->configureTools(modifiers, key);
}
bool WindowsGesture::setActivations(Qt::KeyboardModifiers modifiers, int key, bool requireDrag,
                                    Qt::KeyboardModifiers toolsModifiers, int toolsKey)
{
    if (!validShortcut(toolsModifiers, toolsKey)) {
        d->error = tr("Escolha modificadores ou uma combinação com letra, número ou F1 a F12 para as ferramentas.");
        return false;
    }
    if (modifiers == toolsModifiers && key == toolsKey) {
        d->error = tr("Escolha atalhos diferentes para converter e abrir ferramentas.");
        return false;
    }
    // A tools-only combination can temporarily be our own outgoing direct
    // hotkey. Do not mistake that reservation for a different app's conflict.
    const bool retiringOwnedKey = d->hotkeyId && toolsModifiers == d->shortcutModifiers
        && toolsKey == d->shortcutKey;
    int probe = 0;
    if (toolsKey && toolsKey != Qt::Key_F12 && !retiringOwnedKey
        && !d->registerHotkey(toolsModifiers, toolsKey, probe)) return false;
    if (probe) UnregisterHotKey(nullptr, probe);
    const auto previousModifiers = d->toolsModifiers;
    const int previousKey = d->toolsKey;
    const int previousNativeKey = d->toolsNativeKey;
    d->toolsModifiers = toolsModifiers;
    d->toolsKey = toolsKey;
    d->toolsNativeKey = nativeKey(toolsKey);
    if (!d->configureActivation(modifiers, key, requireDrag)) {
        d->toolsModifiers = previousModifiers;
        d->toolsKey = previousKey;
        d->toolsNativeKey = previousNativeKey;
        return false;
    }
    // configureActivation can be a no-op when only the tools binding changed.
    // Invalidate its preceding probe in either case, without input sampling.
    d->hideWheel();
    d->haveOrigin = d->moved = d->attemptedThisPress = d->lastLeft = false;
    d->lastTrigger = d->lastToolsTrigger = false;
    if (d->enabled) d->status = d->activeStatus();
    emit changed();
    return true;
}
void WindowsGesture::setExternalBusy(bool busy)
{
    if (d->externalBusy == busy) return;
    d->externalBusy = busy;
    if (!busy) return;
    d->hideWheel();
    // A background file operation cannot authorize a probe from the drag that
    // preceded it. Resume only after the next complete button press.
    d->haveOrigin = d->moved = d->attemptedThisPress = false;
    d->lastLeft = true;
    d->lastTrigger = d->lastToolsTrigger = false;
}
void WindowsGesture::setWheelTransparency(int percent)
{
    const int clamped = std::clamp(percent, 0, 80);
    if (d->wheelTransparency == clamped) return;
    d->wheelTransparency = clamped;
    if (d->wheel && d->wheel->isVisible()) d->wheel->update();
}
void WindowsGesture::setWheelSize(int percent)
{
    // Keep an existing OLE target stationary for the rest of its drag. Painting
    // and hit testing both follow its actual client rectangle; the next probe
    // creates that rectangle with the newly configured extent.
    d->wheelSize = std::clamp(percent, 70, 130);
}
void WindowsGesture::setWheelColor(const QString &hex)
{
    const auto defaultColor = WheelAppearance::DefaultColor;
    WheelAppearance::Rgb color = defaultColor;
    if (!WheelAppearance::parseColor(hex.toStdString(), &color)) color = defaultColor;
    if (d->wheelColor.red == color.red && d->wheelColor.green == color.green
        && d->wheelColor.blue == color.blue) return;
    d->wheelColor = color;
    if (d->wheel && d->wheel->isVisible()) d->wheel->update();
}
QString WindowsGesture::errorString() const { return d->error; }

#ifdef MARACUJA_WINDOWS_GESTURE_TESTING
bool WindowsGesture::initializeForTest()
{
    d->inputPollingAllowed = false;
    d->polling.stop();
    if (!d->initialize()) return false;
    d->enabled = true;
    d->polling.stop();
    return true;
}
void WindowsGesture::showForTest(const QPoint &nativePoint)
{
    d->showCandidate(POINT{LONG(nativePoint.x()), LONG(nativePoint.y())});
}
void WindowsGesture::showToolsForTest(const QPoint &nativePoint)
{
    d->showCandidate(POINT{LONG(nativePoint.x()), LONG(nativePoint.y())}, true);
}
void *WindowsGesture::dropTargetForTest() const { return static_cast<IDropTarget *>(d->target); }
void *WindowsGesture::nativeWindowForTest() const { return d->window; }
bool WindowsGesture::shortcutMatchesForTest(Qt::KeyboardModifiers modifiers, int key) const
{
    return d->triggerMatches(modifiers, key);
}
bool WindowsGesture::toolsShortcutMatchesForTest(Qt::KeyboardModifiers modifiers, int key) const
{
    return modifiers == d->toolsModifiers && (!d->toolsKey || key == d->toolsKey);
}
int WindowsGesture::shortcutVirtualKeyForTest() const { return d->shortcutNativeKey; }
bool WindowsGesture::requiresDragForTest() const { return d->requireDrag; }
bool WindowsGesture::ownsHotkeyForTest() const { return d->hotkeyId != 0; }
bool WindowsGesture::dispatchHotkeyForTest()
{
    if (!d->hotkeyId) return false;
    MSG message {};
    message.message = WM_HOTKEY;
    message.wParam = WPARAM(d->hotkeyId);
    qintptr result = -1;
    return d->nativeEventFilter(QByteArrayLiteral("windows_dispatcher_MSG"), &message, &result)
        && result == 0;
}
#endif

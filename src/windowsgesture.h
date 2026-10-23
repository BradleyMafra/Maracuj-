#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

class Converter;
class QPoint;

// Windows-only desktop adapter. It reads the configured trigger keys, the four
// modifier states and the left button, never text input or the clipboard. File
// data arrives only through this app's registered OLE drop target. The public
// header intentionally has no Win32 types.
class WindowsGesture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit WindowsGesture(Converter *converter, QObject *parent = nullptr);
    ~WindowsGesture() override;

    bool available() const;
    bool enabled() const;
    QString status() const;
    // Discover/preinitialize desktop capability without enabling polling or
    // reserving a shortcut. A binding conflict must not make the UI unusable.
    bool prepare();
    void start();
    void stop();
    void setEnabled(bool enabled);
    // One simultaneous combination, used while dragging. key=0 means modifiers
    // only; otherwise it is an A-Z, 0-9 or F1-F12 Qt::Key. Invalid combinations
    // leave the previous trigger unchanged. Persistence belongs to the panel.
    bool setShortcut(Qt::KeyboardModifiers modifiers, int key = 0);
    // Without a drag, use the OS's reserved hotkey mechanism; a conflict leaves
    // the previous activation working. Standalone letters/digits need Ctrl,
    // Alt or Meta. Only F1-F12 may be standalone without those modifiers.
    bool setActivation(Qt::KeyboardModifiers modifiers, int key, bool requireDrag);
    // The independent file-tools wheel always requires a drag. Its shortcut
    // defaults to Alt and must differ from a conversion drag shortcut.
    bool setToolsShortcut(Qt::KeyboardModifiers modifiers, int key = 0);
    // Apply a saved pair without transient collisions against the defaults or
    // the previous pair (including exchanging the two drag shortcuts).
    bool setActivations(Qt::KeyboardModifiers modifiers, int key, bool requireDrag,
                        Qt::KeyboardModifiers toolsModifiers, int toolsKey);
    void setExternalBusy(bool busy);
    // Background transparency only. Labels and the native drag target remain
    // fully functional; settings persistence belongs to the desktop panel.
    void setWheelTransparency(int percent);
    // Percentage of the default wheel extent (70-130). Apply on the next
    // gesture so changing a preference cannot move an active drop target.
    void setWheelSize(int percent);
    // Surface tint only, in strict #RRGGBB form. Text and interaction accents
    // keep their normal colors; persistence belongs to the desktop panel.
    void setWheelColor(const QString &hex);
    QString errorString() const;

#ifdef MARACUJA_WINDOWS_GESTURE_TESTING
    // Test target only: no keyboard/button polling or synthesized input. OLE
    // methods are exercised directly with an in-process generated data object.
    bool initializeForTest();
    void showForTest(const QPoint &nativePoint);
    void showToolsForTest(const QPoint &nativePoint);
    void *dropTargetForTest() const;
    void *nativeWindowForTest() const;
    bool shortcutMatchesForTest(Qt::KeyboardModifiers modifiers, int key) const;
    bool toolsShortcutMatchesForTest(Qt::KeyboardModifiers modifiers, int key) const;
    int shortcutVirtualKeyForTest() const;
    bool requiresDragForTest() const;
    bool ownsHotkeyForTest() const;
    bool dispatchHotkeyForTest();
#endif

signals:
    void changed();
    // A live, accepted file drag entered a different actionable sector. The
    // panel owns playback and the user's sound preference.
    void wheelSectorHovered();
    // Emitted after an accepted gesture has actually started a conversion.
    void conversionStarted();
    void activationRequested();
    // File tools open their settings after OLE returns; they do not start an
    // implicit conversion or mutate the dropped files.
    void toolsRequested(const QStringList &paths, const QString &tool);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

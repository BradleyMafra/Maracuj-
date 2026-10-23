#pragma once
#include <QObject>
#include <QWindow>

// QML Window.startSystemMove requires Qt 6.8; the C++ API also works with
// our supported Qt 6.4 baseline and the compositor's native move protocol.
class WindowMoveHandle final : public QObject {
    Q_OBJECT
public:
    explicit WindowMoveHandle(QObject *parent = nullptr) : QObject(parent) {}
    void setWindow(QWindow *window) { m_window = window; }
    Q_INVOKABLE void beginMove() { if (m_window) m_window->startSystemMove(); }
private:
    QWindow *m_window = nullptr;
};

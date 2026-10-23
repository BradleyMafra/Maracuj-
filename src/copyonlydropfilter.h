#pragma once

#include <QDropEvent>
#include <QObject>

// QQuickDeliveryAgent does not propagate the chosen action out of its first
// translated DragEnter event. Set the native event's
// action before Quick delivers it; acceptance remains entirely up to the scene.
// This fixes Quick's native preference, not Qt Wayland's offered action mask.
// Refuse Shift+Move-capable drags in this Qt fallback: the separate native
// Wayland gesture destination is responsible for that case with Copy-only.
// Installed exclusively on Mandarina's own windows, never on global input.
class CopyDropPreferenceFilter final : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove
                || event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            if (drop->modifiers().testFlag(Qt::ShiftModifier)
                    && drop->possibleActions().testFlag(Qt::MoveAction)) {
                drop->ignore();
                return true;
            }
            if (drop->possibleActions().testFlag(Qt::CopyAction))
                drop->setDropAction(Qt::CopyAction);
        }
        return QObject::eventFilter(object, event);
    }
};

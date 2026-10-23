#pragma once

#include <QString>
#include <Qt>

// One simultaneous chord. Modifier-only chords are useful during a file drag;
// a standalone chord also needs a final key that the desktop can register.
struct ActivationShortcut {
    Qt::KeyboardModifiers modifiers = Qt::ShiftModifier;
    int key = 0;
    bool requireDrag = true;

    QString portable() const;
    QString display() const;
    static bool parse(QString text, bool requireDrag, ActivationShortcut *result,
                      QString *error);
    static QString fromKey(int key, int modifiers);
};

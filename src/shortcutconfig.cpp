#include "shortcutconfig.h"

#include <QKeySequence>
#include <QStringList>

namespace {
constexpr int supportedModifiers = int(Qt::ControlModifier) | int(Qt::AltModifier)
    | int(Qt::ShiftModifier) | int(Qt::MetaModifier);

QString modifierText(Qt::KeyboardModifiers modifiers)
{
    QStringList parts;
    if (modifiers.testFlag(Qt::ControlModifier)) parts << QStringLiteral("Ctrl");
    if (modifiers.testFlag(Qt::AltModifier)) parts << QStringLiteral("Alt");
    if (modifiers.testFlag(Qt::ShiftModifier)) parts << QStringLiteral("Shift");
    if (modifiers.testFlag(Qt::MetaModifier)) parts << QStringLiteral("Meta");
    return parts.join(QLatin1Char('+'));
}

Qt::KeyboardModifier modifierForName(const QString &name)
{
    if (name.compare(QStringLiteral("Ctrl"), Qt::CaseInsensitive) == 0) return Qt::ControlModifier;
    if (name.compare(QStringLiteral("Alt"), Qt::CaseInsensitive) == 0) return Qt::AltModifier;
    if (name.compare(QStringLiteral("Shift"), Qt::CaseInsensitive) == 0) return Qt::ShiftModifier;
    if (name.compare(QStringLiteral("Meta"), Qt::CaseInsensitive) == 0) return Qt::MetaModifier;
    return Qt::NoModifier;
}

int keyForName(const QString &name)
{
    if (name.size() == 1) {
        const auto character = name.at(0).unicode();
        if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))
            return int(character);
        if (character >= 'a' && character <= 'z') return int(character - 'a' + 'A');
    }
    const QString upper = name.toUpper();
    if (upper.startsWith(QLatin1Char('F')) && upper.size() >= 2 && upper.size() <= 3) {
        bool ok = false;
        const int number = upper.mid(1).toInt(&ok);
        // Reject alternate spellings such as F01 and F+1.
        if (ok && number >= 1 && number <= 12 && upper.mid(1) == QString::number(number))
            return int(Qt::Key_F1) + number - 1;
    }
    return 0;
}

bool isSupportedKey(int key)
{
    return (key >= Qt::Key_A && key <= Qt::Key_Z)
        || (key >= Qt::Key_0 && key <= Qt::Key_9)
        || (key >= Qt::Key_F1 && key <= Qt::Key_F12);
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
}

QString ActivationShortcut::portable() const
{
    if (key == 0) return modifierText(modifiers);
    return QKeySequence(QKeyCombination(modifiers, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QString ActivationShortcut::display() const
{
    if (key == 0) return modifierText(modifiers);
    return QKeySequence(QKeyCombination(modifiers, Qt::Key(key))).toString(QKeySequence::NativeText);
}

bool ActivationShortcut::parse(QString text, bool drag, ActivationShortcut *result, QString *error)
{
    if (error) error->clear();
    text = text.trimmed();
    if (text.isEmpty()) return fail(error, QStringLiteral("Escolha uma tecla ou uma combinação para o atalho."));
    if (text.size() > 80 || text.contains(QLatin1Char(',')))
        return fail(error, QStringLiteral("Use uma única combinação de teclas pressionadas ao mesmo tempo."));
    for (const QChar character : text) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f)
            return fail(error, QStringLiteral("Use uma única combinação de teclas pressionadas ao mesmo tempo."));
    }

    const QStringList parts = text.split(QLatin1Char('+'), Qt::KeepEmptyParts);
    if (parts.size() > 5)
        return fail(error, QStringLiteral("Use Ctrl, Alt, Shift ou Meta e no máximo uma tecla final."));
    ActivationShortcut parsed;
    parsed.modifiers = Qt::NoModifier;
    parsed.requireDrag = drag;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QString token = parts.at(i).trimmed();
        if (token.isEmpty())
            return fail(error, QStringLiteral("Complete a combinação; o sinal + deve separar as teclas."));
        const auto modifier = modifierForName(token);
        if (modifier != Qt::NoModifier) {
            if (parsed.modifiers.testFlag(modifier))
                return fail(error, QStringLiteral("Cada tecla modificadora deve aparecer uma única vez."));
            parsed.modifiers |= modifier;
            continue;
        }
        if (i != parts.size() - 1)
            return fail(error, QStringLiteral("Coloque Ctrl, Alt, Shift ou Meta antes da tecla final."));
        parsed.key = keyForName(token);
        if (!parsed.key)
            return fail(error, QStringLiteral("Use letras A–Z, números 0–9 ou teclas F1–F12. AltGr e teclado numérico não são aceitos."));
    }

    if (parsed.key == 0 && !drag)
        return fail(error, QStringLiteral("Para abrir sem arrastar, inclua uma letra, um número ou uma tecla F1–F12."));
    if (parsed.key != 0 && parsed.key < Qt::Key_F1
            && !(parsed.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        return fail(error, QStringLiteral("Combine letras ou números com Ctrl, Alt ou Meta para preservar a digitação normal."));
    }
    if (result) *result = parsed;
    return true;
}

QString ActivationShortcut::fromKey(int pressedKey, int modifierBits)
{
    // Called only by the app's focused settings recorder. Ignore keys and
    // modifier flags which cannot be represented safely on both desktops.
    if ((modifierBits & ~supportedModifiers) != 0) return {};
    ActivationShortcut captured;
    captured.modifiers = Qt::KeyboardModifiers(modifierBits);
    switch (pressedKey) {
    case Qt::Key_Control: captured.modifiers |= Qt::ControlModifier; break;
    case Qt::Key_Alt: captured.modifiers |= Qt::AltModifier; break;
    case Qt::Key_Shift: captured.modifiers |= Qt::ShiftModifier; break;
    case Qt::Key_Meta: captured.modifiers |= Qt::MetaModifier; break;
    default:
        if (!isSupportedKey(pressedKey)) return {};
        captured.key = pressedKey;
    }
    return captured.portable();
}

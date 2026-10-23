#include "shortcutconfig.h"

#include <QGuiApplication>
#include <QKeySequence>
#include <cstdio>

namespace {
bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "shortcut_config_check: %s\n", message);
    return condition;
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ActivationShortcut defaults;
    if (!check(defaults.modifiers == Qt::ShiftModifier && defaults.key == 0 && defaults.requireDrag,
               "o padrão deve ser Shift com arraste")
        || !check(defaults.portable() == QStringLiteral("Shift") && !defaults.display().isEmpty(),
                  "o padrão precisa de representação legível")) return 1;

    struct Valid {
        const char *text;
        bool drag;
        Qt::KeyboardModifiers modifiers;
        int key;
    };
    const Valid valid[] = {
        {"Shift", true, Qt::ShiftModifier, 0},
        {"Ctrl", true, Qt::ControlModifier, 0},
        {"Alt", true, Qt::AltModifier, 0},
        {"Meta", true, Qt::MetaModifier, 0},
        {"  meta + shift + alt + ctrl  ", true, Qt::MetaModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier, 0},
        {"Ctrl+Alt+M", true, Qt::ControlModifier | Qt::AltModifier, Qt::Key_M},
        {"alt + ctrl + m", false, Qt::ControlModifier | Qt::AltModifier, Qt::Key_M},
        {"Meta+0", false, Qt::MetaModifier, Qt::Key_0},
        {"Ctrl+Shift+9", true, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_9},
        {"Alt+Z", true, Qt::AltModifier, Qt::Key_Z},
        {"F1", true, Qt::NoModifier, Qt::Key_F1},
        {"f8", false, Qt::NoModifier, Qt::Key_F8},
        {"F12", false, Qt::NoModifier, Qt::Key_F12},
        {"Shift+F7", true, Qt::ShiftModifier, Qt::Key_F7},
        {"Meta+Ctrl+Shift+F12", false, Qt::MetaModifier | Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_F12}
    };
    for (const auto &entry : valid) {
        ActivationShortcut parsed;
        QString error = QStringLiteral("erro anterior");
        if (!check(ActivationShortcut::parse(QString::fromLatin1(entry.text), entry.drag, &parsed, &error), entry.text)
            || !check(error.isEmpty(), "parse válido não limpou o erro")
            || !check(parsed.modifiers == entry.modifiers && parsed.key == entry.key && parsed.requireDrag == entry.drag,
                      "parse alterou as teclas ou o modo")
            || !check(!parsed.display().isEmpty(), "atalho válido ficou sem texto para a interface")) return 1;
        ActivationShortcut roundTrip;
        if (!check(ActivationShortcut::parse(parsed.portable(), entry.drag, &roundTrip, &error), "representação não faz round-trip")
            || !check(roundTrip.modifiers == parsed.modifiers && roundTrip.key == parsed.key
                      && roundTrip.requireDrag == parsed.requireDrag, "round-trip alterou o atalho")
            || !check(roundTrip.portable() == parsed.portable(), "a representação não é canônica")) return 1;
        if (parsed.key && !check(QKeySequence::fromString(parsed.portable(), QKeySequence::PortableText)[0]
                                   == QKeyCombination(parsed.modifiers, Qt::Key(parsed.key)),
                               "a combinação não corresponde ao formato portátil do Qt")) return 1;
    }
    ActivationShortcut allModifiers;
    ActivationShortcut::parse(QStringLiteral("Meta+Shift+Alt+Ctrl"), true, &allModifiers, nullptr);
    if (!check(allModifiers.portable() == QStringLiteral("Ctrl+Alt+Shift+Meta"), "ordem canônica das modificadoras mudou")) return 1;

    const char *invalid[] = {
        "", " ", "A", "9", "Shift+M", "Shift+0", "Ctrl+Alt+M, Ctrl+N",
        "Ctrl+Alt+M+N", "Ctrl+Ctrl+M", "Alt+Alt", "Ctrl++M", "+Ctrl", "Ctrl+",
        "Ctrl+;", "Ctrl+Space", "Ctrl+Escape", "F0", "F01", "F13", "F24",
        "Ctrl+Num+1", "Keypad+1", "AltGr+M", "Ctrl+é", "Ctrl+中", "Ctrl+ı", "Ctrl+ſ", "M+Ctrl", "Ctrl+\nM"
    };
    for (const char *entry : invalid) {
        ActivationShortcut unchanged;
        QString error;
        if (!check(!ActivationShortcut::parse(QString::fromUtf8(entry), true, &unchanged, &error), entry)
            || !check(!error.isEmpty(), "atalho inválido ficou sem mensagem")
            || !check(unchanged.modifiers == Qt::ShiftModifier && unchanged.key == 0 && unchanged.requireDrag,
                      "falha de validação modificou a saída")) return 1;
    }
    for (const char *entry : {"Shift", "Ctrl", "Alt", "Meta", "Ctrl+Alt", "Ctrl+Alt+Shift+Meta"}) {
        QString error;
        if (!check(!ActivationShortcut::parse(QString::fromLatin1(entry), false, nullptr, &error),
                   "modificadoras sozinhas não podem abrir sem arraste")
            || !check(!error.isEmpty(), "modo direto inválido ficou sem mensagem")) return 1;
    }
    if (!check(ActivationShortcut::parse(QStringLiteral("Ctrl+Alt+M"), false, nullptr, nullptr),
               "validação sem saída opcional falhou")) return 1;

    if (!check(ActivationShortcut::fromKey(Qt::Key_Control, 0) == QStringLiteral("Ctrl"), "Control não incluiu sua flag")
        || !check(ActivationShortcut::fromKey(Qt::Key_Alt, int(Qt::ControlModifier)) == QStringLiteral("Ctrl+Alt"),
                  "Alt não preservou a flag anterior")
        || !check(ActivationShortcut::fromKey(Qt::Key_Shift, int(Qt::ControlModifier | Qt::AltModifier)) == QStringLiteral("Ctrl+Alt+Shift"),
                  "Shift não incluiu sua flag")
        || !check(ActivationShortcut::fromKey(Qt::Key_Meta, int(Qt::ShiftModifier)) == QStringLiteral("Shift+Meta"),
                  "Meta não incluiu sua flag")
        || !check(ActivationShortcut::fromKey(Qt::Key_M, int(Qt::ControlModifier | Qt::AltModifier)) == QStringLiteral("Ctrl+Alt+M"),
                  "a captura não preservou Ctrl+Alt+M")
        || !check(ActivationShortcut::fromKey(Qt::Key_F8, 0) == QStringLiteral("F8"), "a captura de F8 falhou")
        || !check(ActivationShortcut::fromKey(Qt::Key_M, int(Qt::ShiftModifier)) == QStringLiteral("Shift+M"),
                  "a captura deve exibir candidatos para validação")) return 1;
    for (int key : {0, int(Qt::Key_unknown), int(Qt::Key_AltGr), int(Qt::Key_F13), int(Qt::Key_Space), int(Qt::Key_Escape)}) {
        if (!check(ActivationShortcut::fromKey(key, int(Qt::ControlModifier)).isEmpty(), "tecla não suportada foi capturada")) return 1;
    }
    for (int flags : {int(Qt::KeypadModifier), int(Qt::GroupSwitchModifier), int(Qt::ControlModifier | Qt::KeypadModifier), 1, -1}) {
        if (!check(ActivationShortcut::fromKey(Qt::Key_M, flags).isEmpty(), "modificador não suportado foi capturado")) return 1;
    }
    std::puts("shortcut_config_check: padrão, parse, round-trip, modo direto, captura local e rejeição de atalhos inseguros");
    return 0;
}

// SPDX-License-Identifier: MIT
// Candidate detector only. The native destination must validate a real
// local-file drag before displaying its wheel or accepting Copy.
// Only the configured global shortcut is registered. No raw key events,
// clipboard data, filenames, input history or logs are captured.
"use strict";

(function () {
    // Values from Qt::MouseButton / Qt::KeyboardModifier. ScriptedEffect uses
    // QJSEngine and does not promise the QML global `Qt` object.
    var LEFT_BUTTON = 1;
    var SHIFT_MODIFIER = 0x02000000;
    var ALT_MODIFIER = 0x08000000;
    var SUPPORTED_MODIFIERS = 0x1e000000; // Shift, Ctrl, Alt, Meta.
    var ALL_MODIFIERS = 0x7e000000; // Also reject Keypad / GroupSwitch (AltGr).
    var MIN_DISTANCE_SQUARED = 8 * 8;
    var APP_ID = "org.winux.Mandarina";
    var pressOrigin = null;
    var movedEnough = false;
    var leftDown = false;
    var lastPosition = null;

    function readOption(name, fallback) {
        if (typeof effect === "undefined" || typeof effect.readConfig !== "function") return fallback;
        var value = effect.readConfig(name, fallback);
        return value === undefined || value === null ? fallback : value;
    }

    // The application reloads only this effect after applying its settings.
    // Registering again in configChanged would leave the old QAction alive.
    var triggerMode = String(readOption("TriggerMode", "drag"));
    if (triggerMode !== "direct") triggerMode = "drag";
    function modifierOption(name, fallback) {
        var value = Number(readOption(name, fallback));
        if (!isFinite(value) || Math.floor(value) !== value
                || value <= 0 || (value & SUPPORTED_MODIFIERS) !== value) return fallback;
        return value;
    }
    var conversion = {
        mode: triggerMode,
        modifiers: modifierOption("TriggerModifiers", SHIFT_MODIFIER),
        shortcut: String(readOption("TriggerShortcut", "")).trim(),
        action: String(readOption("ShortcutAction", "")).trim(),
        actionPattern: /^_k_session:maracuja-wheel-[A-Za-z0-9-]+$/,
        caption: "MaracujaShiftProbe",
        label: "Maracujá · Converter arquivos",
        triggered: false
    };
    var tools = {
        mode: "drag",
        modifiers: modifierOption("ToolsModifiers", ALT_MODIFIER),
        shortcut: String(readOption("ToolsShortcut", "")).trim(),
        action: String(readOption("ToolsShortcutAction", "")).trim(),
        actionPattern: /^_k_session:maracuja-tools-[A-Za-z0-9-]+$/,
        caption: "MaracujaToolsProbe",
        label: "Maracujá · Ferramentas de arquivo",
        triggered: false
    };
    var triggers = [conversion];
    if (readOption("ToolsEnabled", true) !== false) triggers.push(tools);

    function isProbe(window, caption) {
        if (!window || window.deleted || !window.managed || !window.waylandClient
                || window.caption !== caption || !window.moveable) {
            return false;
        }
        // KWin's EffectWindow::windowClass() combines resourceName and
        // resourceClass with a space. Match the app id as one exact token.
        var classes = String(window.windowClass).split(/\s+/);
        return classes.indexOf(APP_ID) !== -1;
    }

    function findUniqueProbe(caption) {
        var candidate = null;
        var windows = effects.stackingOrder;
        for (var i = 0; i < windows.length; ++i) {
            if (!isProbe(windows[i], caption)) continue;
            // Ambiguous instances are deliberately left untouched.
            if (candidate !== null) return null;
            candidate = windows[i];
        }
        return candidate;
    }

    function showDragProbe(trigger, pos) {
        if (!leftDown || !movedEnough || !pos || effects.hasActiveFullScreenEffect) return false;
        var probe = findUniqueProbe(trigger.caption);
        if (probe === null) return false;
        // Follow the native target's configured window geometry (70-130%).
        // An initial server decoration may still be pending removal; use the
        // default only until the target reports a valid square client extent.
        var width = Number(probe.width);
        var height = Number(probe.height);
        if (!isFinite(width) || !isFinite(height) || width !== height
                || width < 322 || width > 598) {
            width = height = 460;
        }
        var destination = {
            x: Math.round(Number(pos.x) - width / 2),
            y: Math.round(Number(pos.y) - height / 2)
        };
        // Never activate a window or alter the user's application windows.
        // The probe owns its focus policy, timeout and actual drag validation.
        var other = findUniqueProbe(trigger === tools ? conversion.caption : tools.caption);
        if (other !== null && !other.minimized) other.minimized = true;
        effects.moveWindow(probe, destination, false, 1.0);
        if (probe.minimized) probe.minimized = false;
        return true;
    }

    function registerTrigger(trigger) {
        if (trigger.shortcut.length > 0 && trigger.shortcut.length <= 80
            && trigger.actionPattern.test(trigger.action)
            && typeof registerShortcut === "function") {
        registerShortcut(trigger.action, trigger.label, trigger.shortcut, function () {
            // QAction can repeat while a key is held. Keep the wheel anchored
            // at its initial position until this drag ends, even if dismissed.
            if (trigger.mode === "drag" && !trigger.triggered && showDragProbe(trigger, lastPosition))
                trigger.triggered = true;
            // ScriptedEffect has no callDBus API. In direct mode the app
            // listens to KGlobalAccel's signal for this exact session action
            // and opens its compact panel. Never show the drag probe here.
        });
        }
    }
    triggers.forEach(registerTrigger);

    effects.mouseChanged.connect(function (pos, oldPos, buttons, oldButtons, modifiers, oldModifiers) {
        lastPosition = { x: Number(pos.x), y: Number(pos.y) };
        leftDown = (Number(buttons) & LEFT_BUTTON) !== 0;
        var wasLeftDown = (Number(oldButtons) & LEFT_BUTTON) !== 0;
        if (!leftDown) {
            pressOrigin = null;
            movedEnough = false;
            triggers.forEach(function (trigger) { trigger.triggered = false; });
            return;
        }
        if (!wasLeftDown) {
            pressOrigin = { x: Number(pos.x), y: Number(pos.y) };
            movedEnough = false;
            triggers.forEach(function (trigger) { trigger.triggered = false; });
        }
        if (pressOrigin === null) return;

        var dx = Number(pos.x) - pressOrigin.x;
        var dy = Number(pos.y) - pressOrigin.y;
        if (dx * dx + dy * dy >= MIN_DISTANCE_SQUARED) movedEnough = true;
        triggers.forEach(function (trigger) {
            if (trigger.mode !== "drag" || trigger.shortcut.length > 0 || trigger.triggered) return;
            var modifiersDown = (Number(modifiers) & ALL_MODIFIERS) === trigger.modifiers;
            var wereModifiersDown = (Number(oldModifiers) & ALL_MODIFIERS) === trigger.modifiers;
            if (modifiersDown && !wereModifiersDown && showDragProbe(trigger, lastPosition))
                trigger.triggered = true;
        });
    });
}());

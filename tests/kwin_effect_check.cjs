// The compositor-side script is evaluated with synthetic state, never host input.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const code = fs.readFileSync(path.join(__dirname, '../integration/kwin/mandarina-shift/contents/code/main.js'), 'utf8');
const SHIFT = 0x02000000;
const CTRL = 0x04000000;
const ALT = 0x08000000;
const META = 0x10000000;
const GROUP_SWITCH = 0x40000000;
const ACTION = '_k_session:maracuja-wheel-12345678-1234-1234-1234-123456789abc';
const TOOLS_ACTION = '_k_session:maracuja-tools-12345678-1234-1234-1234-123456789abc';

function create(config = {}, overrides = {}, legacy = false) {
    let mouseCallback;
    let oldButtons = 0;
    let oldModifiers = 0;
    const probe = {caption:'MaracujaShiftProbe', windowClass:'Mandarina org.winux.Mandarina',
        managed:true, waylandClient:true, moveable:true, minimized:true, width:460, height:460};
    const toolsProbe = {...probe, caption:'MaracujaToolsProbe'};
    const moves = [];
    const shortcuts = [];
    const effects = {stackingOrder:[probe], hasActiveFullScreenEffect:false,
        mouseChanged:{connect(fn) {mouseCallback=fn;}}, moveWindow(window, point) {moves.push({window, point});}, ...overrides};
    const context = {effects, registerShortcut(action, label, sequence, callback) {
        shortcuts.push({action, label, sequence, callback});
    }};
    if (!legacy) context.effect = {readConfig(name, fallback) {
        return Object.hasOwn(config, name) ? config[name] : fallback;
    }};
    vm.runInNewContext(code, context, {timeout:1000});
    function event(x, y, buttons, modifiers = 0) {
        mouseCallback({x,y}, {x,y}, buttons, oldButtons, modifiers, oldModifiers);
        oldButtons = buttons;
        oldModifiers = modifiers;
    }
    function drag() {
        event(460,480,1);
        event(500,500,1);
    }
    function shortcut(index = 0) {
        assert.ok(shortcuts[index], 'registered shortcut must exist');
        shortcuts[index].callback();
    }
    return {event, drag, shortcut, moves, probe, toolsProbe, shortcuts, effects};
}

// Defaults remain compatible with installations which do not have the XML yet.
for (const legacy of [false, true]) {
    const h = create({}, {}, legacy);
    h.drag(); h.event(500,500,1,SHIFT);
    assert.equal(h.moves.length, 1);
    assert.equal(h.moves[0].window, h.probe);
    assert.equal(h.moves[0].point.x, 270);
    assert.equal(h.moves[0].point.y, 270);
    assert.equal(h.probe.minimized, false);
    assert.equal(h.shortcuts.length, 0);
}

for (const input of ['typing','click','shift-first']) {
    const h = create();
    if (input === 'typing') h.event(500,500,0,SHIFT);
    else if (input === 'click') {h.event(500,500,1); h.event(500,500,1,SHIFT);}
    else {
        h.event(500,500,0,SHIFT); h.event(500,500,1,SHIFT); h.event(550,500,1,SHIFT);
    }
    assert.equal(h.moves.length, 0, input);
}

// Fullscreen effects and other applications' windows must remain untouched.
const sample = create().probe;
const invalidOverrides = [
    {hasActiveFullScreenEffect:true},
    {stackingOrder:[]},
    {stackingOrder:[{...sample,caption:'Outro aplicativo'}]},
    {stackingOrder:[{...sample,windowClass:'org.winux.Mandarina.Other'}]},
    {stackingOrder:[{...sample}, {...sample}]},
    {stackingOrder:[{...sample,deleted:true}]},
    {stackingOrder:[{...sample,waylandClient:false}]}
];
for (const overrides of invalidOverrides) {
    const h = create({}, overrides);
    h.drag(); h.event(500,500,1,SHIFT);
    assert.equal(h.moves.length, 0);
}

// Both independently owned wheels center using the actual configured extent,
// including outputs to the left of or above the primary screen. The effect
// moves the probe; output-edge clamping belongs to the regular KWin script.
for (const [caption, modifier] of [['MaracujaShiftProbe', SHIFT], ['MaracujaToolsProbe', ALT]]) {
    for (const extent of [322, 460, 598]) {
        for (const point of [{x: 1200, y: 800}, {x: -1500, y: -200}]) {
            const probe = {...sample, caption, width: extent, height: extent};
            const h = create({}, {stackingOrder: [probe]});
            h.event(point.x - 40, point.y - 20, 1);
            h.event(point.x, point.y, 1);
            h.event(point.x, point.y, 1, modifier);
            assert.equal(h.moves.length, 1);
            assert.equal(h.moves[0].window, probe);
            assert.deepEqual({...h.moves[0].point}, {
                x: point.x - extent / 2, y: point.y - extent / 2
            });
            assert.equal(probe.width, extent, 'placement must not resize the input target');
            assert.equal(probe.height, extent);
        }
    }
}
for (const dimensions of [
    {width: 321, height: 321}, {width: 599, height: 599},
    {width: 598, height: 597}, {width: NaN, height: 460},
    {width: 460, height: Infinity}, {width: 0, height: 0}
]) {
    const probe = {...sample, ...dimensions};
    const h = create({}, {stackingOrder: [probe]});
    h.drag(); h.event(500,500,1,SHIFT);
    assert.equal(h.moves.length, 1);
    assert.deepEqual({...h.moves[0].point}, {x: 270, y: 270},
        'initial or invalid target geometry falls back to the default extent');
}
for (const [caption, modifier] of [['MaracujaShiftBusy', SHIFT], ['MaracujaToolsBusy', ALT]]) {
    for (const probe of [
        {...sample, caption, width: 598, height: 598},
        {...sample, caption: caption.replace('Busy', 'Probe'), width: 322, height: 322,
            windowClass: 'org.winux.Mandarina.Other'}
    ]) {
        const h = create({}, {stackingOrder: [probe]});
        h.drag(); h.event(500,500,1,modifier);
        assert.equal(h.moves.length, 0, 'busy and unrelated scaled targets remain untouched');
        assert.equal(probe.minimized, true);
    }
}

// Modifier-only shortcuts require precisely the configured combination.
for (const mask of [CTRL, ALT, META, CTRL | ALT, CTRL | ALT | SHIFT]) {
    const h = create({TriggerModifiers:mask});
    h.drag(); h.event(500,500,1,mask);
    assert.equal(h.moves.length, 1);
    assert.equal(h.shortcuts.length, 0);
    h.event(500,500,1,mask);
    assert.equal(h.moves.length, 1, 'unchanged modifiers do not retrigger');
}
for (const mask of [CTRL, ALT, SHIFT, CTRL | ALT | SHIFT, CTRL | ALT | GROUP_SWITCH]) {
    const h = create({TriggerModifiers:CTRL | ALT});
    h.drag(); h.event(500,500,1,mask);
    assert.equal(h.moves.length, 0, `incomplete or extra modifiers: ${mask}`);
}
for (const invalid of [0,-1,1,1.5,'bad',CTRL | GROUP_SWITCH]) {
    const h = create({TriggerModifiers:invalid});
    h.drag(); h.event(500,500,1,SHIFT);
    assert.equal(h.moves.length, 1, 'invalid modifier setting uses the safe default');
}

// Key-based shortcuts are reserved once per effect instance and remain gated
// by an actual drag candidate. Typed keys themselves are never inspected.
for (const sequence of ['F8','Ctrl+Alt+M']) {
    const h = create({TriggerShortcut:sequence, ShortcutAction:ACTION});
    assert.equal(h.shortcuts.length, 1);
    assert.equal(h.shortcuts[0].action, ACTION);
    assert.equal(h.shortcuts[0].sequence, sequence);
    h.shortcut();
    assert.equal(h.moves.length, 0, 'no mouse button');
    h.event(460,480,1); h.shortcut();
    assert.equal(h.moves.length, 0, 'click without drag');
    h.event(500,500,1,SHIFT);
    assert.equal(h.moves.length, 0, 'old Shift gesture is disabled');
    h.shortcut();
    assert.equal(h.moves.length, 1);
    h.shortcut();
    h.event(580,540,1,SHIFT); h.shortcut();
    assert.equal(h.moves.length, 1, 'holding a key must not recenter the wheel');
    assert.equal(h.moves[0].point.x, 270);
    assert.equal(h.moves[0].point.y, 270);
    h.event(500,500,0); h.shortcut();
    assert.equal(h.moves.length, 1, 'button release clears the drag candidate');
    h.drag(); h.shortcut();
    assert.equal(h.moves.length, 2, 'a new drag can activate the shortcut again');
}
for (const overrides of invalidOverrides) {
    const h = create({TriggerShortcut:'F8', ShortcutAction:ACTION}, overrides);
    h.drag(); h.shortcut();
    assert.equal(h.moves.length, 0);
}
for (const action of ['', 'another-app-action', '_k_session:maracuja-wheel-../../other']) {
    const h = create({TriggerShortcut:'F8', ShortcutAction:action});
    assert.equal(h.shortcuts.length, 0, 'never register an unrelated action identifier');
    h.drag(); h.event(500,500,1,SHIFT);
    assert.equal(h.moves.length, 0, 'malformed key shortcut fails closed');
}

// Standalone opening is handled by the app's exact-action KGlobalAccel signal
// subscription. The effect must not move a drag destination or use callDBus,
// which is not part of ScriptedEffect's API.
{
    const h = create({TriggerMode:'direct', TriggerShortcut:'Ctrl+Alt+M', ShortcutAction:ACTION});
    assert.equal(h.shortcuts.length, 1);
    h.shortcut();
    h.drag(); h.event(500,500,1,SHIFT); h.shortcut();
    assert.equal(h.moves.length, 0);
    assert.equal(h.probe.minimized, true);
}
{
    const h = create({TriggerMode:'direct'});
    h.drag(); h.event(500,500,1,SHIFT);
    assert.equal(h.shortcuts.length, 0);
    assert.equal(h.moves.length, 0);
}
// File tools use their own destination, so the native Wayland enter event
// determines the requested wheel without observing keyboard input or focus.
{
    const h = create();
    h.effects.stackingOrder.push(h.toolsProbe);
    h.drag(); h.event(500,500,1,ALT);
    assert.equal(h.moves.length, 1);
    assert.equal(h.moves[0].window, h.toolsProbe);
    assert.equal(h.probe.minimized, true);
    assert.equal(h.toolsProbe.minimized, false);
    h.event(500,500,1,ALT | SHIFT);
    assert.equal(h.moves.length, 1, 'extra modifiers do not choose another wheel');
    h.event(500,500,1,SHIFT);
    assert.equal(h.moves.length, 2);
    assert.equal(h.moves[1].window, h.probe);
    assert.equal(h.toolsProbe.minimized, true, 'changing wheel hides only the other owned probe');
    h.event(500,500,1,ALT);
    assert.equal(h.moves.length, 2, 'one activation per wheel per drag avoids recentering');
    h.event(500,500,0); h.drag(); h.event(500,500,1,ALT);
    assert.equal(h.moves.length, 3);
}
for (const input of ['typing', 'click', 'alt-first', 'altgr']) {
    const h = create();
    h.effects.stackingOrder.push(h.toolsProbe);
    if (input === 'typing') h.event(500,500,0,ALT);
    else if (input === 'click') { h.event(500,500,1); h.event(500,500,1,ALT); }
    else if (input === 'alt-first') {
        h.event(500,500,0,ALT); h.event(500,500,1,ALT); h.event(550,500,1,ALT);
    } else { h.drag(); h.event(500,500,1,ALT | GROUP_SWITCH); }
    assert.equal(h.moves.length, 0, input);
}
for (const mask of [CTRL, META, CTRL | ALT]) {
    const h = create({ToolsModifiers:mask});
    h.effects.stackingOrder.push(h.toolsProbe);
    h.drag(); h.event(500,500,1,mask);
    assert.equal(h.moves.length, 1);
    assert.equal(h.moves[0].window, h.toolsProbe);
}
{
    const h = create({ToolsEnabled:false});
    h.effects.stackingOrder.push(h.toolsProbe);
    h.drag(); h.event(500,500,1,ALT);
    assert.equal(h.moves.length, 0, 'disabled tools never move a destination');
}
{
    const h = create({ToolsShortcut:'Ctrl+Alt+T', ToolsShortcutAction:TOOLS_ACTION});
    h.effects.stackingOrder.push(h.toolsProbe);
    assert.equal(h.shortcuts.length, 1);
    assert.equal(h.shortcuts[0].action, TOOLS_ACTION);
    h.shortcut(); assert.equal(h.moves.length, 0);
    h.drag(); h.event(500,500,1,ALT); assert.equal(h.moves.length, 0);
    h.shortcut(); assert.equal(h.moves.length, 1);
    assert.equal(h.moves[0].window, h.toolsProbe);
}
{
    const h = create({TriggerMode:'direct', TriggerShortcut:'Ctrl+Alt+M', ShortcutAction:ACTION,
        ToolsShortcut:'Ctrl+Alt+T', ToolsShortcutAction:TOOLS_ACTION});
    h.effects.stackingOrder.push(h.toolsProbe);
    assert.equal(h.shortcuts.length, 2);
    h.drag(); h.shortcut(0); assert.equal(h.moves.length, 0);
    h.shortcut(1); assert.equal(h.moves.length, 1);
    assert.equal(h.moves[0].window, h.toolsProbe, 'tools still require dragging when conversion is direct');
}
for (const caption of ['MaracujaToolsBusy', 'UnrelatedToolsProbe']) {
    const h = create();
    h.toolsProbe.caption = caption;
    h.effects.stackingOrder.push(h.toolsProbe);
    h.drag(); h.event(500,500,1,ALT);
    assert.equal(h.moves.length, 0);
}
{
    const h = create();
    h.effects.stackingOrder.push(h.toolsProbe, {...h.toolsProbe});
    h.drag(); h.event(500,500,1,ALT);
    assert.equal(h.moves.length, 0, 'ambiguous tools destinations fail closed');
}
for (const action of [ACTION, '', 'another-app-action', '_k_session:maracuja-tools-../../other']) {
    const h = create({ToolsShortcut:'F8', ToolsShortcutAction:action});
    assert.equal(h.shortcuts.length, 0, 'tools action cannot collide with conversion or unrelated action');
}
console.log('PASS: Shift padrão, Alt para ferramentas, modificadores e teclas configuráveis, modo direto e isolamento das duas rodas.');

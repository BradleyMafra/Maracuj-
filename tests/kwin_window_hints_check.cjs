// Evaluate the KWin window script with synthetic windows only. No host input,
// compositor, D-Bus service or desktop preference is accessed by this test.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');

const code = fs.readFileSync(path.join(__dirname, '../integration/kwin/window-hints.js'), 'utf8');
const OWN_CLASS = 'org.winux.Mandarina';
const COMPRESS = 'Comprimir · Maracujá';
const METADATA = 'Metadados · Maracujá';
const PHOTO = 'Editar foto · Maracujá';

function signal() {
    const callbacks = [];
    return {
        connect(callback) { callbacks.push(callback); },
        disconnect(callback) {
            const index = callbacks.indexOf(callback);
            if (index !== -1) callbacks.splice(index, 1);
        },
        emit(...args) { for (const callback of [...callbacks]) callback(...args); },
        count() { return callbacks.length; }
    };
}

function window(caption, overrides = {}) {
    let geometry = {x: 100, y: 120, width: 620, height: 680};
    const geometryWrites = [];
    const result = {
        caption, resourceClass: OWN_CLASS, minimized: false,
        captionChanged: signal(), windowClassChanged: signal(), minimizedChanged: signal(),
        frameGeometryChanged: signal(),
        geometryWrites, ...overrides
    };
    if (overrides.frameGeometry) geometry = {...overrides.frameGeometry};
    Object.defineProperty(result, 'frameGeometry', {
        get() { return geometry; },
        set(value) {
            const previous = {...geometry};
            geometry = {x: value.x, y: value.y, width: value.width, height: value.height};
            geometryWrites.push({...geometry});
            result.frameGeometryChanged.emit(previous);
        }
    });
    return result;
}

function create(windows = [], overrides = {}) {
    const original = window('Other editor', {resourceClass: 'other.editor'});
    const area = {x: 0, y: 32, width: 1920, height: 1048};
    const output = {name: 'synthetic-output'};
    const areaRequests = [];
    const screenRequests = [];
    const dbusCalls = [];
    let active = original;
    const activeWrites = [];
    const workspace = {
        cursorPos: {x: 960, y: 540}, currentDesktop: {id: 'synthetic-desktop'},
        windowList() { return windows; },
        screenAt(point) { screenRequests.push({...point}); return output; },
        clientArea(kind, requestedOutput, desktop) {
            areaRequests.push({kind, output: requestedOutput, desktop});
            return kind === 'placement' ? area : {x: 0, y: 0, width: 1920, height: 1080};
        },
        windowAdded: signal(), windowRemoved: signal(), windowActivated: signal(),
        ...overrides
    };
    Object.defineProperty(workspace, 'activeWindow', {
        get() { return active; },
        set(value) { active = value; activeWrites.push(value); }
    });
    vm.runInNewContext(code, {
        workspace, KWin: {ScreenArea: 'screen', PlacementArea: 'placement'},
        callDBus(...args) { dbusCalls.push(args); }
    }, {timeout: 1000});
    function activate(next) {
        active = next;
        workspace.windowActivated.emit(next);
    }
    return {workspace, original, area, output, areaRequests, screenRequests,
        dbusCalls, activeWrites, activate};
}

function checkFlags(w) {
    for (const property of ['noBorder', 'keepAbove', 'skipTaskbar', 'skipPager', 'skipSwitcher'])
        assert.equal(w[property], true, property);
}

// Exact application class and title protect the main window and other apps.
for (const caption of [COMPRESS, METADATA, PHOTO]) {
    const w = window(caption);
    const h = create([w]);
    checkFlags(w);
    assert.deepEqual(w.frameGeometry, {x: 650, y: 200, width: 620, height: 680});
    assert.equal(w.geometryWrites.length, 1);
    assert.equal(h.areaRequests[0].kind, 'placement');
    assert.equal(h.areaRequests[0].output, h.output);
    assert.equal(h.areaRequests[0].desktop, h.workspace.currentDesktop);
    assert.equal(w.minimizedChanged.count(), 0, 'tool windows must not notify probe activation');
    assert.equal(w.frameGeometryChanged.count(), 1);
    h.activate(w);
    assert.equal(h.workspace.activeWindow, w, 'metadata fields retain keyboard focus');
    assert.equal(h.activeWrites.length, 0, 'tool activation must not restore another app');
    assert.equal(h.dbusCalls.length, 0);
}
for (const [caption, resourceClass] of [
    ['Maracujá · Conversor de arquivos', OWN_CLASS],
    ['Comprimir', OWN_CLASS], [COMPRESS + ' extra', OWN_CLASS],
    ['Metadados · Maracujá (outro)', OWN_CLASS],
    [PHOTO + ' extra', OWN_CLASS],
    [COMPRESS, 'org.winux.Mandarina.Other'], [METADATA, 'another.app'], [PHOTO, 'another.app']
]) {
    const w = window(caption, {resourceClass});
    const h = create([w]);
    assert.equal(w.geometryWrites.length, 0);
    assert.equal(w.keepAbove, undefined);
    assert.equal(w.frameGeometryChanged.count(), 0, 'unrelated windows have no resize handler');
    assert.equal(h.areaRequests.length, 0);
    h.activate(w);
    assert.equal(h.workspace.activeWindow, w);
}

// Match delayed captions/classes, but place only once per compositor window.
{
    const w = window('Loading');
    const h = create([w]);
    assert.equal(w.geometryWrites.length, 0);
    w.caption = COMPRESS;
    w.captionChanged.emit();
    checkFlags(w);
    assert.equal(w.geometryWrites.length, 1);
    h.workspace.cursorPos = {x: 1900, y: 1040};
    w.caption = METADATA;
    w.captionChanged.emit();
    w.minimized = true; w.minimizedChanged.emit();
    w.minimized = false; w.minimizedChanged.emit();
    w.windowClassChanged.emit();
    assert.equal(w.geometryWrites.length, 1, 'tab, focus and picker changes must not jump');
    h.workspace.windowRemoved.emit(w);
    assert.equal(w.frameGeometryChanged.count(), 0, 'removal releases the own resize handler');
    h.workspace.windowAdded.emit(w);
    assert.equal(w.geometryWrites.length, 2, 'a reopened native window receives initial placement');
    assert.deepEqual(w.frameGeometry, {x: 1300, y: 400, width: 620, height: 680});
    assert.equal(w.frameGeometryChanged.count(), 1, 'reopening does not duplicate resize callbacks');
}
{
    const w = window(COMPRESS, {resourceClass: ''});
    const h = create([w]);
    assert.equal(w.geometryWrites.length, 0);
    w.resourceClass = OWN_CLASS; w.windowClassChanged.emit();
    assert.equal(w.geometryWrites.length, 1);
}

// Output-local placement handles negative coordinates, panels and small screens.
for (const test of [
    {area: {x: -1920, y: 30, width: 1920, height: 1050}, point: {x: -10, y: 1070},
        want: {x: -620, y: 400, width: 620, height: 680}},
    {area: {x: 1920, y: 48, width: 1280, height: 976}, point: {x: 1930, y: 55},
        want: {x: 1920, y: 48, width: 620, height: 680}},
    {area: {x: 100, y: 40, width: 500, height: 400}, point: {x: 300, y: 200},
        want: {x: 100, y: 40, width: 500, height: 400}}
]) {
    const w = window(METADATA);
    const h = create([w], {
        cursorPos: test.point,
        clientArea(kind) { assert.equal(kind, 'placement'); return test.area; }
    });
    assert.deepEqual(w.frameGeometry, test.want);
    assert.equal(h.dbusCalls.length, 0);
}
for (const overrides of [
    {screenAt() { return null; }},
    {cursorPos: {x: NaN, y: 80}},
    {clientArea() { return {x: 0, y: 0, width: 0, height: 800}; }},
    {clientArea() { return {x: 0, y: Infinity, width: 1200, height: 800}; }}
]) {
    const w = window(COMPRESS);
    create([w], overrides);
    assert.equal(w.geometryWrites.length, 0, 'invalid placement information fails closed');
}

// The compact compression card can grow into metadata options at the lower
// screen edge. Clamp only that size change; never recenter a user-moved card.
{
    const w = window(COMPRESS, {frameGeometry: {x: 20, y: 20, width: 440, height: 430}});
    const h = create([w], {cursorPos: {x: 600, y: 1060}});
    assert.deepEqual(w.frameGeometry, {x: 380, y: 650, width: 440, height: 430});
    const output = {name: 'window-output'};
    w.output = output;
    h.workspace.cursorPos = {x: -1500, y: -1500};
    const requestsBefore = h.areaRequests.length;
    const screensBefore = h.screenRequests.length;
    w.caption = METADATA; w.captionChanged.emit();
    w.frameGeometry = {...w.frameGeometry, height: 500};
    assert.deepEqual(w.frameGeometry, {x: 380, y: 580, width: 440, height: 500});
    assert.equal(h.areaRequests.length, requestsBefore + 1, 'recursive clamping runs only once');
    assert.equal(h.areaRequests.at(-1).output, output, 'resize uses the window output');
    assert.equal(h.screenRequests.length, screensBefore, 'resize does not follow the cursor');
    const writesBeforeMove = w.geometryWrites.length;
    const requestsBeforeMove = h.areaRequests.length;
    w.frameGeometry = {...w.frameGeometry, x: 460, y: 620};
    assert.deepEqual(w.frameGeometry, {x: 460, y: 620, width: 440, height: 500});
    assert.equal(w.geometryWrites.length, writesBeforeMove + 1, 'a pure move is never corrected');
    assert.equal(h.areaRequests.length, requestsBeforeMove);
    w.frameGeometry = {...w.frameGeometry, height: 430};
    assert.deepEqual(w.frameGeometry, {x: 460, y: 620, width: 440, height: 430},
        'shrinking preserves the user position when it already fits');
    assert.equal(h.dbusCalls.length, 0, 'resize never arms a probe');
    assert.equal(h.activeWrites.length, 0, 'resize never changes focus');
}
{
    const w = window(METADATA, {frameGeometry: {x: 50, y: 50, width: 440, height: 430}});
    const h = create([w]);
    const geometry = {...w.frameGeometry, height: 500};
    w.frameGeometry = geometry;
    assert.deepEqual(h.screenRequests.at(-1), {x: geometry.x + 220, y: geometry.y + 250},
        'without an output property, resize falls back to the window center');
    assert.deepEqual(w.frameGeometry, geometry, 'a size change that fits is not repositioned');
}

// Both conversion and tools probes retain their size, D-Bus arm
// messages and return-to-source focus. Tools options never enter this branch.
for (const [caption, method] of [
    ['MaracujaShiftProbe', 'ProbeOpened'], ['MaracujaShiftBusy', 'ProbeOpened'],
    ['MaracujaToolsProbe', 'ToolsProbeOpened'], ['MaracujaToolsBusy', 'ToolsProbeOpened']
]) {
    const probe = window(caption, {minimized: true});
    const h = create([probe]);
    checkFlags(probe);
    assert.equal(probe.frameGeometryChanged.count(), 0, 'probes do not use tool resize clamping');
    assert.equal(probe.geometryWrites.length, 0);
    probe.minimized = false; probe.minimizedChanged.emit();
    assert.deepEqual(probe.frameGeometry, {x: 730, y: 310, width: 460, height: 460});
    assert.equal(h.areaRequests[0].kind, 'screen');
    assert.deepEqual(h.dbusCalls[0], ['org.winux.Mandarina', '/Mandarina', 'org.winux.Mandarina', method]);
    h.activate(probe);
    assert.equal(h.workspace.activeWindow, h.original);
    const metadata = window(METADATA);
    h.workspace.windowAdded.emit(metadata);
    h.activate(metadata);
    h.activate(probe);
    assert.equal(h.workspace.activeWindow, metadata, 'focus may return to editable tool options');
    h.workspace.windowRemoved.emit(metadata);
    h.activate(probe);
    assert.equal(h.workspace.activeWindow, probe, 'a removed previous window is not reactivated');
}

// Native size preferences survive probe placement on each output. Centering,
// edge clamping and focus restoration use the actual 70% and 130% extents.
for (const [caption, method] of [
    ['MaracujaShiftProbe', 'ProbeOpened'], ['MaracujaToolsProbe', 'ToolsProbeOpened']
]) {
    for (const extent of [322, 598]) {
        for (const test of [
            {area: {x: -1920, y: -1080, width: 1920, height: 1080}, point: {x: -1900, y: -1070},
                position: {x: -1920, y: -1080}},
            {area: {x: -1920, y: 0, width: 1920, height: 1080}, point: {x: -10, y: 1070},
                position: {x: -extent, y: 1080 - extent}},
            {area: {x: 1920, y: 0, width: 1280, height: 1024}, point: {x: 2560, y: 512},
                position: {x: 2560 - extent / 2, y: 512 - extent / 2}},
            {area: {x: 100, y: 40, width: 300, height: 280}, point: {x: 250, y: 180},
                position: {x: 100, y: 40}}
        ]) {
            const originalGeometry = {x: 50, y: 60, width: extent, height: extent};
            const probe = window(caption, {minimized: true, frameGeometry: originalGeometry});
            const output = {name: `output-${test.area.x}-${test.area.y}`};
            const h = create([probe], {
                cursorPos: test.point,
                screenAt(point) { assert.deepEqual({...point}, test.point); return output; },
                clientArea(kind, selectedOutput) {
                    assert.equal(kind, 'screen');
                    assert.equal(selectedOutput, output);
                    return test.area;
                }
            });
            probe.minimized = false; probe.minimizedChanged.emit();
            assert.deepEqual(probe.frameGeometry, {...test.position, width: extent, height: extent});
            assert.equal(probe.geometryWrites.length, 1);
            assert.deepEqual(h.original.frameGeometry, {x: 100, y: 120, width: 620, height: 680});
            assert.equal(h.original.geometryWrites.length, 0, 'source window is never moved');
            assert.deepEqual(h.dbusCalls[0], ['org.winux.Mandarina', '/Mandarina', 'org.winux.Mandarina', method]);
            assert.equal(h.activeWrites.length, 0, 'positioning does not activate the probe');
            h.activate(probe);
            assert.equal(h.workspace.activeWindow, h.original);
        }
    }
}
for (const dimensions of [
    {width: 321, height: 321}, {width: 599, height: 599},
    {width: 322, height: 323}, {width: Infinity, height: 460},
    {width: 460, height: NaN}, {width: 0, height: 0}
]) {
    const probe = window('MaracujaShiftProbe', {
        minimized: true, frameGeometry: {x: 30, y: 40, ...dimensions}
    });
    create([probe]);
    probe.minimized = false; probe.minimizedChanged.emit();
    assert.deepEqual(probe.frameGeometry, {x: 730, y: 310, width: 460, height: 460},
        'initial or invalid server geometry falls back to the default extent');
}
for (const caption of ['MaracujaShiftProbe', 'MaracujaToolsProbe']) {
    const geometry = {x: -1700, y: 10, width: 598, height: 598};
    const unrelated = window(caption, {resourceClass: 'another.app', minimized: true, frameGeometry: geometry});
    const h = create([unrelated]);
    unrelated.minimized = false; unrelated.minimizedChanged.emit();
    assert.deepEqual(unrelated.frameGeometry, geometry);
    assert.equal(unrelated.geometryWrites.length, 0);
    assert.equal(unrelated.keepAbove, undefined);
    assert.equal(h.dbusCalls.length, 0, 'unowned scaled windows never arm a probe');
    assert.equal(h.areaRequests.length, 0);
}

console.log('KWin file-tool window placement and probe isolation checks passed.');

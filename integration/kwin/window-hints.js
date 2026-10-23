// Applies only to Maracujá's own drag targets and file-tool windows.
// No settings are persisted.
(function () {
    var previous = workspace.activeWindow;
    var prepared = [];
    var positionedTools = [];
    function ownProbe(w) {
        return w && String(w.resourceClass) === "org.winux.Mandarina"
            && (w.caption === "MaracujaShiftProbe" || w.caption === "MaracujaShiftBusy"
                || w.caption === "MaracujaToolsProbe" || w.caption === "MaracujaToolsBusy");
    }
    function ownTool(w) {
        return w && String(w.resourceClass) === "org.winux.Mandarina"
            && (w.caption === "Comprimir · Maracujá" || w.caption === "Metadados · Maracujá"
                || w.caption === "Editar foto · Maracujá");
    }
    function toolState(w) {
        for (var index = 0; index < positionedTools.length; ++index) {
            if (positionedTools[index].window === w) return positionedTools[index];
        }
        return null;
    }
    function clampProbe(w) {
        if (!ownProbe(w)) return;
        // Scripted effects do not expose screenAt/clientArea to JavaScript.
        // The regular KWin script does, in logical coordinates on each output.
        var pointer = workspace.cursorPos;
        var output = workspace.screenAt(pointer);
        if (!output) return;
        var area = workspace.clientArea(KWin.ScreenArea, output, workspace.currentDesktop);
        var left = Number(area.x);
        var top = Number(area.y);
        var width = Number(area.width);
        var height = Number(area.height);
        var pointerX = Number(pointer.x);
        var pointerY = Number(pointer.y);
        if (!isFinite(left) || !isFinite(top) || !isFinite(width) || !isFinite(height)
                || !isFinite(pointerX) || !isFinite(pointerY) || width <= 0 || height <= 0) return;
        // Match the helper's actual input/window extent, not its maximum SHM
        // buffer. Keep its scale intact while positioning inside this output.
        var geometry = w.frameGeometry;
        var probeWidth = Number(geometry.width);
        var probeHeight = Number(geometry.height);
        if (!isFinite(probeWidth) || !isFinite(probeHeight) || probeWidth !== probeHeight
                || probeWidth < 322 || probeWidth > 598) {
            probeWidth = probeHeight = 460;
        }
        var x = Math.max(left, Math.min(pointerX - probeWidth / 2,
            left + Math.max(0, width - probeWidth)));
        var y = Math.max(top, Math.min(pointerY - probeHeight / 2,
            top + Math.max(0, height - probeHeight)));
        w.frameGeometry = {x: x, y: y, width: probeWidth, height: probeHeight};
    }
    function clampToolResize(w, state) {
        if (state.adjusting || !ownTool(w) || toolState(w) !== state) return;
        var geometry = w.frameGeometry;
        var width = Number(geometry.width);
        var height = Number(geometry.height);
        var oldX = Number(geometry.x);
        var oldY = Number(geometry.y);
        if (!isFinite(width) || !isFinite(height) || !isFinite(oldX) || !isFinite(oldY)
                || width <= 0 || height <= 0 || (width === state.width && height === state.height)) return;
        state.width = width;
        state.height = height;
        // A move belongs to the user. Only a size change can bring an edge
        // back inside this window's output, without following the cursor.
        var output = w.output || workspace.screenAt({x: oldX + width / 2, y: oldY + height / 2});
        if (!output) return;
        var area = workspace.clientArea(KWin.PlacementArea, output, workspace.currentDesktop);
        var left = Number(area.x);
        var top = Number(area.y);
        var availableWidth = Number(area.width);
        var availableHeight = Number(area.height);
        if (!isFinite(left) || !isFinite(top) || !isFinite(availableWidth)
                || !isFinite(availableHeight) || availableWidth <= 0 || availableHeight <= 0) return;
        width = Math.min(width, availableWidth);
        height = Math.min(height, availableHeight);
        var x = Math.max(left, Math.min(oldX, left + availableWidth - width));
        var y = Math.max(top, Math.min(oldY, top + availableHeight - height));
        if (x === oldX && y === oldY && width === state.width && height === state.height) return;
        state.width = width;
        state.height = height;
        state.adjusting = true;
        try {
            w.frameGeometry = {x: x, y: y, width: width, height: height};
        } finally {
            state.adjusting = false;
        }
    }
    function placeTool(w) {
        if (toolState(w)) return;
        var pointer = workspace.cursorPos;
        var output = workspace.screenAt(pointer);
        if (!output) return;
        // PlacementArea is this output's usable area, excluding panels.
        // WorkArea can span multiple outputs; ScreenArea also includes panels.
        var area = workspace.clientArea(KWin.PlacementArea, output, workspace.currentDesktop);
        var left = Number(area.x);
        var top = Number(area.y);
        var availableWidth = Number(area.width);
        var availableHeight = Number(area.height);
        var pointerX = Number(pointer.x);
        var pointerY = Number(pointer.y);
        if (!isFinite(left) || !isFinite(top) || !isFinite(availableWidth)
                || !isFinite(availableHeight) || !isFinite(pointerX) || !isFinite(pointerY)
                || availableWidth <= 0 || availableHeight <= 0) return;
        var geometry = w.frameGeometry;
        var width = Number(geometry.width);
        var height = Number(geometry.height);
        if (!isFinite(width) || !isFinite(height) || width <= 0 || height <= 0) return;
        width = Math.min(width, availableWidth);
        height = Math.min(height, availableHeight);
        var x = Math.max(left, Math.min(pointerX - width / 2, left + availableWidth - width));
        var y = Math.max(top, Math.min(pointerY - height / 2, top + availableHeight - height));
        // Only initial placement follows the pointer. A later height change
        // may clamp an overflowing edge, but editing or moving does not.
        var state = {window: w, width: width, height: height, adjusting: true};
        positionedTools.push(state);
        if (w.frameGeometryChanged) {
            state.geometryChanged = function () { clampToolResize(w, state); };
            w.frameGeometryChanged.connect(state.geometryChanged);
        }
        try {
            w.frameGeometry = {x: x, y: y, width: width, height: height};
        } finally {
            state.adjusting = false;
        }
    }
    function prepare(w) {
        if (!ownProbe(w) && !ownTool(w)) return;
        w.noBorder = true;
        w.keepAbove = true;
        w.skipTaskbar = true;
        w.skipPager = true;
        w.skipSwitcher = true;
        if (ownTool(w)) {
            placeTool(w);
            return;
        }
        if (prepared.indexOf(w) === -1) {
            prepared.push(w);
            w.minimizedChanged.connect(function () {
                if (!w.minimized && ownProbe(w)) {
                    clampProbe(w);
                    callDBus("org.winux.Mandarina", "/Mandarina", "org.winux.Mandarina",
                        w.caption.indexOf("MaracujaTools") === 0 ? "ToolsProbeOpened" : "ProbeOpened");
                }
            });
        }
    }
    function watch(w) {
        prepare(w);
        w.captionChanged.connect(function () { prepare(w); });
        if (w.windowClassChanged) w.windowClassChanged.connect(function () { prepare(w); });
    }
    workspace.windowList().forEach(watch);
    workspace.windowAdded.connect(watch);
    workspace.windowRemoved.connect(function (w) {
        if (previous === w) previous = null;
        var index = prepared.indexOf(w);
        if (index !== -1) prepared.splice(index, 1);
        var state = toolState(w);
        if (state) {
            if (state.geometryChanged) w.frameGeometryChanged.disconnect(state.geometryChanged);
            positionedTools.splice(positionedTools.indexOf(state), 1);
        }
    });
    workspace.windowActivated.connect(function (w) {
        if (ownProbe(w)) {
            if (previous) workspace.activeWindow = previous;
        } else previous = w;
    });
}());

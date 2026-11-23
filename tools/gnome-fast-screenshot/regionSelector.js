import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {freezeMonitor, snapshotActor, snapshotAt} from './desktopSnapshot.js';

const DRAG_DISTANCE = 4;
const SHADE = 'background-color: rgba(0, 0, 0, 0.45);';

function monitorAt(monitors, x, y) {
    return monitors.find(monitor => x >= monitor.x && y >= monitor.y &&
        x < monitor.x + monitor.width && y < monitor.y + monitor.height) ?? null;
}

function dragArea(monitor, start, x, y) {
    // A drag stays on the output where it began, even when the pointer crosses an edge.
    const endX = Math.min(Math.max(x, monitor.x), monitor.x + monitor.width);
    const endY = Math.min(Math.max(y, monitor.y), monitor.y + monitor.height);
    const left = Math.floor(Math.min(start.x, endX));
    const top = Math.floor(Math.min(start.y, endY));
    const right = Math.max(left + 1, Math.ceil(Math.max(start.x, endX)));
    const bottom = Math.max(top + 1, Math.ceil(Math.max(start.y, endY)));
    return {x: left, y: top, width: right - left, height: bottom - top};
}

function place(actor, x, y, width, height) {
    actor.set_position(x, y);
    actor.set_size(Math.max(0, width), Math.max(0, height));
}

function sameArea(a, b) {
    return a === b || (a && b && a.x === b.x && a.y === b.y &&
        a.width === b.width && a.height === b.height);
}

export class RegionSelector {
    constructor(ownerPid = null, prepareMonitor = null, cancelled = null) {
        this._ownerPid = ownerPid;
        this._prepareMonitor = prepareMonitor;
        this._cancelled = cancelled;
    }

    select() {
        if (this._operation)
            return Promise.reject(new Error('A SnapLing selection is already open'));

        if (Main.sessionMode.isLocked || Main.sessionMode.isGreeter || Main.screenshotUI.visible)
            return Promise.reject(new Error('The desktop is not available for SnapLing selection'));

        return new Promise((resolve, reject) => {
            const operation = {resolve, reject, start: null, dragged: false, cancelled: false};
            this._operation = operation;
            this._freeze(operation);
        });
    }

    cancel() {
        if (this._operation)
            this._finish(this._operation, null);
        if (this._preview?.grab)
            Main.popModal(this._preview.grab);
        if (this._preview?.displayCursorChanged)
            global.display.set_cursor(Meta.Cursor.DEFAULT);
        this._preview?.overlay.destroy();
        this._preview = null;
    }

    setEditorBackdrop(actor, monitor) {
        const snapshot = this._preview?.snapshot;
        if (!snapshot || !actor.get_last_child())
            return null;

        // Mutter supplies a black backdrop to translucent fullscreen surfaces.
        // Keep the frozen pixels on the compositor instead of sending another image.
        const transform = actor.get_child_transform();
        const backdrop = new Clutter.Actor({clip_to_allocation: true,
            width: monitor.width, height: monitor.height,
            scale_x: 1 / transform.get_value(0, 0), scale_y: 1 / transform.get_value(1, 1),
        });
        backdrop.add_child(snapshotActor(snapshot, monitor.x, monitor.y));
        actor.insert_child_below(backdrop, actor.get_last_child());
        return backdrop;
    }

    async _freeze(operation) {
        try {
            operation.monitors = Main.layoutManager.monitors.map(({x, y, width, height}) => ({x, y, width, height}));
            if (operation.monitors.length === 0)
                throw new Error('No monitor is available for SnapLing selection');
            operation.startedAt = GLib.get_monotonic_time();
            operation.snapshots = [];
            operation.hasSnapshot = false;
            // Qt hide() and the compositor's fade-out are asynchronous. Mask every actor
            // owned by the caller before painting the frozen desktop so neither a stale
            // surface nor a semi-transparent closing window becomes part of the snapshot.
            operation.hiddenActors = this._hideOwnerActors();
            this._show(operation);

            // The transparent selector and input grab are visible before the first
            // expensive compositor texture is read. Capture the pointer monitor first,
            // then freeze the other outputs one by one so three screens do not serialize
            // into one long blank interval.
            await this._delay(16);
            if (this._operation !== operation || operation.cancelled)
                return;
            const [pointerX, pointerY] = global.get_pointer();
            const primary = monitorAt(operation.monitors, pointerX, pointerY) ?? operation.monitors[0];
            const ordered = [primary, ...operation.monitors.filter(monitor => monitor !== primary)];
            for (let index = 0; index < ordered.length; ++index) {
                if (this._operation !== operation || operation.cancelled)
                    return;
                operation.hiddenActors.push(...this._hideOwnerActors());
                // After the first output is visible its shades are part of the stage.
                // Temporarily remove the selector before freezing later outputs so the
                // dark selection mask is not baked into their frozen textures.
                const captureLayers = [operation.snapshotLayer, operation.dim,
                    operation.selectionPatch, operation.border, ...operation.handles];
                const visibleLayers = index > 0 ? captureLayers.filter(actor => actor.visible) : [];
                for (const actor of visibleLayers)
                    actor.hide();
                let snapshots;
                try {
                    snapshots = await freezeMonitor(ordered[index]);
                } finally {
                    if (this._operation === operation && !operation.cancelled)
                        for (const actor of visibleLayers)
                            actor.show();
                }
                if (this._operation !== operation || operation.cancelled)
                    return;
                for (const snapshot of snapshots) {
                    operation.snapshots.push(snapshot);
                    operation.snapshotLayer.add_child(snapshotActor(snapshot));
                }
                operation.hasSnapshot = true;
                operation.paintedArea = undefined;
                const [x, y] = global.get_pointer();
                this._paint(operation, monitorAt(operation.monitors, x, y));
                this._applyPendingSelection(operation);
                if (index === 0) {
                    const elapsedMs = (GLib.get_monotonic_time() - operation.startedAt) / 1000;
                    console.log(`SnapLing frozen pointer monitor in ${elapsedMs.toFixed(1)} ms`);
                }
                if (this._operation !== operation ||
                    operation.monitors.every(monitor => snapshotAt(operation.snapshots, monitor)))
                    break;
                if (index + 1 < ordered.length)
                    await this._delay(0);
            }
        } catch (error) {
            this._finish(operation, null, error);
        }
    }

    _delay(milliseconds) {
        return new Promise(resolve => {
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, milliseconds, () => {
                resolve();
                return GLib.SOURCE_REMOVE;
            });
        });
    }

    _hideOwnerActors() {
        if (this._ownerPid === null)
            return [];
        const hidden = [];
        for (const actor of global.get_window_actors()) {
            const window = actor.meta_window;
            if (!window || window.get_pid() !== this._ownerPid || !actor.visible)
                continue;
            hidden.push(actor);
            actor.hide();
        }
        return hidden;
    }

    _restoreOwnerActors(operation) {
        for (const actor of operation.hiddenActors ?? []) {
            try {
                // Qt may have completed its own hide while the selector was open. Only
                // restore actors whose window is still logically shown.
                if (actor.meta_window?.showing_on_its_workspace?.())
                    actor.show();
            } catch {
                // The actor can be destroyed when the owning Wayland surface unmaps.
            }
        }
        operation.hiddenActors = [];
    }

    _show(operation) {
        if (Main.screenshotUI.visible)
            throw new Error('The desktop is not available for SnapLing selection');

        const overlay = new St.Widget({
            name: 'snapling-region-selector',
            reactive: true,
            can_focus: true,
        });
        overlay.set_size(global.stage.width, global.stage.height);
        operation.overlay = overlay;
        operation.snapshotLayer = new Clutter.Actor({
            width: global.stage.width,
            height: global.stage.height,
        });
        overlay.add_child(operation.snapshotLayer);

        operation.dim = new St.Widget({style: SHADE});
        operation.dim.set_size(global.stage.width, global.stage.height);
        operation.selectionPatch = new Clutter.Actor({clip_to_allocation: true});
        operation.border = new St.Widget({style: 'border: 2px solid #0bd18b;'});
        operation.handles = Array.from({length: 8}, () => new St.Widget({
            style: 'background-color:#0bd18b; border:1px solid #123c30;',
        }));
        for (const actor of [operation.dim, operation.selectionPatch, operation.border, ...operation.handles]) {
            actor.hide();
            overlay.add_child(actor);
        }

        overlay.connect('captured-event', (_actor, event) => this._event(operation, event));

        // The screenshot is already in a compositor texture. Dismiss desktop popups
        // before grabbing input so their grabs cannot interfere with the selection.
        Main.layoutManager.emit('system-modal-opened');
        const group = Main.layoutManager.screenshotUIGroup;
        group.get_parent().set_child_above_sibling(group, null);
        group.add_child(overlay);
        operation.grab = Main.pushModal(overlay, {actionMode: Shell.ActionMode.POPUP});
        if (operation.grab.get_seat_state && operation.grab.get_seat_state() !== Clutter.GrabState.ALL)
            throw new Error('GNOME could not grab input for SnapLing selection');

        if (typeof overlay.set_cursor_type === 'function') {
            overlay.set_cursor_type(Clutter.CursorType.CROSSHAIR);
        } else {
            global.display.set_cursor(Meta.Cursor.CROSSHAIR);
            operation.displayCursorChanged = true;
        }
    }

    _paint(operation, area) {
        if (sameArea(operation.paintedArea, area))
            return;
        if (!operation.hasSnapshot)
            return;
        operation.paintedArea = area;
        const {width, height} = operation.overlay;
        operation.dim.show();
        if (!area) {
            operation.selectionPatch.hide();
            operation.border.hide();
            return;
        }
        const monitor = monitorAt(operation.monitors, area.x + 1, area.y + 1);
        const snapshot = monitor ? snapshotAt(operation.snapshots, monitor) : null;
        if (snapshot) {
            this._prepareMonitor?.(snapshot, monitor);
            place(operation.selectionPatch, area.x, area.y, area.width, area.height);
            if (operation.patchSnapshot !== snapshot) {
                operation.selectionPatch.destroy_all_children();
                operation.patchContent = snapshotActor(snapshot);
                operation.selectionPatch.add_child(operation.patchContent);
                operation.patchSnapshot = snapshot;
            }
            operation.patchContent.set_position(snapshot.bounds.x - area.x, snapshot.bounds.y - area.y);
            operation.selectionPatch.show();
        } else {
            operation.selectionPatch.hide();
        }
        place(operation.border, area.x, area.y, area.width, area.height);
        operation.border.show();
    }

    _paintFinalSelection(operation, area) {
        const left = Math.max(1, area.x - 1);
        const top = Math.max(1, area.y - 1);
        const right = Math.min(operation.overlay.width - 1, area.x + area.width + 1);
        const bottom = Math.min(operation.overlay.height - 1, area.y + area.height + 1);
        place(operation.border, left, top, right - left, bottom - top);
        const cx = area.x + area.width / 2;
        const cy = area.y + area.height / 2;
        const rightEdge = area.x + area.width;
        const bottomEdge = area.y + area.height;
        const points = [[area.x, area.y], [cx, area.y], [rightEdge, area.y],
            [area.x, cy], [rightEdge, cy], [area.x, bottomEdge], [cx, bottomEdge], [rightEdge, bottomEdge]];
        operation.handles.forEach((handle, index) => {
            place(handle, points[index][0] - 4, points[index][1] - 4, 8, 8);
            handle.show();
        });
    }

    _queuePaint(operation, area) {
        operation.nextArea = area;
        if (operation.paintLater || sameArea(operation.paintedArea, area))
            return;
        operation.paintLater = global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, () => {
            operation.paintLater = 0;
            this._paint(operation, operation.nextArea);
            return false;
        });
    }

    _event(operation, event) {
        if (operation.cancelled) {
            if ((event.type() === Clutter.EventType.KEY_PRESS && event.get_key_symbol() === Clutter.KEY_Escape) ||
                (event.type() === Clutter.EventType.BUTTON_PRESS && event.get_button() === Clutter.BUTTON_SECONDARY))
                this._cancelled?.();
            return event.type() === Clutter.EventType.ENTER || event.type() === Clutter.EventType.LEAVE
                ? Clutter.EVENT_PROPAGATE : Clutter.EVENT_STOP;
        }
        switch (event.type()) {
        case Clutter.EventType.ENTER:
        case Clutter.EventType.LEAVE:
            return Clutter.EVENT_PROPAGATE;
        case Clutter.EventType.KEY_PRESS:
            if (event.get_key_symbol() === Clutter.KEY_Escape)
                this._finish(operation, null);
            break;
        case Clutter.EventType.BUTTON_PRESS: {
            if (event.get_button() === Clutter.BUTTON_SECONDARY) {
                this._finish(operation, null);
                break;
            }
            if (event.get_button() !== Clutter.BUTTON_PRIMARY || operation.start)
                break;
            const [x, y] = event.get_coords();
            operation.monitor = monitorAt(operation.monitors, x, y);
            if (operation.monitor)
                operation.start = {x, y};
            break;
        }
        case Clutter.EventType.MOTION: {
            const [x, y] = event.get_coords();
            if (!operation.pendingSelection)
                this._queuePaint(operation, this._areaAt(operation, x, y));
            break;
        }
        case Clutter.EventType.BUTTON_RELEASE: {
            if (event.get_button() !== Clutter.BUTTON_PRIMARY || !operation.start)
                break;
            const [x, y] = event.get_coords();
            const area = this._areaAt(operation, x, y);
            const snapshot = snapshotAt(operation.snapshots, operation.monitor);
            this._paint(operation, area);
            this._paintFinalSelection(operation, area);
            if (snapshot)
                this._finish(operation, {snapshot, area, monitor: operation.monitor});
            else
                operation.pendingSelection = {area, monitor: operation.monitor};
            break;
        }
        }
        return Clutter.EVENT_STOP;
    }

    _areaAt(operation, x, y) {
        if (!operation.start)
            return monitorAt(operation.monitors, x, y);
        const {start, monitor} = operation;
        operation.dragged ||= Math.hypot(x - start.x, y - start.y) >= DRAG_DISTANCE;
        return operation.dragged ? dragArea(monitor, start, x, y) : monitor;
    }

    _applyPendingSelection(operation) {
        const pending = operation.pendingSelection;
        if (!pending)
            return;
        const snapshot = snapshotAt(operation.snapshots, pending.monitor);
        if (!snapshot)
            return;
        operation.pendingSelection = null;
        this._paint(operation, pending.area);
        this._finish(operation, {snapshot, area: pending.area, monitor: pending.monitor});
    }

    _finish(operation, result, error = null) {
        if (this._operation !== operation)
            return;
        this._operation = null;
        operation.cancelled = true;
        this._restoreOwnerActors(operation);

        if (result) {
            this._paint(operation, result.area);
            this._paintFinalSelection(operation, result.area);
        }
        if (operation.paintLater)
            global.compositor.get_laters().remove(operation.paintLater);
        if (operation.grab && !result)
            Main.popModal(operation.grab);
        if (operation.displayCursorChanged && !result)
            global.display.set_cursor(Meta.Cursor.DEFAULT);
        if (result) {
            this._preview = {overlay: operation.overlay, snapshot: result.snapshot,
                grab: operation.grab, displayCursorChanged: operation.displayCursorChanged};
        } else {
            operation.overlay?.destroy();
        }
        if (error)
            operation.reject(error);
        else
            operation.resolve(result);
    }
}

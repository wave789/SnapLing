import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {freezeDesktop, snapshotActor, snapshotAt} from './desktopSnapshot.js';

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
    constructor(ownerPid = null) {
        this._ownerPid = ownerPid;
    }

    select() {
        if (this._operation)
            return Promise.reject(new Error('A SnapLing selection is already open'));

        if (Main.sessionMode.isLocked || Main.sessionMode.isGreeter || Main.screenshotUI.visible)
            return Promise.reject(new Error('The desktop is not available for SnapLing selection'));

        return new Promise((resolve, reject) => {
            const operation = {resolve, reject, start: null, dragged: false};
            this._operation = operation;
            this._freeze(operation);
        });
    }

    cancel() {
        if (this._operation)
            this._finish(this._operation, null);
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
            // Qt hide() and the compositor's fade-out are asynchronous. Mask every actor
            // owned by the caller before painting the frozen desktop so neither a stale
            // surface nor a semi-transparent closing window becomes part of the snapshot.
            operation.hiddenActors = this._hideOwnerActors();
            operation.snapshots = await freezeDesktop(operation.monitors);
            if (this._operation !== operation)
                return;

            this._show(operation);
        } catch (error) {
            this._finish(operation, null, error);
        }
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
        for (const snapshot of operation.snapshots)
            overlay.add_child(snapshotActor(snapshot));

        operation.shades = Array.from({length: 4}, () => new St.Widget({style: SHADE}));
        operation.border = new St.Widget({style: 'border: 2px solid #0bd18b;'});
        for (const actor of [...operation.shades, operation.border])
            overlay.add_child(actor);

        const [x, y] = global.get_pointer();
        this._paint(operation, monitorAt(operation.monitors, x, y));
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
        operation.paintedArea = area;
        const [top, bottom, left, right] = operation.shades;
        const {width, height} = operation.overlay;
        if (!area) {
            place(top, 0, 0, width, height);
            for (const shade of [bottom, left, right])
                shade.hide();
            operation.border.hide();
            return;
        }
        place(top, 0, 0, width, area.y);
        place(bottom, 0, area.y + area.height, width, height - area.y - area.height);
        place(left, 0, area.y, area.x, area.height);
        place(right, area.x + area.width, area.y, width - area.x - area.width, area.height);
        for (const shade of [bottom, left, right])
            shade.show();
        place(operation.border, area.x, area.y, area.width, area.height);
        operation.border.show();
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
            if (snapshot)
                this._finish(operation, {snapshot, area, monitor: operation.monitor});
            else
                this._finish(operation, null, new Error('No frozen image covers the selection'));
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

    _finish(operation, result, error = null) {
        if (this._operation !== operation)
            return;
        this._operation = null;
        this._restoreOwnerActors(operation);

        if (operation.paintLater)
            global.compositor.get_laters().remove(operation.paintLater);
        if (operation.grab)
            Main.popModal(operation.grab);
        if (operation.displayCursorChanged)
            global.display.set_cursor(Meta.Cursor.DEFAULT);
        if (result) {
            operation.overlay.reactive = false;
            this._preview = {overlay: operation.overlay, snapshot: result.snapshot};
        } else {
            operation.overlay?.destroy();
        }
        if (error)
            operation.reject(error);
        else
            operation.resolve(result);
    }
}

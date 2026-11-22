import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {RegionSelector} from './regionSelector.js';

const PATH = '/io/snapling/Screenshot';
const IFACE = 'io.snapling.Screenshot1';
const XML = `<node>
  <interface name="${IFACE}">
    <method name="GetVersion"><arg type="u" direction="out"/></method>
    <method name="SelectRegion">
      <arg name="png" type="h" direction="out"/>
      <arg name="monitor_x" type="i" direction="out"/>
      <arg name="monitor_y" type="i" direction="out"/>
      <arg name="monitor_width" type="i" direction="out"/>
      <arg name="monitor_height" type="i" direction="out"/>
      <arg name="selection_x" type="i" direction="out"/>
      <arg name="selection_y" type="i" direction="out"/>
      <arg name="selection_width" type="i" direction="out"/>
      <arg name="selection_height" type="i" direction="out"/>
    </method>
    <method name="Cancel"/>
  </interface>
</node>`;

function callerPid(invocation) {
    return new Promise((resolve, reject) => {
        invocation.get_connection().call(
            'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetConnectionUnixProcessID', new GLib.Variant('(s)', [invocation.get_sender()]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, 2000, null,
            (connection, result) => {
                try {
                    resolve(connection.call_finish(result).deep_unpack()[0]);
                } catch (error) {
                    reject(error);
                }
            });
    });
}

function allowedExecutables(extensionPath) {
    const prefix = GLib.canonicalize_filename('../../../..', extensionPath);
    return new Set([
        GLib.build_filenamev([prefix, 'bin', 'snapling']),
        '/usr/bin/snapling',
        GLib.build_filenamev([GLib.get_home_dir(), '.local', 'snapling', 'bin', 'snapling']),
    ]);
}

function canCapture() {
    return !Main.sessionMode.isLocked && !Main.sessionMode.isGreeter &&
        typeof Shell.Screenshot.prototype.screenshot_stage_to_content === 'function' &&
        typeof Shell.Screenshot.composite_to_stream === 'function';
}

async function writeMonitor(selection, stream) {
    const {snapshot: {bounds: source, content, scale}, area, monitor} = selection;
    const localX = area.x - monitor.x;
    const localY = area.y - monitor.y;
    const bounds = [monitor.x, monitor.y, monitor.width, monitor.height,
        localX, localY, area.width, area.height];
    if (!bounds.every(Number.isInteger) || !Number.isFinite(scale) || scale <= 0)
        throw new Error('invalid selection');

    const texture = content.get_texture();
    // Keep the complete monitor so the Qt layer can move or enlarge the initial
    // selection without asking the compositor for a second frozen frame.
    const x = Math.round((monitor.x - source.x) * scale);
    const y = Math.round((monitor.y - source.y) * scale);
    const w = Math.round((monitor.x + monitor.width - source.x) * scale) - x;
    const h = Math.round((monitor.y + monitor.height - source.y) * scale) - y;
    if (localX < 0 || localY < 0 || area.width <= 0 || area.height <= 0 ||
        localX + area.width > monitor.width || localY + area.height > monitor.height ||
        x < 0 || y < 0 || w <= 0 || h <= 0 || w * h > 100_000_000 ||
        x + w > texture.get_width() || y + h > texture.get_height())
        throw new Error('invalid selection bounds');

    // The cursor stays live in the selector and is not included in the monitor texture.
    await Shell.Screenshot.composite_to_stream(texture, x, y, w, h, scale,
        null, 0, 0, 1, stream);
    stream.flush(null);
    return bounds;
}

export class ScreenshotBridge {
    constructor(extensionPath) {
        this._allowed = allowedExecutables(extensionPath);
        this._pending = null;
        this._dbus = Gio.DBusExportedObject.wrapJSObject(XML, this);
        this._dbus.export(Gio.DBus.session, PATH);
        this._sessionChanged = Main.sessionMode.connect('updated', () => {
            if (!canCapture()) {
                this._cancel(this._pending);
                this._clearHandoff();
            }
        });
        this._monitorsChanged = Main.layoutManager.connect('monitors-changed', () => {
            this._cancel(this._pending);
            this._clearHandoff();
        });
        this._windowMapped = global.window_manager.connect('map', (_manager, actor) => this._editorMapped(actor));
        this._windowMoved = global.display.connect('window-entered-monitor', (_display, _index, window) =>
            this._editorMapped(window.get_compositor_private()));
    }

    GetVersion() {
        return 3;
    }

    async SelectRegionAsync(_params, invocation) {
        if (!this._dbus || !canCapture()) {
            invocation.return_dbus_error(`${IFACE}.Unsupported`, 'Screenshot selection is unavailable in this session');
            return;
        }
        if (this._pending || this._handoff) {
            invocation.return_dbus_error(`${IFACE}.Busy`, 'A selection is already in progress');
            return;
        }

        const request = {sender: invocation.get_sender(), selector: null, cancelled: false};
        this._pending = request;
        let io;
        try {
            let executable;
            let pid;
            try {
                pid = await callerPid(invocation);
                executable = GLib.file_read_link(`/proc/${pid}/exe`);
            } catch (error) {
                executable = null;
            }
            if (request.cancelled) {
                this._replyCancelled(invocation);
                return;
            }
            // Compare exact paths obtained from the bus sender's process, not a client-supplied ID.
            if (!this._allowed.has(executable)) {
                invocation.return_dbus_error(`${IFACE}.NotAllowed`, 'Only the installed SnapLing executable can request a selection');
                return;
            }

            request.watch = Gio.bus_watch_name_on_connection(invocation.get_connection(),
                request.sender, Gio.BusNameWatcherFlags.NONE, null, () => this._cancel(request));
            request.timeout = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 90, () => {
                request.timeout = 0;
                this._cancel(request);
                return GLib.SOURCE_REMOVE;
            });
            request.selector = new RegionSelector(pid);
            const selection = await request.selector.select();
            if (!selection || request.cancelled) {
                this._replyCancelled(invocation);
                return;
            }

            // Unlink before writing any pixels. The only handoff is the private bus reply's FD.
            const [file, stream] = Gio.File.new_tmp('snapling-selected-XXXXXX');
            io = stream;
            file.delete(null);
            const bounds = await writeMonitor(selection, io.get_output_stream());
            if (request.cancelled) {
                this._replyCancelled(invocation);
                return;
            }
            io.seek(0, GLib.SeekType.SET, null);
            const fds = Gio.UnixFDList.new();
            const handle = fds.append(io.get_input_stream().get_fd());
            this._startHandoff(request, pid, selection.monitor);
            invocation.return_value_with_unix_fd_list(
                new GLib.Variant('(hiiiiiiii)', [handle, ...bounds]), fds);
        } catch (error) {
            this._clearHandoff();
            if (request.cancelled) {
                this._replyCancelled(invocation);
            } else {
                console.warn(`SnapLing selection failed: ${error.message}`);
                invocation.return_dbus_error(`${IFACE}.Failed`, 'Could not prepare the monitor image');
            }
        } finally {
            if (this._handoff?.request !== request) {
                request.selector?.cancel();
                this._unwatch(request);
            }
            if (request.timeout)
                GLib.Source.remove(request.timeout);
            this._pending = null;
            io?.close(null);
        }
    }

    CancelAsync(_params, invocation) {
        if (this._pending?.sender === invocation.get_sender())
            this._cancel(this._pending);
        if (this._handoff?.request.sender === invocation.get_sender())
            this._clearHandoff();
        invocation.return_value(new GLib.Variant('()', []));
    }

    _replyCancelled(invocation) {
        invocation.return_dbus_error(`${IFACE}.Cancelled`, 'Screenshot selection was cancelled');
    }

    _cancel(request) {
        if (!request)
            return;
        request.cancelled = true;
        if (this._handoff?.request === request)
            this._clearHandoff();
        else
            request.selector?.cancel();
    }

    _startHandoff(request, pid, monitor) {
        const handoff = {request, pid, monitor, timeout: 0};
        this._handoff = handoff;
        handoff.timeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => {
            handoff.timeout = 0;
            this._clearHandoff();
            return GLib.SOURCE_REMOVE;
        });
    }

    _editorMapped(actor) {
        const handoff = this._handoff;
        const window = actor?.meta_window;
        if (!handoff || !actor?.mapped || !window || window.get_pid() !== handoff.pid ||
            window.get_wm_class() !== 'io.snapling.SnapLing' ||
            window.get_title() !== 'SnapLing · 即时截图编辑' || !window.is_fullscreen())
            return;

        // A fullscreen Wayland window may map on the primary output before moving to its requested output.
        const {monitor, request} = handoff;
        const output = Main.layoutManager.monitors[window.get_monitor()];
        if (!output || output.x !== monitor.x || output.y !== monitor.y ||
            output.width !== monitor.width || output.height !== monitor.height)
            return;

        try {
            this._backdrop?.destroy();
            const backdrop = request.selector.setEditorBackdrop(actor, monitor);
            this._backdrop = backdrop;
            backdrop?.connect('destroy', () => {
                if (this._backdrop === backdrop)
                    this._backdrop = null;
            });
        } finally {
            this._clearHandoff();
        }
    }

    _clearHandoff() {
        const handoff = this._handoff;
        if (!handoff)
            return;
        this._handoff = null;
        if (handoff.timeout)
            GLib.Source.remove(handoff.timeout);
        handoff.request.selector.cancel();
        this._unwatch(handoff.request);
    }

    _unwatch(request) {
        if (request.watch) {
            Gio.bus_unwatch_name(request.watch);
            request.watch = 0;
        }
    }

    destroy() {
        this._cancel(this._pending);
        this._clearHandoff();
        this._backdrop?.destroy();
        Main.sessionMode.disconnect(this._sessionChanged);
        Main.layoutManager.disconnect(this._monitorsChanged);
        global.window_manager.disconnect(this._windowMapped);
        global.display.disconnect(this._windowMoved);
        this._dbus.unexport();
        this._dbus = null;
    }
}

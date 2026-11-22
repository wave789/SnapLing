import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Cogl from 'gi://Cogl';
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

function isUserOwnedSnapLingBuild(executable) {
    if (GLib.path_get_basename(executable) !== 'snapling' ||
        !executable.startsWith(`${GLib.get_home_dir()}/`))
        return false;
    try {
        const info = Gio.File.new_for_path(executable).query_info(
            'owner::user,access::can-execute', Gio.FileQueryInfoFlags.NONE, null);
        return info.get_attribute_boolean('access::can-execute') &&
            info.get_attribute_string('owner::user') === GLib.get_user_name();
    } catch {
        return false;
    }
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

    try {
        const source = texture.get_width() === w && texture.get_height() === h
            ? texture
            : Cogl.SubTexture.new(texture.get_context(), texture, x, y, w, h);
        const stride = w * 4;
        // Screenshots are opaque output pixels. Prefer BGRX so a transparent or
        // premultiplied source alpha can never turn the whole capture invisible.
        // Keep premultiplied BGRA as a compatibility fallback for Cogl drivers
        // that cannot convert to BGRX.
        let readback;
        for (const candidate of [
            {format: Cogl.PixelFormat.BGRX_8888, signature: 2, name: 'BGRX_8888'},
            {format: Cogl.PixelFormat.BGRA_8888_PRE, signature: 1, name: 'BGRA_8888_PRE'},
        ]) {
            const pixels = new Uint8Array(stride * h);
            try {
                const written = source.get_data(candidate.format, stride, pixels);
                if (Number.isFinite(written) && written > 0) {
                    readback = {...candidate, pixels, written};
                    break;
                }
            } catch (error) {
                console.log(`SnapLing texture format ${candidate.name} unavailable: ${error.message}`);
            }
        }
        if (!readback)
            throw new Error('texture readback is unavailable');
        const {pixels, signature} = readback;
        // Keep a cheap content signature in the journal. A positive byte count with
        // zero colour samples identifies a Cogl driver that returns an empty buffer.
        const sampleStep = Math.max(4, Math.floor(pixels.length / 4096 / 4) * 4);
        let colourSamples = 0;
        let firstColour = -1;
        let colourVaried = false;
        for (let offset = 0; offset + 3 < pixels.length; offset += sampleStep) {
            const colour = (pixels[offset] << 16) | (pixels[offset + 1] << 8) | pixels[offset + 2];
            if (colour) {
                ++colourSamples;
                if (firstColour < 0)
                    firstColour = colour;
                else if (colour !== firstColour)
                    colourVaried = true;
            }
        }
        if (colourSamples === 0)
            throw new Error(`texture ${readback.name} returned an empty pixel buffer`);
        if (!colourVaried)
            throw new Error(`texture ${readback.name} returned a uniform pixel buffer`);

        // Private raw-pixel container shared with SnapLing:
        // magic[6], version u16, width i32, height i32, stride i32,
        // format u32 (1 = premultiplied BGRA, 2 = opaque BGRX), payload size u64.
        const header = new Uint8Array(32);
        const view = new DataView(header.buffer);
        header.set([0x53, 0x4e, 0x52, 0x41, 0x57, 0x31], 0);
        view.setUint16(6, 1, true);
        view.setInt32(8, w, true);
        view.setInt32(12, h, true);
        view.setInt32(16, stride, true);
        view.setUint32(20, signature, true);
        view.setUint32(24, pixels.length, true);
        view.setUint32(28, 0, true);
        // Use GLib.Bytes for the large pixel block. write_all(Uint8Array) can report
        // success without preserving the binary backing store on some GJS versions.
        for (const block of [header, pixels]) {
            const result = stream.write_bytes(GLib.Bytes.new(block), null);
            const success = Array.isArray(result) ? result[0] : result;
            if (!success)
                throw new Error('incomplete texture handoff write');
        }
        stream.flush(null);
        console.log(`SnapLing texture handoff ${readback.name}: bytes=${readback.written} ` +
            `colour_samples=${colourSamples}`);
        return bounds;
    } catch (error) {
        console.log(`SnapLing raw texture handoff unavailable: ${error.message}`);
    }

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
        return 4;
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
            if (!executable || (!this._allowed.has(executable) && !isUserOwnedSnapLingBuild(executable))) {
                invocation.return_dbus_error(`${IFACE}.NotAllowed`,
                    `SnapLing executable identity rejected: ${executable ?? 'unknown'}`);
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
            const encodeStart = GLib.get_monotonic_time();
            const bounds = await writeMonitor(selection, io.get_output_stream());
            const encodeMs = (GLib.get_monotonic_time() - encodeStart) / 1000;
            console.log(`SnapLing monitor handoff encoded in ${encodeMs.toFixed(1)} ms`);
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

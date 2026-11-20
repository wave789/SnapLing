#!/usr/bin/python3
"""Real xdg-desktop-portal frontend, isolated bus/config, synthetic backend and permission store.

Requires system Python PyGObject, dbus-run-session and a real xdg-desktop-portal executable.
Does not connect to the user's desktop bus or capture their screen.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def fixture_png(path, color):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = (b"\0" + color * 240) * 320
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 240, 320, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def worker(args):
    if os.environ.get("SNAPLING_IDENTITY_TEST_PRIVATE_BUS") != "1":
        raise RuntimeError("worker requires the isolated test launcher")
    from gi.repository import Gio, GLib

    root = Path(os.environ["SNAPLING_IDENTITY_TEST_ROOT"])
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    records = {"": ["no"], "another.app": ["yes"]}
    dialogs, captures, writes = [], [], []
    app_id = "io.snapling.SnapLing"
    window_probe = "io.snapling.Test.WindowProbe"
    xml = """<node>
    <interface name="org.freedesktop.impl.portal.PermissionStore">
      <method name="Lookup"><arg type="s" direction="in"/><arg type="s" direction="in"/>
        <arg type="a{sas}" direction="out"/><arg type="v" direction="out"/></method>
      <method name="SetPermission"><arg type="s" direction="in"/><arg type="b" direction="in"/>
        <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/></method>
    </interface>
    <interface name="org.freedesktop.impl.portal.Screenshot">
      <property name="version" type="u" access="read"/>
      <method name="Screenshot"><arg type="o" direction="in"/><arg type="s" direction="in"/>
        <arg type="s" direction="in"/><arg type="a{sv}" direction="in"/>
        <arg type="u" direction="out"/><arg type="a{sv}" direction="out"/></method>
    </interface>
    <interface name="org.freedesktop.impl.portal.Access">
      <property name="version" type="u" access="read"/>
      <method name="AccessDialog"><arg type="o" direction="in"/><arg type="s" direction="in"/>
        <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
        <arg type="s" direction="in"/><arg type="a{sv}" direction="in"/>
        <arg type="u" direction="out"/><arg type="a{sv}" direction="out"/></method>
    </interface></node>"""

    def window_state():
        # Before the application starts there are no client windows. During its
        # capture query the real Qt window state, not a counter or guessed delay.
        owned, = bus.call_sync(
            "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
            GLib.Variant("(s)", (window_probe,)), None, Gio.DBusCallFlags.NONE, 2000, None).unpack()
        if not owned:
            return "hidden"
        state, = bus.call_sync(window_probe, "/probe", window_probe, "MainWindowState", None, None,
                               Gio.DBusCallFlags.NONE, 2000, None).unpack()
        return state

    def method(connection, sender, path, interface, name, parameters, invocation):
        values = parameters.unpack()
        if name == "Lookup":
            invocation.return_value(GLib.Variant("(a{sas}v)", (records, GLib.Variant("s", "test-data"))))
        elif name == "SetPermission":
            table, create, item, app, permissions = values
            if table != "screenshot" or item != "screenshot" or app != "io.snapling.SnapLing":
                invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied", "unexpected permission mutation")
                return
            records[app] = permissions
            writes.append((app, permissions))
            invocation.return_value(GLib.Variant("()", ()))
        elif name == "AccessDialog":
            state = window_state()
            dialogs.append((values[1], state))
            # GNOME Shell rejects a parentless prompt for an unfocused caller
            # with a method error, which must leave the permission unset.
            if values[1] != app_id or state != "focused":
                invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied",
                                             "Only the focused app is allowed to show a system access dialog")
                return
            invocation.return_value(GLib.Variant("(ua{sv})", (0, {})))
        elif name == "Screenshot":
            state = window_state()
            captures.append((values[1], state))
            path = root / ("desktop.png" if state == "hidden" else "with-snapling.png")
            result = {"uri": GLib.Variant("s", path.as_uri())} if values[1] == app_id else {}
            invocation.return_value(GLib.Variant("(ua{sv})", (0 if result else 2, result)))
        else:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", name)

    for info in Gio.DBusNodeInfo.new_for_xml(xml).interfaces:
        path = ("/org/freedesktop/impl/portal/PermissionStore" if info.name.endswith("PermissionStore")
                else "/org/freedesktop/portal/desktop")
        bus.register_object(path, info, method, lambda *unused: GLib.Variant("u", 2), None)
    for name in ("org.freedesktop.impl.portal.PermissionStore", "org.freedesktop.impl.portal.desktop.snaplingtest"):
        bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                      GLib.Variant("(su)", (name, 0)), None, Gio.DBusCallFlags.NONE, 3000, None)

    def pump_until(predicate, seconds=15):
        context = GLib.MainContext.default()
        deadline = time.monotonic() + seconds
        while not predicate() and time.monotonic() < deadline:
            if frontend.poll() is not None:
                raise RuntimeError("private xdg-desktop-portal exited")
            while context.pending():
                context.iteration(False)
            time.sleep(0.01)
        if not predicate():
            raise RuntimeError("timed out waiting for private Portal test")

    def portal_available():
        answer = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                               GLib.Variant("(s)", ("org.freedesktop.portal.Desktop",)), None,
                               Gio.DBusCallFlags.NONE, 1000, None)
        return answer.unpack()[0]

    def new_connection():
        return Gio.DBusConnection.new_for_address_sync(
            os.environ["DBUS_SESSION_BUS_ADDRESS"],
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

    def register(conn):
        conn.call_sync("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                       "org.freedesktop.host.portal.Registry", "Register",
                       GLib.Variant("(sa{sv})", (app_id, {})), None, Gio.DBusCallFlags.NONE, 3000, None)

    def screenshot_response(conn):
        replies = []
        subscription = conn.signal_subscribe("org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
                                             "Response", None, None, Gio.DBusSignalFlags.NONE,
                                             lambda *event: replies.append(event[-1].unpack()))
        try:
            conn.call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                      "org.freedesktop.portal.Screenshot", "Screenshot",
                      GLib.Variant("(sa{sv})", ("", {"interactive": GLib.Variant("b", False)})),
                      None, Gio.DBusCallFlags.NONE, 5000, None, None)
            pump_until(lambda: bool(replies))
            return replies[0]
        finally:
            conn.signal_unsubscribe(subscription)

    log = (root / "frontend.log").open("w+")
    frontend = subprocess.Popen([args.portal, "--verbose"], stdout=log, stderr=subprocess.STDOUT)
    try:
        pump_until(portal_available)
        client = new_connection()
        try:
            assert screenshot_response(client)[0] == 2
        finally:
            client.close_sync(None)
        assert not dialogs and not captures
        print("real_portal_anonymous_host_denied=PASS code=2", flush=True)

        identity_client = new_connection()
        try:
            register(identity_client)
        except GLib.Error as error:
            assert "App info not found" in str(error), error
        else:
            raise AssertionError("registration must fail before preparing the desktop entry")
        finally:
            identity_client.close_sync(None)
        print("real_portal_missing_desktop_rejected=PASS", flush=True)
        source_root = Path(__file__).resolve().parent.parent
        subprocess.run([str(source_root / "tools/install-desktop.sh")], check=True, timeout=15)
        assert Gio.DesktopAppInfo.new("io.snapling.SnapLing.desktop") is not None
        print("real_portal_source_entry_prepared=PASS", flush=True)

        background = new_connection()
        try:
            register(background)
            assert screenshot_response(background) == (2, {})
            assert dialogs == [(app_id, "hidden")] and not captures and not writes
            assert app_id not in records
        finally:
            background.close_sync(None)
        print("real_portal_registered_background_denied=PASS code=2 app=unset", flush=True)

        output_file = (root / "client.log").open("w+")
        process = subprocess.Popen([args.test_binary, "firstCaptureWithForegroundConsent"], stdout=output_file,
                                   stderr=subprocess.STDOUT, env=os.environ | {"XDG_CURRENT_DESKTOP": "ubuntu:GNOME"})
        try:
            pump_until(lambda: process.poll() is not None)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
        output_file.seek(0)
        output = output_file.read()
        output_file.close()
        print(output, end="", flush=True)
        assert process.returncode == 0, process.returncode
        assert "portal_actual_image=240x320" in output and "SKIP" not in output
        assert "portal_identity=registered app_id=io.snapling.SnapLing" in output
        assert dialogs == [(app_id, "hidden"), (app_id, "focused")], dialogs
        assert captures == [(app_id, "focused"), (app_id, "hidden")], captures
        assert records[""] == ["no"] and records["another.app"] == ["yes"]
        assert writes == [("io.snapling.SnapLing", ["yes"])], writes
        print("real_portal_registered_app_scoped_dialog_and_capture=PASS", flush=True)
        print("real_portal_consent_frame_discarded_clean_desktop_presented=PASS", flush=True)
        print("real_portal_host_denial_unchanged=PASS", flush=True)
    finally:
        frontend.terminate()
        frontend.wait(timeout=5)
        log.seek(0)
        if sys.exc_info()[0]:
            print(log.read(), file=sys.stderr)
        log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portal", required=True, help="path to real xdg-desktop-portal")
    parser.add_argument("--test-binary", default="build/snapling_portal_test")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.portal = str(Path(args.portal).resolve())
    args.test_binary = str(Path(args.test_binary).resolve())
    if args.worker:
        worker(args)
        return
    with tempfile.TemporaryDirectory(prefix="snapling-real-portal-") as directory:
        root = Path(directory)
        for name in ("config/xdg-desktop-portal", "data/applications", "portals", "runtime", "empty"):
            (root / name).mkdir(parents=True, exist_ok=True)
        (root / "runtime").chmod(0o700)
        (root / "portals/snaplingtest.portal").write_text(
            "[portal]\nDBusName=org.freedesktop.impl.portal.desktop.snaplingtest\n"
            "Interfaces=org.freedesktop.impl.portal.Screenshot;org.freedesktop.impl.portal.Access;\nUseIn=SnapLingTest;\n")
        (root / "config/xdg-desktop-portal/portals.conf").write_text("[preferred]\ndefault=snaplingtest\n")
        (root / "portals/portals.conf").write_text("[preferred]\ndefault=snaplingtest\n")
        fixture_png(root / "desktop.png", b"\x22\x88\x55")
        fixture_png(root / "with-snapling.png", b"\xcc\x11\x11")
        env = os.environ.copy()
        env.update({"SNAPLING_IDENTITY_TEST_PRIVATE_BUS": "1", "SNAPLING_IDENTITY_TEST_ROOT": str(root),
                    "XDG_CONFIG_HOME": str(root / "config"), "XDG_CONFIG_DIRS": str(root / "empty"),
                    "XDG_DATA_HOME": str(root / "data"), "XDG_DATA_DIRS": str(root / "empty"),
                    "XDG_CACHE_HOME": str(root / "cache"), "XDG_RUNTIME_DIR": str(root / "runtime"),
                    "XDG_DESKTOP_PORTAL_DIR": str(root / "portals"), "XDG_CURRENT_DESKTOP": "SnapLingTest",
                    "QT_QPA_PLATFORM": "minimal", "GSETTINGS_BACKEND": "memory", "GIO_USE_VFS": "local"})
        env.setdefault("GSETTINGS_SCHEMA_DIR", "/usr/share/glib-2.0/schemas")
        command = ["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()), "--worker",
                   "--portal", args.portal, "--test-binary", args.test_binary]
        subprocess.run(command, env=env, check=True, timeout=45)


if __name__ == "__main__":
    main()

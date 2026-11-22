import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Screenshot from 'resource:///org/gnome/shell/ui/screenshot.js';

import {ScreenshotBridge} from './screenshotBridge.js';

export default class SkipScreenshotFlash extends Extension {
    enable() {
        this._injections = new InjectionManager();
        this._bridge = new ScreenshotBridge(this.path);
        const prototype = Screenshot.Flashspot?.prototype;
        if (typeof prototype?.fire === 'function') {
            this._injections.overrideMethod(prototype, 'fire', original => function(done) {
                if (typeof done !== 'function')
                    return original.call(this, done);

                // Complete only the flash step. The shell awaits the screenshot file separately.
                try {
                    done();
                } finally {
                    this.destroy();
                }
            });
        }

        if (typeof Main.wm?._shouldAnimateActor === 'function') {
            this._injections.overrideMethod(Main.wm, '_shouldAnimateActor', original => function(actor, types) {
                // Preserve the window manager's own animation state before checking our overlay.
                if (!original.call(this, actor, types))
                    return false;
                const window = actor.meta_window;
                return !(window?.get_wm_class() === 'io.snapling.SnapLing' &&
                    window.get_title() === 'SnapLing · 即时截图编辑' && window.is_fullscreen());
            });
        }
    }

    disable() {
        this._bridge?.destroy();
        this._bridge = null;
        this._injections?.clear();
        this._injections = null;
    }
}

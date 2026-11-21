import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Screenshot from 'resource:///org/gnome/shell/ui/screenshot.js';

export default class SkipScreenshotFlash extends Extension {
    enable() {
        const prototype = Screenshot.Flashspot?.prototype;
        if (typeof prototype?.fire !== 'function')
            return;

        this._injections = new InjectionManager();
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

    disable() {
        this._injections?.clear();
        this._injections = null;
    }
}

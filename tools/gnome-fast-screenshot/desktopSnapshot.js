import Clutter from 'gi://Clutter';
import Mtk from 'gi://Mtk';
import Shell from 'gi://Shell';

// Installs the async bindings used by the Shell screenshot implementation.
import 'resource:///org/gnome/shell/ui/screenshot.js';

function frameSize(bounds) {
    const rect = new Mtk.Rectangle(bounds);
    const [valid, width, height, scale] = global.stage.get_capture_final_size(rect);
    if (!valid || width <= 0 || height <= 0 || !Number.isFinite(scale) || scale <= 0)
        throw new Error('GNOME did not provide a valid capture size');
    return {bounds, rect, scale, pixels: width * height};
}

function render({bounds, rect, scale}, withColorState) {
    const flags = Clutter.PaintFlag.NO_CURSORS;
    const content = withColorState
        ? global.stage.paint_to_content(rect, scale, null, flags)
        : global.stage.paint_to_content(rect, scale, flags);
    if (!content?.get_texture())
        throw new Error('GNOME did not provide a frozen desktop');
    return {bounds, content, scale};
}

function stagePaintAvailable() {
    const paintArity = global.stage.paint_to_content?.length;
    return (paintArity === 3 || paintArity === 4) &&
        typeof global.stage.get_capture_final_size === 'function';
}

async function fallbackDesktop() {
    const [content, scale] = await new Shell.Screenshot().screenshot_stage_to_content();
    if (!content?.get_texture() || !Number.isFinite(scale) || scale <= 0)
        throw new Error('GNOME did not provide a frozen desktop');
    const [width, height] = global.display.get_size();
    return [{bounds: {x: 0, y: 0, width, height}, content, scale}];
}

export async function freezeMonitor(monitor) {
    if (!stagePaintAvailable())
        return fallbackDesktop();
    return [render(frameSize(monitor), global.stage.paint_to_content.length === 4)];
}

export async function freezeDesktop(monitors) {
    const stage = global.stage;
    const [width, height] = global.display.get_size();
    const bounds = {x: 0, y: 0, width, height};
    const paintArity = stage.paint_to_content?.length;
    if (!stagePaintAvailable())
        return fallbackDesktop();

    const desktop = frameSize(bounds);
    const outputs = monitors.map(frameSize);
    const outputPixels = outputs.reduce((sum, output) => sum + output.pixels, 0);
    // On mixed-DPI or sparse layouts a full-stage texture can contain far more pixels.
    // Only split when the pixel savings justify the additional paint calls.
    const frames = outputPixels <= desktop.pixels * 0.75 ? outputs : [desktop];
    // Paint synchronously in one compositor turn, before showing any overlay.
    return frames.map(frame => render(frame, paintArity === 4));
}

export function snapshotAt(snapshots, monitor) {
    return snapshots.find(({bounds}) => monitor.x >= bounds.x && monitor.y >= bounds.y &&
        monitor.x + monitor.width <= bounds.x + bounds.width &&
        monitor.y + monitor.height <= bounds.y + bounds.height);
}

export function snapshotActor({bounds, content}, x = 0, y = 0) {
    return new Clutter.Actor({
        content,
        content_gravity: Clutter.ContentGravity.RESIZE_FILL,
        x: bounds.x - x, y: bounds.y - y, width: bounds.width, height: bounds.height,
    });
}

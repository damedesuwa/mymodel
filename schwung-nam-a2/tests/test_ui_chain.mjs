/* Run ui_chain.js against stub host globals and drive it.
 *
 * "The menu does not open" has been reported twice with no way to tell it
 * from "the build has no menu" or "the click never arrived". None of this
 * needs the device: the file is ordinary JavaScript and the host bindings
 * it uses are half a dozen functions. */
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const src = fs.readFileSync(path.join(here, '..', 'src', 'c', 'ui_chain.js'), 'utf8')
    .replace(/^import .*$/m, '');

/* --- the module's world ------------------------------------------------ */
const params = {
    sel_block: '0', cpu: '41', build: 'test',
    model_list: JSON.stringify(['OCD', 'Recto']),
    cab_list: JSON.stringify(['TF MESA']),
};
for (let b = 1; b <= 8; b++) {
    params[`b${b}_type`] = b <= 2 ? '1' : (b === 3 ? '2' : '0');
    params[`b${b}_on`] = '0';                 /* 0 = On */
    params[`b${b}_model`] = '0';
    params[`b${b}_quality`] = '0';
    params[`b${b}_cab`] = '0';
    params[`b${b}_cpu`] = '7';
    params[`b${b}_dmode`] = '0';
    params[`b${b}_drive`] = '0.5';
    params[`b${b}_tone`] = '0.5';
    params[`b${b}_level`] = '0.5';
}

const writes = [];
const drawn = [];
const leds = {};
let padBlock = -1;
const actionsRun = [];

const host = {
    host_module_get_param: (k) => (k in params ? params[k] : null),
    host_module_set_param: (k, v) => { params[k] = String(v); writes.push([k, String(v)]); },
    host_pad_block: (v) => { padBlock = v; },
    clear_screen: () => drawn.push('<clear>'),
    print: (x, y, t) => drawn.push(String(t)),
    draw_rect: () => {}, fill_rect: () => {},
    text_width: (t) => String(t).length * 5,
    console: { log: () => {} },
    shadow_component_trailing_menus: () => ([
        { name: 'My Presets', entries: [
            { label: 'Preset', value: '(none)', action: 'up_load' },
            { label: 'Save As', action: 'up_save_as' } ] },
        { name: 'Module', entries: [
            { label: 'Swap Module', action: 'swap' },
            { label: 'Remove Module', action: 'remove' },
            { label: 'Module Help', action: 'help' } ] },
    ]),
    shadow_component_run_action: (a) => { actionsRun.push(a); return true; },
};

const stubs = `const setLED=(n,c)=>{leds[n]=c;},
      decodeDelta=(v)=>(v===0?0:(v<=63?v:-(128-v))),
      invalidateLedCache=()=>{};`;
const names = Object.keys(host);
const fn = new Function('globalThis', 'leds', ...names,
    stubs + src + '\nreturn globalThis.chain_ui;');
const ui = fn({}, leds, ...names.map(n => host[n]));

/* --- drive it ---------------------------------------------------------- */
let fails = 0;
const ok = (cond, what) => { console.log((cond ? 'ok   ' : 'FAIL ') + what); if (!cond) fails++; };

ui.init();
ok(typeof ui.tick === 'function', 'chain_ui exports init/tick/onMidiMessageInternal');

ui.tick();
ok(padBlock === 1, 'tick() raises host_pad_block(1)');
ok(drawn.some(t => t.includes('test')), 'header carries the build id');
ok(drawn.some(t => t === 'NAM'), 'pedalboard draws block types');

/* A pad TAP stomps. */
writes.length = 0;
ui.onMidiMessageInternal([0x90, 68, 127]);
ui.onMidiMessageInternal([0x80, 68, 0]);
ok(writes.some(([k, v]) => k === 'b1_on' && v === '1'), 'pad tap bypasses the block');

/* A pad HOLD selects. */
writes.length = 0;
ui.onMidiMessageInternal([0x90, 70, 127]);
const realNow = Date.now;
Date.now = () => realNow() + 1000;          /* past HOLD_MS */
ui.tick();
Date.now = realNow;
ok(writes.some(([k, v]) => k === 'sel_block' && v === '2'), 'pad hold selects the block');
ui.onMidiMessageInternal([0x80, 70, 0]);

/* THE MENU. */
drawn.length = 0;
ui.onMidiMessageInternal([0xb0, 3, 127]);   /* jog click */
ui.tick();
ok(drawn.some(t => t === 'Remove Module'), 'jog click opens the menu with Remove Module');
ok(drawn.some(t => t === 'Preset: (none)'), 'preset row carries its value');

/* Scroll to Remove Module and run it. */
const rows = ['Preset: (none)', 'Save As', 'Swap Module', 'Remove Module', 'Module Help', 'Close'];
for (let i = 0; i < rows.indexOf('Remove Module'); i++)
    ui.onMidiMessageInternal([0xb0, 14, 1]); /* jog turn, one detent CW */
drawn.length = 0;
ui.tick();
ok(drawn.some(t => t === 'Remove Module'), 'cursor reaches Remove Module');
ui.onMidiMessageInternal([0xb0, 3, 127]);   /* click it */
ok(actionsRun.includes('remove'), 'clicking Remove Module runs the host action');

/* Pads are inert while the menu is up. */
ui.onMidiMessageInternal([0xb0, 3, 127]);   /* re-open */
writes.length = 0;
ui.onMidiMessageInternal([0x90, 68, 127]);
ui.onMidiMessageInternal([0x80, 68, 0]);
ok(writes.length === 0, 'pads are inert while the menu is open');

/* Close leaves. */
for (let i = 0; i < 10; i++) ui.onMidiMessageInternal([0xb0, 14, 1]);  /* to the end */
ui.onMidiMessageInternal([0xb0, 3, 127]);
drawn.length = 0;
ui.tick();
ok(drawn.some(t => t === 'NAM'), 'Close returns to the pedalboard');

/* --- the host refusing to answer -------------------------------------- */
/* Three ways the binding can come back useless. The menu has to stay usable
 * in all of them, because it is the only way to remove the module from its
 * own screen. */
for (const [what, impl] of [
    ['empty list', () => []],
    ['a throw',    () => { throw new Error('state read failed'); }],
    ['no binding', null],
]) {
    const h2 = Object.assign({}, host);
    if (impl) h2.shadow_component_trailing_menus = impl;
    else delete h2.shadow_component_trailing_menus;
    const ran = [];
    h2.shadow_component_run_action = (a) => { ran.push(a); return true; };
    const seen = [];
    h2.print = (x, y, t) => seen.push(String(t));
    h2.clear_screen = () => {};
    const n2 = Object.keys(h2);
    const ui2 = new Function('globalThis', 'leds', ...n2,
        stubs + src + '\nreturn globalThis.chain_ui;')({}, {}, ...n2.map(n => h2[n]));
    ui2.init();
    ui2.onMidiMessageInternal([0xb0, 3, 127]);       /* open */

    /* Scroll to the bottom: Remove Module is the last action row, below a
     * five-row fold, exactly as it is in the host's own menu. */
    let found = false;
    for (let i = 0; i < 10 && !found; i++) {
        seen.length = 0;
        ui2.tick();
        found = seen.includes('Remove Module');
        if (!found) ui2.onMidiMessageInternal([0xb0, 14, 1]);
    }
    ok(found, `host gives ${what} -> Remove Module is reachable`);

    /* And it runs. The cursor is on it when the scan stopped. */
    ui2.onMidiMessageInternal([0xb0, 3, 127]);
    ok(ran.includes('remove_module'), `host gives ${what} -> Remove Module runs`);
}

console.log(fails ? 'FAILED' : 'PASS');
process.exit(fails ? 1 : 0);

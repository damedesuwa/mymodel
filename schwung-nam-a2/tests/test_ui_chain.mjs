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
    fx_list: JSON.stringify(['Overdrive', 'Distortion', 'Fuzz', 'Boost',
        'Compressor', 'Gate', 'EQ', 'Auto Wah', 'Chorus', 'Phaser',
        'Tremolo', 'Delay', 'Slapback', 'Reverb', 'Doubler', 'Detune']),
};
for (let b = 1; b <= 8; b++) {
    params[`b${b}_type`] = b <= 2 ? '1' : (b === 3 ? '2' : '0');
    params[`b${b}_on`] = '0';                 /* 0 = On */
    params[`b${b}_model`] = '0';
    params[`b${b}_quality`] = '0';
    params[`b${b}_cab`] = '0';
    params[`b${b}_cpu`] = '7';
    params[`b${b}_fx`] = '0';
    for (let k = 1; k <= 5; k++) params[`b${b}_p${k}`] = '0.5';
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

/* --- the three-knob tree ---------------------------------------------- */
/* Coarse to fine: knob 1 the kind, knob 2 the family, knob 3 the pedal. */
{
    /* Block 4 is Off; make it an FX block with knob 1. */
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 71, 127]);       /* hold pad 4 */
    const rn = Date.now; Date.now = () => rn() + 1000; ui.tick(); Date.now = rn;
    ui.onMidiMessageInternal([0x80, 71, 0]);
    ok(writes.some(([k, v]) => k === 'sel_block' && v === '3'), 'tree: pad 4 selected');

    writes.length = 0;
    for (let i = 0; i < 3; i++) ui.onMidiMessageInternal([0xb0, 71, 1]);   /* knob 1 up */
    ok(params['b4_type'] === '3', 'tree: knob 1 reaches FX');

    /* Knob 2 walks families and lands on each one's FIRST pedal. */
    const famFirst = [];
    for (let i = 0; i < 5; i++) {
        ui.onMidiMessageInternal([0xb0, 72, 1]);
        famFirst.push(Number(params['b4_fx']));
    }
    ok(JSON.stringify(famFirst) === JSON.stringify([4, 6, 8, 11, 14]),
       'tree: knob 2 steps Drive->Dynamic->Filter->Mod->Time->Pitch');

    /* Knob 3 walks inside the family it is already in, and stops at its end. */
    ui.onMidiMessageInternal([0xb0, 73, 1]);
    ok(params['b4_fx'] === '15', 'tree: knob 3 moves within Pitch');
    ui.onMidiMessageInternal([0xb0, 73, 1]);
    ok(params['b4_fx'] === '15', 'tree: knob 3 stops at the end of its family');

    /* Back to Time, and the knobs past the tree are that pedal's. */
    ui.onMidiMessageInternal([0xb0, 72, 127]);       /* knob 2 down -> Time */
    ok(params['b4_fx'] === '11', 'tree: knob 2 back to Time = Delay');
    drawn.length = 0;
    ui.onMidiMessageInternal([0xb0, 76, 1]);         /* knob 6 = Mix */
    ui.tick();
    ok(drawn.includes('Mix'), 'tree: Delay exposes Time/Fdbk/Mix past the tree');
    ok(Number(params['b4_p3']) > 0.5, 'tree: turning it writes that pedal param');

    /* A pedal with fewer knobs does not offer the ones it lacks. */
    ui.onMidiMessageInternal([0xb0, 72, 1]);         /* -> Pitch / Doubler */
    ui.onMidiMessageInternal([0xb0, 72, 127]);
    ui.onMidiMessageInternal([0xb0, 72, 127]);
    ui.onMidiMessageInternal([0xb0, 72, 127]);       /* -> Dynamic / Compressor */
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, 78, 1]);         /* knob 8: past the end */
    ok(writes.length === 0, 'tree: a knob the pedal has no use for is inert');
}

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
    h2.host_swap_module = () => { ran.push('__swap'); };
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
        found = seen.includes('Swap / Remove...');
        if (!found) ui2.onMidiMessageInternal([0xb0, 14, 1]);
    }
    ok(found, `host gives ${what} -> Swap / Remove is reachable`);

    /* Land ON it: run to the end (the cursor clamps at Close, the last
     * row) and come back one. Scrolling only until the row is DRAWN leaves
     * the cursor wherever the fold put it. */
    for (let i = 0; i < 20; i++) ui2.onMidiMessageInternal([0xb0, 14, 1]);
    ui2.onMidiMessageInternal([0xb0, 14, 127]);   /* one detent CCW */
    ui2.onMidiMessageInternal([0xb0, 3, 127]);
    ok(ran.includes('__swap'), `host gives ${what} -> Swap / Remove opens the picker`);
}

/* AN OLD HOST: only host_swap_module exists. Every action-key row would be
 * inert, so none is offered - but the door still opens. */
{
    const h3 = Object.assign({}, host);
    delete h3.shadow_component_trailing_menus;
    delete h3.shadow_component_run_action;
    const ran = [];
    h3.host_swap_module = () => ran.push('__swap');
    const seen = [];
    h3.print = (x, y, t) => seen.push(String(t));
    h3.clear_screen = () => {};
    const n3 = Object.keys(h3);
    const ui3 = new Function('globalThis', 'leds', ...n3,
        stubs + src + '\nreturn globalThis.chain_ui;')({}, {}, ...n3.map(n => h3[n]));
    ui3.init();
    ui3.onMidiMessageInternal([0xb0, 3, 127]);
    ui3.tick();
    ok(seen.includes('Swap / Remove...'), 'old host -> Swap / Remove offered');
    ok(!seen.includes('Save As'), 'old host -> no inert rows offered');
    ui3.onMidiMessageInternal([0xb0, 3, 127]);
    ok(ran.includes('__swap'), 'old host -> it opens the picker');
}

console.log(fails ? 'FAILED' : 'PASS');
process.exit(fails ? 1 : 0);

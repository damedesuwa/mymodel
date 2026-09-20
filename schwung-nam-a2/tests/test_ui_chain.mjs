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
const buttonLeds = {};
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
      setButtonLED=(cc,c)=>{buttonLeds[cc]=c;},
      decodeDelta=(v)=>(v===0?0:(v<=63?v:-(128-v))),
      invalidateLedCache=()=>{};`;
const names = Object.keys(host);
const fn = new Function('globalThis', 'leds', 'buttonLeds', ...names,
    stubs + src + '\nreturn globalThis.chain_ui;');
const ui = fn({}, leds, buttonLeds, ...names.map(n => host[n]));

/* --- drive it ---------------------------------------------------------- */
let fails = 0;
const ok = (cond, what) => { console.log((cond ? 'ok   ' : 'FAIL ') + what); if (!cond) fails++; };

ui.init();
ok(typeof ui.tick === 'function', 'chain_ui exports init/tick/onMidiMessageInternal');

ui.tick();
ok(padBlock === 1, 'tick() raises host_pad_block(1)');
/* The build id moved off the pedalboard header - it was clutter on the one
 * line you read while playing - and onto the menu, which is where you go
 * when you are asking questions about the module. */
ok(!drawn.some(t => String(t).includes('test')), 'pedalboard header is not carrying the build id');
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

    /* ONE STEP IS THREE DETENTS on every discrete control - the encoders
     * are not detented and the shim coalesces, so a message carries however
     * many ticks arrived in that audio frame. Every discrete turn below
     * spends three, which is what the screen costs. */
    const STEP = 3;
    const turn = (cc, dir, steps = 1) => {
        for (let i = 0; i < steps * STEP; i++)
            ui.onMidiMessageInternal([0xb0, cc, dir > 0 ? 1 : 127]);
    };

    /* The sensitivity itself, which is the thing that was wrong: two
     * detents must move NOTHING, and the third must move exactly one. */
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, 71, 1]);
    ui.onMidiMessageInternal([0xb0, 71, 1]);
    ok(writes.length === 0, 'knob: two detents do not make a step');
    ui.onMidiMessageInternal([0xb0, 71, 1]);
    ok(params['b4_type'] === '1', 'knob: the third detent makes exactly one step');

    /* And a fast spin is still proportional - the coalesced magnitude is
     * spent, not thrown away, so six ticks in one message is two steps. */
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, 71, 6]);
    ok(params['b4_type'] === '3', 'knob: a coalesced spin spends every tick');

    /* Knob 2 walks families and lands on each one's FIRST pedal. */
    const famFirst = [];
    for (let i = 0; i < 5; i++) {
        turn(72, +1);
        famFirst.push(Number(params['b4_fx']));
    }
    ok(JSON.stringify(famFirst) === JSON.stringify([4, 6, 8, 11, 14]),
       'tree: knob 2 steps Drive->Dynamic->Filter->Mod->Time->Pitch');

    /* Knob 3 walks inside the family it is already in, and stops at its end. */
    turn(73, +1);
    ok(params['b4_fx'] === '15', 'tree: knob 3 moves within Pitch');
    turn(73, +1, 4);
    ok(params['b4_fx'] === '22', 'tree: knob 3 stops at the end of its family');

    /* Back to Time, and the knobs past the tree are that pedal's. */
    turn(72, -1);                                    /* knob 2 down -> Time */
    ok(params['b4_fx'] === '11', 'tree: knob 2 back to Time = Delay');
    drawn.length = 0;
    ui.onMidiMessageInternal([0xb0, 76, 1]);         /* knob 6 = Mix */
    ui.tick();
    ok(drawn.includes('Mix'), 'tree: Delay exposes Time/Fdbk/Mix past the tree');
    /* WHERE YOU ARE, spelled out. The group cell clips at 31 px, so the
     * footer is the only place the family name survives in full - and the
     * n/N is what says there is more of it to turn to. */
    ok(drawn.some(t => String(t) === 'Delay  Time 1/5'),
       'footer: names the pedal, its family and its place in it');
    ok(Number(params['b4_p3']) > 0.5, 'tree: turning it writes that pedal param');

    /* A pedal with fewer knobs does not offer the ones it lacks - but the
     * encoder is still CLAIMED, and writes nothing. Gate has two knobs, so
     * encoder 6 is past its end and before the board's own two. */
    turn(72, -1, 3);                                 /* -> Dynamic / Compressor */
    ok(params['b4_fx'] === '4', 'tree: back round to Compressor');
    turn(73, +1);                                    /* -> Gate */
    ok(params['b4_fx'] === '5', 'tree: knob 3 reaches Gate');
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, 76, 1]);         /* knob 6: past the end */
    ok(writes.length === 0, 'tree: a knob the pedal has no use for is inert');

    /* THE BOARD'S OWN TWO. Knobs 7 and 8 are In and Out on every screen,
     * whatever is loaded, and they address the plain keys - not b4_. */
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, 77, 1]);
    ok(writes.some(([k]) => k === 'in_level'), 'board: knob 7 is Input');
    ok(!writes.some(([k]) => k.indexOf('b4_') === 0), 'board: it is not a block key');
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, 78, 1]);
    ok(writes.some(([k]) => k === 'out_level'), 'board: knob 8 is Output');
    drawn.length = 0;
    ui.tick();
    ok(drawn.includes('In') && drawn.includes('Out'),
       'board: the levels are on screen once a level knob is touched');

    /* THE RINGS. A knob that does something is lit; one that does not is
     * dark. Colour 0 is the reserved "nothing here". */
    ok(buttonLeds[76] === 0, 'rings: an unbound encoder is dark');
    ok(buttonLeds[77] !== 0 && buttonLeds[77] !== undefined,
       'rings: Input is lit');
    ok(buttonLeds[71] !== 0 && buttonLeds[71] !== undefined,
       'rings: Type is lit');
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

/* --- nothing may be drawn off the screen ------------------------------ */
/* The overlap the user reported was arithmetic on assumed text widths. The
 * only way to keep it fixed is to check every print, in every state. */
{
    const bad = [];
    const h4 = Object.assign({}, host);
    h4.text_width = (t) => String(t).length * 6;     /* a WIDER font than
                                                        the stub's, so the
                                                        clipping is the
                                                        thing under test */
    h4.print = (x, y, t) => {
        const w = String(t).length * 6;
        if (x < 0 || y < 0 || x + w > 128 || y + 8 > 64)
            bad.push(`"${t}" at ${x},${y} (w=${w})`);
    };
    h4.fill_rect = (x, y, w, h) => {
        if (x < 0 || y < 0 || x + w > 128 || y + h > 64)
            bad.push(`fill ${x},${y} ${w}x${h}`);
    };
    h4.draw_rect = h4.fill_rect;
    h4.clear_screen = () => {};
    const n4 = Object.keys(h4);
    const mk = () => new Function('globalThis', 'leds', ...n4,
        stubs + src + '\nreturn globalThis.chain_ui;')({}, {}, ...n4.map(n => h4[n]));

    const ui4 = mk();
    ui4.init();
    /* Every block type, every pedal, every knob column, and the menu. */
    for (let b = 0; b < 8; b++) {
        params[`b${b + 1}_type`] = String(b % 4);
        params[`b${b + 1}_on`] = String(b % 2);
    }
    for (let fx = 0; fx < 16; fx++) {
        params.b1_type = '3';
        params.b1_fx = String(fx);
        const u = mk();
        u.init();
        for (let k = 0; k < 8; k++) {
            u.onMidiMessageInternal([0xb0, 71 + k, 1]);
            u.tick();
        }
    }
    const u5 = mk();
    u5.init();
    u5.onMidiMessageInternal([0xb0, 3, 127]);
    for (let i = 0; i < 8; i++) { u5.tick(); u5.onMidiMessageInternal([0xb0, 14, 1]); }
    ok(bad.length === 0, 'nothing is drawn outside 128x64' +
        (bad.length ? ' -- ' + bad.slice(0, 4).join('; ') : ''));
}

console.log(fails ? 'FAILED' : 'PASS');
process.exit(fails ? 1 : 0);

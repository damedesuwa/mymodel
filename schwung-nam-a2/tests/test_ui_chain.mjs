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
const SPECS_FIXTURE = path.join(here, 'fixtures', 'fx_specs.json');
let specsRaw;
try { specsRaw = fs.readFileSync(SPECS_FIXTURE, 'utf8').trim(); }
catch (e) {
    console.log('FAIL ' + SPECS_FIXTURE + ' missing - run tests/run_contract_test.sh first');
    process.exit(1);
}

const params = {
    /* THE BOARD IS SIXTEEN COLUMNS NOW, so the main row starts at 16,
     * not 8 - slotOf(ROW_MAIN, 0) is row*NUM_COLS. At 8 this named the
     * TOP row's ninth column, which is off the path on a mono board. */
    sel_block: '16', cpu: '41', build: 'test',  /* main row, column 1 */
    split: '0', pan_a: '-1.0', pan_b: '1.0',
    /* Two in models/ (Amp) and two in a category folder, which is what
     * the plugin's scan produces: root first, then each folder. */
    model_list: JSON.stringify(['Recto', 'Plexi', 'My RAT', 'My Muff']),
    model_dirs: JSON.stringify(['amps','amps','dist','fuzz']),
    cab_list: JSON.stringify(['TF MESA']),
    ir_list: JSON.stringify(['Big Hall', 'Plate A']),
    /* THE PEDAL TABLE COMES FROM THE PLUGIN, not from a copy written out
     * here. tests/run_contract_test.sh compiles the real plugin and writes
     * what it serves into the fixture; this reads it. A hand-written table
     * would pass this test while disagreeing with the binary, which is the
     * exact failure the served spec exists to prevent. */
    fx_list: JSON.stringify(JSON.parse(specsRaw).map(p => p.n)),
    fx_specs: specsRaw,
    /* The plugin's scan counter. See the list_gen block at the end. */
    list_gen: '0',
};
/* TWO ROWS: `b<n>` is the bottom (main) chain, `t<n>` the top (parallel)
 * one. The top row starts EMPTY, which is what makes the board mono - the
 * fork is derived from it, not stored. */
for (const r of ['b', 't']) for (let b = 1; b <= 16; b++) {
    params[`${r}${b}_type`] = (r === 'b') ? (b <= 2 ? '1' : (b === 3 ? '2' : '0')) : '0';
    params[`${r}${b}_on`] = '0';              /* 0 = On */
    params[`${r}${b}_model`] = '0';
    params[`${r}${b}_quality`] = '0';
    params[`${r}${b}_cab`] = '0';
    params[`${r}${b}_cpu`] = '7';
    params[`${r}${b}_fx`] = '0';
    for (let k = 1; k <= 5; k++) params[`${r}${b}_p${k}`] = '0.5';
}
/* Every scenario below sets the board it needs; this puts BOTH rows back
 * to a known state first, so a test cannot inherit a fork from the one
 * above it - which is exactly how the lane assertions used to pass for
 * the wrong reason. */
const board = (spec) => {
    for (const r of ['b', 't']) for (let b = 1; b <= 16; b++) {
        params[`${r}${b}_type`] = '0';
        params[`${r}${b}_on`] = '0';
    }
    for (const k of Object.keys(spec || {})) {
        params[`${k}_type`] = String(spec[k].t !== undefined ? spec[k].t : spec[k]);
        if (spec[k].on !== undefined) params[`${k}_on`] = spec[k].on ? '0' : '1';
        if (spec[k].fx !== undefined) params[`${k}_fx`] = String(spec[k].fx);
    }
};




const writes = [];
const drawn = [];
const reads = [];       /* every key the module asks the plugin for */
const drawnAt = [];     /* the same text, with where it landed */
const rects = [];
const leds = {};
const buttonLeds = {};
let padBlock = -1;
const claimed = [];     /* every host_claim_ccs call, in order */
const actionsRun = [];

const host = {
    host_module_get_param: (k) => { reads.push(k); return (k in params ? params[k] : null); },
    host_module_set_param: (k, v) => { params[k] = String(v); writes.push([k, String(v)]); },
    host_pad_block: (v) => { padBlock = v; },
    host_claim_ccs: (a) => { claimed.push(Array.isArray(a) ? a.slice() : a); },
    clear_screen: () => drawn.push('<clear>'),
    print: (x, y, t) => { drawn.push(String(t)); drawnAt.push([x, y, String(t)]); },
    draw_rect: (x, y, w, h, c) => rects.push(['draw', x, y, w, h, c]),
    fill_rect: (x, y, w, h, c) => rects.push(['fill', x, y, w, h, c]),
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

/* ONE CLOCK, SKEWED, FOR THE WHOLE FILE.
 *
 * Two tests used to override Date.now locally to fire a pad hold, and the
 * rest advanced a shared skew. That leaves timestamps recorded against one
 * clock being compared with another: a 1.1 second message set at
 * real+2100 outlived a check made at real+2050, and the failure landed on
 * an unrelated assertion about the footer. One monotonic clock, advanced
 * in one way, or the timing in these tests is not testing anything. */
let clockSkew = 0;
const realClock = Date.now;
Date.now = () => realClock() + clockSkew;
const advance = (ms) => { clockSkew += ms; };

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
ok(drawn.some(t => t === 'Amp'), 'pedalboard draws block types');

/* A pad TAP stomps. */
writes.length = 0;
ui.onMidiMessageInternal([0x90, 68, 127]);
ui.onMidiMessageInternal([0x80, 68, 0]);
ok(writes.some(([k, v]) => k === 'b1_on' && v === '1'), 'pad tap bypasses the block');

/* A pad HOLD selects. */
writes.length = 0;
ui.onMidiMessageInternal([0x90, 70, 127]);
advance(1000);                              /* past HOLD_MS */
ui.tick();
/* Pad 70 is the main row's third column, and the main row starts at
 * NUM_COLS. Sixteen columns makes that 18, where eight made it 10 - the
 * slot number is row*NUM_COLS + col and both halves moved. */
ok(writes.some(([k, v]) => k === 'sel_block' && v === '18'), 'pad hold selects the block');
ui.onMidiMessageInternal([0x80, 70, 0]);

/* THE MENU. */
drawn.length = 0;
ui.onMidiMessageInternal([0xb0, 3, 127]);   /* jog click */
ui.tick();
ok(drawn.some(t => t === 'Remove Module'), 'jog click opens the menu with Remove Module');
ok(drawn.some(t => t === 'Preset: (none)'), 'preset row carries its value');

/* Scroll to Remove Module and run it.
 *
 * BY LABEL, never by counting. This used to restate the row list and index
 * into it, so adding a row anywhere above (the Tuner) moved the cursor one
 * short and the click ran the wrong action - a copy of the menu that agrees
 * with itself and with nothing else, which is the reason menuGoTo exists. */
ok(menuGoTo(ui, 'Remove Module', () => { drawn.length = 0; ui.tick(); }),
   'cursor reaches Remove Module');
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
ok(rects.some(r => r[0] === 'fill' && r[1] === 0 && r[3] === 2),
   'Close returns to the pedalboard');

/* --- the three-knob tree ---------------------------------------------- */
/* Coarse to fine: knob 1 the kind, knob 2 the family, knob 3 the pedal. */
{
    /* Block 4 is Off; make it an FX block with knob 1. */
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 71, 127]);       /* hold pad 4 */
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 71, 0]);
    ok(writes.some(([k, v]) => k === 'sel_block' && v === '19'), 'tree: pad 4 selected');

    /* ONE STEP IS THREE DETENTS on every discrete control - the encoders
     * are not detented and the shim coalesces, so a message carries
     * however many ticks arrived in that audio frame. */
    /* DETENTS_PER_STEP in ui_chain.js. It went 3 -> 5 when the knobs came
     * back as still too sensitive to find anything with; this file has to
     * move with it or every discrete assertion tests the old scale. */
    const STEP = 5;
    const turn = (cc, dir, steps = 1) => {
        for (let i = 0; i < steps * STEP; i++)
            ui.onMidiMessageInternal([0xb0, cc, dir > 0 ? 1 : 127]);
    };
    const BAR_Y = 52;
    const K1 = 71, K2 = 72, K3 = 73, K4 = 74, K5 = 75, K6 = 76, K7 = 77, K8 = 78;
    /* draw() is throttled to 33 ms, so a burst of ticks in one millisecond
     * paints ONCE and every capture after it comes back empty - which
     * reads as "the feature draws nothing". Move the clock instead. */
    const repaint = (n = 1) => {
        for (let i = 0; i < n; i++) { advance(50); ui.tick(); }
    };
    /* Off NAM Cab OD Dist Fuzz Boost Dyn Filter Mod Time Pitch */
    /* Delay and Reverb are two lists now, so `reverbs/` lands somewhere
     * with the same name on screen. */
    const CAT = { OFF:0, AMP:1, CAB:2, OD:3, DIST:4, FUZZ:5, BOOST:6,
                  DYN:7, FILTER:8, MOD:9, DELAY:10, REVERB:11, PITCH:12 };
    const gotoCat = (c) => { turn(K1, -1, 16); turn(K1, +1, c); };

    /* The sensitivity itself, which is the thing that was wrong twice:
     * four detents must move NOTHING, and the fifth must move exactly
     * one. */
    turn(K1, -1, 16);                                /* park on Off */
    writes.length = 0;
    for (let i = 0; i < STEP - 1; i++) ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(writes.length === 0, 'knob: a part turn does not make a step');
    ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(params['b4_type'] === '1', 'knob: the last detent makes exactly one step (Off->NAM)');

    /* A BURST IS CLIPPED. The shim coalesces, so one CC can carry ten
     * ticks - and at three ticks a step that was three items per message
     * on a fifty-entry list. The cap is what stops a real turn crossing
     * the whole list faster than it can be read. */
    turn(K1, -1, 16);                                /* park on Off */
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K1, 20]);        /* twenty ticks at once */
    ok(params['b4_type'] === '0',
       'knob: one huge message cannot step further than the cap allows');
    /* Re-select, which drops the part-turn that message banked - a stale
     * remainder would silently shift every count below by one. */
    ui.onMidiMessageInternal([0x90, 71, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 71, 0]);

    /* ONE KNOB NOW DECIDES WHAT THE BLOCK IS. Type and family were two
     * encoders and are one, which is what freed the fifth parameter slot
     * the 1176 and the Dual Delay need. */
    gotoCat(CAT.OD);
    ok(params['b4_type'] === '3' && params['b4_fx'] === '0',
       'cat: OD sets type=FX and lands on the first pedal');
    turn(K2, +1, 4);
    ok(params['b4_fx'] === '4', 'cat: knob 2 walks along the ODs to the Centaur');
    turn(K2, +1, 9);
    ok(params['b4_fx'] === '41', 'cat: and stops on the last one, the Plexi');
    gotoCat(CAT.DIST);
    ok(params['b4_fx'] === '5', 'cat: Dist starts at the RAT');

    /* THE KNOBS ARE THE REAL PEDAL'S, read from the served table. */
    gotoCat(CAT.DYN);
    ok(params['b4_fx'] === '13', 'dyn: starts at the 1176');
    /* Four cells, eight encoders: the row follows the knob last touched,
     * so reaching the fifth control is what brings it on screen. */
    for (const [cc, label] of [[K3, 'Input'], [K4, 'Attac'], [K5, 'Relea'],
                               [K6, 'Ratio'], [K7, 'Outpu']]) {
        ui.onMidiMessageInternal([0xb0, cc, 0]);     /* claim, move nothing */
        drawn.length = 0; ui.tick();
        ok(drawn.some(t => String(t).startsWith(label)),
           `1176: the panel names ${label}`);
    }

    /* FIVE PARAMETERS FIT. This is the whole reason the type knob was
     * merged away - the 1176 has five controls and a five-knob pedal plus
     * two navigation knobs plus the output level is exactly eight. */
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K7, 1]);
    ok(writes.some(([k]) => k === 'b4_p5'), '1176: its fifth knob reaches p5');
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K8, 1]);
    ok(writes.some(([k]) => k === 'out_level'), 'board: knob 8 is Output, always');
    ok(!writes.some(([k]) => k.indexOf('b4_') === 0), 'board: it is not a block key');

    /* A SWITCH STEPS AND PRINTS ITS POSITION, not a number. */
    params['b4_p4'] = '0';
    ui.init();
    drawn.length = 0; ui.tick();
    ok(drawn.some(t => String(t) === '4:1'), 'enum: Ratio draws its option, not 0');

    /* MS IS PRINTED AS MS. A rate that reads 37 is a rate you cannot set
     * against a tempo, which is the whole of what was asked for. */
    gotoCat(CAT.MOD);
    ok(params['b4_fx'] === '23', 'mod: starts at the Chorus');
    params['b4_p1'] = '0.5';
    ui.init();
    drawn.length = 0; ui.tick();
    ui.onMidiMessageInternal([0xb0, K3, 0]);
    drawn.length = 0; ui.tick();
    ok(drawn.some(t => /^[\d.]+(ms|s|m)$/.test(String(t))),
       'ms: the rate cell prints a period with its unit, not a percentage');

    /* A ONE-KNOB PEDAL HAS ONE KNOB. */
    turn(K2, +1, 4);
    ok(params['b4_fx'] === '26', 'mod: reaches the Phase 90');
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K4, 1]);
    ok(writes.length === 0, 'Phase 90: encoder 4 is claimed and does nothing');
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K3, 1]);
    ok(writes.some(([k]) => k === 'b4_p1'), 'Phase 90: encoder 3 is its Speed');

    /* MODE RE-LABELS ITS NEIGHBOUR. The delay's Time is milliseconds until
     * Mode says Note, and then the same encoder is a division - which is
     * only safe because the alternate travels in the plugin's own table. */
    gotoCat(CAT.DELAY);
    ok(params['b4_fx'] === '30', 'delay: starts at the Digital Delay');
    params['b4_p1'] = '0.5'; params['b4_p4'] = '0';
    ui.init();
    drawn.length = 0; ui.tick();
    ui.onMidiMessageInternal([0xb0, K3, 0]);
    drawn.length = 0; ui.tick();
    ok(drawn.some(t => String(t).startsWith('Time')), 'delay: ms mode names it Time');
    ok(drawn.some(t => /^[\d.]+(ms|s|m)$/.test(String(t))), 'delay: and prints a time');
    params['b4_p4'] = '1';
    ui.init();
    ui.onMidiMessageInternal([0xb0, K3, 0]);
    drawn.length = 0; ui.tick();
    ok(drawn.some(t => String(t) === 'Note'), 'delay: Note mode re-labels the same encoder');
    ok(drawn.some(t => String(t).indexOf('/') > 0),
       'delay: and it now draws a division');

    /* THE FOOTER SAYS WHERE YOU ARE. Forty pedals behind two knobs is a
     * place you can be lost in.
     *
     * Past any flash first: a routing or stomp message owns this line
     * while it is fresh, which is the whole point of it. */
    advance(2000);
    drawn.length = 0; repaint();
    /* The count is not pinned: a category grows by however many captures
     * are on the card, and a test that spelled it out would fail on a
     * device with files rather than on a bug. */
    ok(drawn.some(t => /^Digital Dly {2}Delay 1\/\d+$/.test(String(t))),
       'footer: names the pedal, its family and its place in it');

    /* EVERY CATEGORY HAS ITS OWN PAD COLOUR, and the two brightnesses are
     * one hue - "switched on" and "bypassed" have to read as the SAME
     * pedal, which two unrelated constants would not. */
    {
        const seen = new Map();
        for (let c = 1; c < 12; c++) {
            gotoCat(c);
            /* Pad 4 is the selected one and BLINKS white, so read pad 1 -
             * which carries the same category only if we set it too. */
            const led = CATS_OF(ui)[c];
            ok(led && led[0] !== led[1], `colour: category ${c} has an on and an off`);
            ok(!seen.has(led[0]), `colour: category ${c} is not a repeat`);
            seen.set(led[0], c);
        }
    }

    /* THE RINGS SAY WHICH ENCODERS DO SOMETHING. */
    ok(buttonLeds[K8] !== 0 && buttonLeds[K8] !== undefined, 'rings: Output is lit');
    ok(buttonLeds[K1] !== 0 && buttonLeds[K1] !== undefined, 'rings: Block is lit');
    ok(buttonLeds[K7] === 0, 'rings: an encoder past the pedal is dark');

    /* A KNOB IS A POSITION, AND +0.0 IS NOT ONE. Every cell draws a bar
     * under it; a BIPOLAR control fills from the centre, so a row of EQ
     * bands is a picture of the curve before any number is read. */
    gotoCat(CAT.FILTER);
    turn(K2, +1);                                    /* -> Graphic EQ */
    ok(params['b4_fx'] === '47', 'eq: reaches the Graphic EQ');
    params['b4_p1'] = '1.0';   /* +12 dB */
    params['b4_p2'] = '0.5';   /*   0 dB */
    params['b4_p3'] = '0.0';   /* -12 dB */
    ui.init();
    ui.onMidiMessageInternal([0xb0, K3, 0]);
    repaint();
    rects.length = 0; repaint();
    /* rects are ['fill'|'draw', x, y, w, h, colour]. */
    const barsOf = () => rects.filter(r => r[2] >= 51 && r[2] <= 55);
    const fillAt = (x, w) => barsOf().some(
        r => r[0] === 'fill' && r[2] === BAR_Y && r[1] === x && r[3] >= w);
    const tickAt = (x) => barsOf().some(
        r => r[0] === 'fill' && r[2] === BAR_Y - 1 && r[1] === x && r[3] === 1 && r[4] === 4);
    ok(barsOf().length > 0, 'bar: the cells draw one');
    /* Four cells across; with knob 3 touched the row shows Block, Pedal,
     * then the first two bands. A 31 px cell at x=64 has its centre at 79. */
    ok(fillAt(79, 4), 'bar: a boosted band fills right from its centre');
    ok(tickAt(111), 'bar: a flat band draws the centre tick');
    ok(!fillAt(111, 1), 'bar: and no fill, so the tick is what says where centre is');
    /* Scroll along to the cut band - it must fill LEFT, ending at centre. */
    ui.onMidiMessageInternal([0xb0, K5, 0]);
    repaint();
    rects.length = 0; repaint();
    ok(barsOf().some(r => r[0] === 'fill' && r[2] === BAR_Y &&
                          r[1] + r[3] === 111 && r[3] > 3),
       'bar: a cut band fills left, ending at its own centre');

    /* THE OUTPUT METER. The log carried `out pk=2.02` for a whole session
     * with nothing on the panel saying so. */
    params['peak'] = '50';
    ui.init();
    repaint(8);
    rects.length = 0; repaint();
    ok(rects.some(r => r[0] === 'draw' && r[2] === 2 && r[4] === 6),
       'meter: below full scale it is an outline');
    params['peak'] = '180';
    repaint(8);
    rects.length = 0; repaint();
    ok(rects.some(r => r[0] === 'fill' && r[2] === 2 && r[4] === 6),
       'meter: over full scale it inverts');
    ok(!rects.some(r => r[0] === 'draw' && r[2] === 2 && r[4] === 6),
       'meter: and stops being an outline, so the two cannot be confused');

    /* INPUT LIVES ON THE MENU, and a knob sets it there. Out keeps an
     * encoder because it is the one you ride; In is set once against the
     * guitar and wants to be somewhere it cannot be nudged mid-take. */
    ui.onMidiMessageInternal([0xb0, 3, 127]);        /* jog click: open */
    ok(menuGoTo(ui, 'Input', repaint), 'menu: the cursor reaches Input');
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(writes.some(([k]) => k === 'in_level'), 'menu: a knob sets Input in place');
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(!writes.some(([k]) => k.indexOf('b4_') === 0),
       'menu: and it does not reach the block behind it');

    /* ================================================================ *
     * THE SPLIT                                                        *
     * ================================================================ */

    ui.onMidiMessageInternal([0xb0, 3, 127]);        /* close the menu */

    /* A DERIVED CONTROL IS NOT A PARAMETER. The Block knob reads `cat`,
     * which is computed from type and fx and which the plugin cannot
     * serve - and asking anyway cost a param give-up on the device every
     * time a block changed. */
    reads.length = 0;
    gotoCat(CAT.OD);
    ok(!reads.some(k => /_cat$/.test(k)),
       'reads: nothing asks the plugin for a key it cannot answer');

    /* A NEW BLOCK ARRIVES BYPASSED. Turning knob 1 past Off used to put a
     * pedal straight into the signal at its defaults, and a .nam is the
     * worst case - the device log has `out pk` stepping 0.08 -> 1.32
     * across one load, through headphones, while you are still looking
     * for the pedal. */
    board({});
    ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);       /* hold pad 1 -> select */
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    writes.length = 0;
    for (let i = 0; i < 5; i++) ui.onMidiMessageInternal([0xb0, 71, 1]);
    ok(writes.some(([k, v]) => k === 'b1_type' && v !== '0'),
       'bypass: knob 1 past Off gives the block a type');
    ok(writes.some(([k, v]) => k === 'b1_on' && v === '1'),
       'bypass: and it arrives BYPASSED, not in the signal');
    /* Only on the way OUT of Off - changing a live block's category must
     * not silence it, or auditioning costs a stomp per candidate. */
    writes.length = 0;
    for (let i = 0; i < 5; i++) ui.onMidiMessageInternal([0xb0, 71, 1]);
    ok(writes.some(([k]) => k === 'b1_type'),
       'bypass: the next category still lands');
    ok(!writes.some(([k]) => k === 'b1_on'),
       'bypass: but a live block is not re-bypassed under you');

    /* A CAPTURE SITS INSIDE THE CATEGORY IT IS A CAPTURE OF.
     *
     * `models/` was flat and had one picker called NAM, so a capture of a
     * RAT was behind a menu entry saying "amp" while the modelled RAT was
     * in Dist. A subfolder is the category now, loose files stay Amp, and
     * knob 2 walks pedals and captures together. */
    board({ b1: 0 });
    ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    turn(K1, +1, CAT.DIST);                 /* Off -> ... -> Dist */
    /* Past every modelled Dist pedal and onto the capture in models/dist. */
    writes.length = 0;
    turn(K2, +1, 20);
    ok(writes.some(([k, v]) => k === 'b1_type' && v === '1'),
       'nam: walking off the end of Dist reaches a capture');
    ok(writes.some(([k, v]) => k === 'b1_model' && v === '2'),
       'nam: and it is the one in models/dist, by index');
    drawnAt.length = 0; repaint();
    /* Clipped to the cell, as every name on this row is - the footer is
     * where the full one lives. */
    ok(drawnAt.some(d => /^My RA/.test(d[2])),
       'nam: the picker names the file, not an index');
    ok(drawnAt.some(d => d[2] === 'Dist'),
       'nam: and the block is still in Dist, not in Amp');
    /* Back down past it returns to the modelled pedals - one list. */
    writes.length = 0;
    turn(K2, -1, 20);
    ok(writes.some(([k, v]) => k === 'b1_type' && v === '3'),
       'nam: turning back reaches the modelled pedals again');
    /* A capture in no folder is an Amp, which is what every install that
     * predates this has. */
    board({ b1: 1 });                               /* TYPE_NAM */
    params['b1_model'] = '0';                       /* models/ root */
    ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[2] === 'Amp'), 'nam: a loose capture is an Amp');
    ok(drawnAt.some(d => d[2] === 'Recto'), 'nam: named by its file');
    ok(!drawnAt.some(d => d[2] === 'Model'),
       'nam: and the separate Model picker is gone - one knob does both');

    /* THE TONE STACK IS THE AMP'S OWN KNOBS, so it lives in Amp - but at
     * the END of it. Landing the category on its tail would land `Amp` on
     * something that is not an amp. */
    board({ b1: 0 });
    ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    writes.length = 0;
    turn(K1, +1, CAT.AMP);
    ok(writes.some(([k, v]) => k === 'b1_type' && v === '1'),
       'stack: Amp still lands on a capture, not on the stack');
    /* EXACTLY past the two captures, not to the end: the tail holds two
     * now, and a walk that runs off it lands on the other one. */
    writes.length = 0;
    turn(K2, +1, 2);
    ok(writes.some(([k, v]) => k === 'b1_fx' && v === '50'),
       'stack: and it is after the captures in the Amp list');
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[2] === 'Bass'), 'stack: its page names Bass');
    ui.onMidiMessageInternal([0xb0, K5, 1]);
    ui.onMidiMessageInternal([0xb0, K6, 1]);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[2] === 'Mid'), 'stack: and Mid');
    ok(drawnAt.some(d => /^Trebl/.test(d[2])), 'stack: and Treble');

    /* THE POWER STAGE IS THE OTHER TAIL ENTRY. What a capture cannot
     * have: a .nam is a static function and these are its memory. */
    writes.length = 0;
    turn(K2, +1, 1);
    ok(writes.some(([k, v]) => k === 'b1_fx' && v === '51'),
       'pa: the Power Amp is next to the Tone Stack');
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[2] === 'Sag'), 'pa: its page names Sag');
    ui.onMidiMessageInternal([0xb0, K4, 1]);
    ui.onMidiMessageInternal([0xb0, K5, 1]);
    drawnAt.length = 0; repaint();
    for (const want of ['Bloom', 'Bias'])
        ok(drawnAt.some(d => d[2] === want), 'pa: and ' + want);
    /* Bloom is a TIME, so it reads as one. */
    ok(drawnAt.some(d => /ms$/.test(d[2])), 'pa: Bloom reads in milliseconds');

    /* RE-OPENING MUST NOT GROW THE LIST. A tail entry that survived the
     * rebuild was appended again on every init, and since items[0] is
     * what a category LANDS on, `Amp` stopped landing on an amp the
     * second time the module was opened. The symptom is the assertion. */
    board({ b1: 0 });
    ui.init(); ui.init(); ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    writes.length = 0;
    turn(K1, +1, CAT.AMP);
    ok(writes.some(([k, v]) => k === 'b1_type' && v === '1'),
       'stack: and after three opens Amp still lands on a capture');

    /* A REVERB IR IS A BLOCK TYPE, and it lands in Time where the
     * modelled reverbs already are. The difference from a cab is the tap
     * count and that the dry survives. */
    board({ b1: 0 });
    ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    turn(K1, +1, CAT.REVERB);
    writes.length = 0;
    turn(K2, +1, 20);                       /* past the delays and verbs */
    ok(writes.some(([k, v]) => k === 'b1_type' && v === '4'),
       'ir: walking off the end of Reverb reaches an IR');
    ok(writes.some(([k, v]) => k === 'b1_ir' && v === '0'),
       'ir: and picks it out of irs/, by index');
    drawnAt.length = 0; repaint();
    for (const want of ['Mix', 'Len'])
        ok(drawnAt.some(d => d[2] === want), 'ir: the reverb page names ' + want);
    ui.onMidiMessageInternal([0xb0, K5, 1]);
    ui.onMidiMessageInternal([0xb0, K6, 1]);
    drawnAt.length = 0; repaint();
    for (const want of ['Pre', 'LoCut'])
        ok(drawnAt.some(d => d[2] === want), 'ir: the reverb page names ' + want);
    /* LENGTH IS A CPU KNOB WITH A TIME LABEL, so it says the time. */
    ok(drawnAt.some(d => /^(46|93|186|372)ms$/.test(d[2])) ||
       drawnAt.some(d => /ms$/.test(d[2])),
       'ir: and Len reads as a time, not an index');
    /* It must NOT wear the cab's mic knobs - a room has no mic to move. */
    ok(!drawnAt.some(d => d[2] === 'Phase'),
       'ir: a room is not a cabinet, so no mic controls');

    /* A CAB BLOCK CARRIES THE MIC, on the five encoders a cab never used.
     * The wire already has p1..p5 per slot, so this needed no new keys -
     * they route by the block's TYPE on the plugin side. */
    board({ b1: 2 });                                /* TYPE_CAB */
    /* As the plugin serves a fresh cab: at the cap, at the captured
     * distance, no added character. NOT the knobs' midpoints - 0.5 on
     * Mic would be a 421 - so these come from mic_defaults and the
     * contract test pins that they do. */
    params['b1_p1'] = '0.0'; params['b1_p2'] = '0.5';
    params['b1_p3'] = '0.0'; params['b1_p4'] = '0.0';
    ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    drawnAt.length = 0; repaint();
    for (const want of ['Pos', 'Dist'])
        ok(drawnAt.some(d => d[2] === want), 'mic: the cab page names ' + want);
    /* The strip shows four cells, so the last two are reached by touching
     * their encoder - the same as any pedal with five controls. */
    ui.onMidiMessageInternal([0xb0, K5, 1]);
    ui.onMidiMessageInternal([0xb0, K6, 1]);
    drawnAt.length = 0; repaint();
    for (const want of ['Mic', 'Phase'])
        ok(drawnAt.some(d => d[2] === want), 'mic: the cab page names ' + want);
    /* WHICH POWER STAGE IS DRIVING THE CAB, which an IR cannot carry. */
    ui.onMidiMessageInternal([0xb0, K7, 1]);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[2] === 'Damp'), 'mic: and Damp');
    ok(drawnAt.some(d => d[2] === 'Off'), 'mic: and Mic is Off by default');
    drawnAt.length = 0;
    ui.onMidiMessageInternal([0xb0, K4, 1]);
    repaint();
    /* Dist is CENTRED on the capture, which is the only zero an IR has,
     * so it reads in signed centimetres and its bar fills from the middle. */
    ok(drawnAt.some(d => /^[+-]?\d+cm$/.test(d[2])),
       'mic: and Dist reads in centimetres, signed');
    writes.length = 0;
    for (let i = 0; i < 20; i++) ui.onMidiMessageInternal([0xb0, K4, 1]);
    ok(writes.some(([k, v]) => k === 'b1_p2' && Number(v) > 0.5),
       'mic: encoder 4 walks the mic away from the cab');

    /* THE TOP ROW IS BLOCKS NOW, not a routing row.
     *
     * It used to carry its own language - tap a dark pad to split, tap a
     * lit one to flip a side, hold anywhere to rejoin - which was three
     * things to decode on one row and cost the eight blocks it sat above
     * nothing but READ them. Both rows take the same two gestures, and
     * the fork is wherever the top row starts. */
    const R = (n) => 76 + n;
    const tapR = (n) => {
        ui.onMidiMessageInternal([0x90, R(n), 127]);
        ui.onMidiMessageInternal([0x80, R(n), 0]);
    };
    const holdR = (n) => {
        ui.onMidiMessageInternal([0x90, R(n), 127]);
        advance(1000); ui.tick();
        ui.onMidiMessageInternal([0x80, R(n), 0]);
    };

    /* A MONO BOARD LEAVES THE TOP ROW DARK, and dark means empty - not a
     * dim preview of a side, which is what made the old row unreadable. */
    board({ b1: 3, b2: 3, b3: 3, b4: 3, b5: 3, b6: 3, b7: 3, b8: 3 });
    ui.init();
    repaint();
    for (let b = 0; b < 8; b++)
        ok(leds[R(b)] === 0, `route: a mono board leaves top pad ${b + 1} dark`);
    for (let b = 0; b < 8; b++)
        ok(leds[68 + b] !== 0, `route: and bottom pad ${b + 1} lit`);

    /* A TOP PAD HOLD SELECTS ITS OWN SLOT, which is how a block gets up
     * there at all - and slot 2 is the top row, column 3. */
    writes.length = 0;
    holdR(2);
    ok(writes.some(([k, v]) => k === 'sel_block' && v === '2'),
       'route: a hold on the top row selects that slot');

    /* PUTTING A BLOCK IN THE TOP ROW IS THE SPLIT. Nothing writes a
     * `split` key - there is no such setting any more. */
    writes.length = 0;
    board({ b1: 3, b2: 3, b3: 3, b4: 3, t3: 3, t4: 3 });
    ui.init();
    repaint();
    ok(!writes.some(([k]) => k === 'split'),
       'route: the fork is derived, so nothing stores one');
    ok(leds[R(0)] === 0 && leds[R(1)] === 0,
       'route: before the fork the top row stays dark');
    ok(leds[R(2)] !== 0 && leds[R(3)] !== 0,
       'route: from the fork its blocks light like any others');

    /* A TOP PAD STOMPS, exactly as a bottom one does. */
    writes.length = 0;
    tapR(2);
    ok(writes.some(([k, v]) => k === 't3_on' && v === '1'),
       'route: a tap on the top row bypasses that block');

    /* THE BLOCK ROW IS UNTOUCHED - tap stomps, hold edits. */
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    ok(writes.some(([k]) => k === 'b1_on'), 'blocks: a tap still stomps');
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 74, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 74, 0]);
    ok(writes.some(([k, v]) => k === 'sel_block' && v === '22'),
       'blocks: a hold still selects it for editing');

    /* AND THE SCREEN SAYS WHAT JUST HAPPENED, naming the ROW. `B4` and
     * `T4` are different blocks on the same column, and a message that
     * said only `B4` would be right half the time. */
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => /^B1 (ON|BYPASS)$/.test(d[2])),
       'blocks: a stomp says which way it went');
    tapR(3);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => /^T4 (ON|BYPASS)$/.test(d[2])),
       'blocks: and a top-row stomp names the top row');
    advance(2000);
    drawnAt.length = 0; repaint();
    ok(!drawnAt.some(d => /^[BT]\d (ON|BYPASS)$/.test(d[2])),
       'route: the message is brief, not a mode');

    /* THE PICTURE ON SCREEN: the two lanes are two ROWS. The lane used to
     * be a letter in a 15 px box and a one-pixel rail; both were true and
     * neither was legible, which is what "I cannot tell which side I am
     * editing" meant. */
    board({ b1: 3, b2: 3, b3: 3, b4: 3, t3: 3, t4: 3 });
    ui.init();
    rects.length = 0; drawnAt.length = 0; repaint();
    const GY = 12, LH = 9, CW = 14, BAND = 19;
    const colX = (c) => 4 + c * (CW + 1);
    /* Height > 1 matters: the WIRE through a column is also COL_W wide,
     * so a finder that only matched the width would report every empty
     * slot as a box and pass whatever the drawing did. */
    const boxesAt = (col) => rects.filter(
        r => (r[0] === 'fill' || r[0] === 'draw') &&
             r[1] === colX(col) && r[3] === CW && r[4] > 1);
    const boxAt = (col) => boxesAt(col)[0];
    /* HEIGHT TELLS THE RAIL FROM THE BAND. A head block also starts at
     * GY - it spans both rails - so matching on y alone reports the head
     * as an upper-rail box and the "nothing before the fork" check below
     * passes on every board. */
    const upperAt = (col) => boxesAt(col).find(r => r[2] === GY && r[4] === LH);
    const lowerAt = (col) => boxesAt(col).find(r => r[2] === GY + LH + 1);

    /* A block BEFORE the fork spans BOTH lanes - one amp visibly feeding
     * two cabs - so its height is the band, not a lane. */
    ok(boxAt(0) && boxAt(0)[2] === GY && boxAt(0)[4] === BAND,
       'split: before the fork a block spans both lanes');
    ok(upperAt(2) && upperAt(2)[4] === LH,
       'split: the TOP row draws on the upper rail');
    ok(lowerAt(2) && lowerAt(2)[4] === LH,
       'split: the BOTTOM row draws on the lower rail');
    ok(upperAt(2)[2] !== lowerAt(2)[2],
       'split: so the two sides are two ROWS, not two letters');
    /* The fork is a wire joining the two lane centres, as Axe-Edit draws a
     * block feeding two cabs. */
    ok(rects.some(r => r[0] === 'fill' && r[3] === 1 && r[4] >= LH),
       'split: and a vertical wire says where they part company');
    ok(!drawnAt.some(d => d[2] === 'L' || d[2] === 'R'),
       'split: no lane letters left in the boxes');
    /* THE BRANCH ROW BEFORE THE FORK IS NOT DRAWN AT ALL. There is no
     * wire there for a box to sit on, so a faint one would be a path
     * that does not exist. */
    ok(!upperAt(0) && !upperAt(1),
       'split: nothing is drawn on the branch before it forks');

    /* THE MERGE IS A SETTING, AND ITS DEFAULT IS THE OUTPUT.
     *
     * It was derived - "right after the last top-row block" - and that
     * moved the topology as a side effect of adding a pedal, which is
     * what was reported as unintuitive. Out by default means a board that
     * forks and never rejoins: nothing moves unless you move it. */
    board({ b1: 3, b2: 3, t2: 3, b5: 3, b6: 3 });
    params['merge'] = '0';
    ui.init();
    rects.length = 0; repaint();
    ok(lowerAt(4) && !boxesAt(4).some(r => r[4] === BAND),
       'merge: at Out a block after the branch stays on its own rail');

    /* SET IT, AND THE TAIL IS SHARED. Two amps into one reverb: the
     * reverb is a single block, after the join, spanning both rails -
     * the same picture as the head, read the other way round. */
    params['merge'] = '4';       /* column 4 is one signal again */
    ui.init();
    rects.length = 0; drawnAt.length = 0; repaint();
    ok(upperAt(1) && lowerAt(1),
       'merge: the parallel section is two rails');
    ok(!upperAt(4) && !lowerAt(4),
       'merge: past the merge column there is only one rail');
    ok(boxAt(4) && boxAt(4)[2] === GY && boxAt(4)[4] === BAND,
       'merge: a block in the tail spans both rails, as the head does');
    /* TWO verticals, not one: the fork AND the join. Drawing the parting
     * and leaving the meeting implied is what made "where does this come
     * back together" a question. */
    const rails = rects.filter(r => r[0] === 'fill' && r[3] === 1 && r[4] >= LH);
    ok(rails.length >= 2, 'merge: the join is drawn exactly as the fork is');
    ok(rails.some(r => r[1] > 4 + 1 * (CW + 1)),
       'merge: and it sits AFTER the branch, not at it');

    /* THE ROW SAYS WHERE, and says "Out" rather than "Off" - the lanes do
     * merge when it is not set, at the output, and "Off" would say they
     * never meet. */
    ui.onMidiMessageInternal([0xb0, 3, 127]);
    ok(menuGoTo(ui, 'Merge', repaint), 'merge: it is a menu row');
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => /after 4/.test(d[2])), 'merge: naming how far the branch runs');
    /* AND A KNOB SETS IT IN PLACE, like every other value row. */
    writes.length = 0;
    for (let i = 0; i < 9; i++) ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(writes.some(([k, v]) => k === 'merge' && Number(v) > 4),
       'merge: a knob moves it along the board');
    ui.onMidiMessageInternal([0xb0, 3, 127]);     /* close */

    /* THE JOG IS THE GESTURE, because the jog is the one control on this
     * screen that is PROVEN to arrive - the device log has its CLICK
     * opening this module's own menu. Shift + a pad came back as not
     * working, with nothing in the log either way. */
    params['merge'] = '0';
    ui.init();
    writes.length = 0;
    for (let i = 0; i < 6; i++) ui.onMidiMessageInternal([0xb0, 14, 1]);
    ok(writes.some(([k, v]) => k === 'merge' && v === '3'),
       'merge: the jog walks it along the board');
    writes.length = 0;
    for (let i = 0; i < 6; i++) ui.onMidiMessageInternal([0xb0, 14, 127]);
    ok(writes.some(([k, v]) => k === 'merge' && v === '0'),
       'merge: and back down to Out');
    /* It must not keep going below Out, or the value the plugin holds and
     * the value this screen shows part company at the bottom. */
    writes.length = 0;
    for (let i = 0; i < 6; i++) ui.onMidiMessageInternal([0xb0, 14, 127]);
    ok(!writes.some(([k]) => k === 'merge'),
       'merge: Out is the floor, and a write past it is not sent');
    /* THE FOOTER CARRIES IT while the board is forked - the flash says
     * "did that take", not "where is it now". */
    board({ b1: 3, b2: 3, t2: 3 });
    params['merge'] = '4';
    ui.init();
    /* Past the flash, which owns the whole footer line while it is up. */
    advance(2000);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[2] === 'M:4'), 'merge: the footer carries it');
    board({ b1: 3, b2: 3 });
    ui.init();
    drawnAt.length = 0; repaint();
    ok(!drawnAt.some(d => /^M:/.test(d[2])),
       'merge: and says nothing on a board with one lane');

    /* A BRANCH SLOT PAST THE MERGE IS DEAD - no light, no stomp, no edit.
     * `inPath` was checked by the DRAW path alone, so those slots drew no
     * box while their pads stayed lit and took both gestures. */
    board({ b1: 3, b2: 3, t2: 3, t6: 3 });
    params['merge'] = '3';                       /* parallel to column 3 */
    ui.init();
    repaint();
    ok(leds[76 + 1] !== 0, 'dead: a branch pad inside the fork is lit');
    ok(leds[76 + 5] === 0, 'dead: one past the merge is dark');
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 76 + 5, 127]);
    ui.onMidiMessageInternal([0x80, 76 + 5, 0]);
    ok(!writes.some(([k]) => k === 't6_on'), 'dead: and a tap does not stomp it');
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 76 + 5, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 76 + 5, 0]);
    ok(!writes.some(([k]) => k === 'sel_block'), 'dead: nor a hold select it');
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => /PAST MERGE/.test(d[2])),
       'dead: and the screen says why rather than doing nothing');
    /* BEFORE the fork is a different region: empty by construction, and
     * holding it is the only way to move the fork earlier. */
    writes.length = 0;
    board({ b1: 3, b2: 3, t3: 3 });
    params['merge'] = '0';
    ui.init();
    ui.onMidiMessageInternal([0x90, 76 + 0, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 76 + 0, 0]);
    ok(writes.some(([k, v]) => k === 'sel_block' && v === '0'),
       'dead: but before the fork a hold still selects, or nothing can move it');

    /* SHIFT + A PAD IS GONE. It did this too for one release and could
     * not be shown to arrive; once the jog worked it was strictly
     * downside - a stray Shift under a pad press would silently re-route
     * the board instead of stomping the block. */
    board({ b1: 3, b2: 3, t2: 3 });
    params['merge'] = '0';
    ui.init();
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, 49, 127]);             /* shift down  */
    ui.onMidiMessageInternal([0x90, 76 + 5, 127]);         /* top pad 6   */
    ui.onMidiMessageInternal([0x80, 76 + 5, 0]);
    ok(!writes.some(([k]) => k === 'merge'),
       'merge: shift + a pad no longer re-routes the board');
    ui.onMidiMessageInternal([0xb0, 49, 0]);

    params['merge'] = '0';

    /* AN EMPTY SLOT IS A WIRE, NOT A BOX. Five empty boxes with a dash in
     * each is what "messy" meant: the three blocks that were there had to
     * be found among them. */
    board({ b1: 3 });
    ui.init();
    rects.length = 0; drawnAt.length = 0; repaint();
    ok(boxAt(0), 'grid: a loaded block still draws a box');
    ok(!boxAt(3) && !boxAt(5), 'grid: an empty slot draws no box at all');
    ok(!drawnAt.some(d => d[2] === '-'), 'grid: and no dash to read either');
    /* But the path is unbroken: input bar, wire, output bar. */
    ok(rects.some(r => r[0] === 'fill' && r[1] === 0 && r[3] === 2),
       'grid: the input is part of the picture');
    ok(rects.some(r => r[0] === 'fill' && r[1] === 126 && r[3] === 2),
       'grid: so is the output');
    ok(rects.some(r => r[0] === 'fill' && r[4] === 1 && r[3] > 5 &&
                       r[2] === GY + (16 >> 1)),
       'grid: and a wire runs the whole way between them');

    /* A BYPASSED BLOCK HAS NO SIDE WALLS AND THE WIRE GOES THROUGH IT.
     *
     * The letter B in a fourteen-pixel box cost a session to "no sound,
     * the amp is dead". A missing pair of walls is a difference of SHAPE,
     * legible without reading anything. */
    /* `b<N>_on` is the wire's BYPASS flag - 0 is live - so this is one
     * bypassed block on an otherwise live board. */
    for (let b = 1; b <= 8; b++) { params[`b${b}_type`] = '3'; params[`b${b}_on`] = '0'; }
    params['b2_on'] = '1';
    ui.init();
    rects.length = 0; repaint();
    const wallAt = (c) => rects.some(
        r => r[0] === 'fill' && r[1] === colX(c) && r[3] === 1 && r[4] === 16);
    ok(wallAt(0), 'grid: a block in circuit is a closed box');
    ok(!wallAt(1), 'grid: a bypassed one has no side walls');
    ok(rects.some(r => r[0] === 'fill' && r[1] === colX(1) &&
                       r[3] === CW && r[4] === 1 && r[2] === GY + 8),
       'grid: and the wire runs straight through it');

    /* WHICH SIDE YOU ARE EDITING is a different question from which side
     * a block is on, and it was the one going unanswered. */
    board({ b1: 3, b2: 3, b3: 3, b4: 3, t3: 3, t4: 3 });
    params['merge'] = '0';                           /* lanes run to the out */
    ui.init();
    ui.onMidiMessageInternal([0x90, 71, 127]);       /* hold pad 4 -> select */
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 71, 0]);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[1] === 1 && /^B4 R/.test(d[2])),
       'split: the header names the row AND where it is panned');
    ui.onMidiMessageInternal([0x90, 79, 127]);       /* hold top pad 4 */
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 79, 0]);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[1] === 1 && /^T4 L/.test(d[2])),
       'split: and the other row says the other side');


    /* PAN, in its own units - a position between two names. */
    ui.onMidiMessageInternal([0xb0, 3, 127]);        /* open */
    ok(menuGoTo(ui, 'Pan Top', repaint), 'split: Pan Top is a row');
    writes.length = 0;
    for (let i = 0; i < 5; i++) ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(writes.some(([k, v]) => k === 'pan_a' && Number(v) > -1 && Number(v) <= 1),
       'pan: the knob moves the top rail off hard left');
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => /^(L|R)\d+$|^C$/.test(d[2])),
       'pan: and it reads as a position, not a percentage');
    ui.onMidiMessageInternal([0xb0, 3, 127]);        /* close */
}

/* WHICH ROW THE CURSOR IS ON, asked of the picture.
 *
 * The tests used to count detents from the bottom of the menu, which was
 * fine until the menu grew three rows and every one of those counts was
 * silently off by three. The menu draws a full-width highlight behind the
 * cursor; whatever text shares that row's y IS the row. Counting is
 * replaced by looking.
 */
function cursorRow(ui, repaint) {
    drawnAt.length = 0; rects.length = 0;
    repaint();
    const hl = rects.find(r => r[0] === 'fill' && r[1] === 0 && r[3] === 128 && r[4] === 10);
    if (!hl) return null;
    const y = hl[2] + 1;
    const t = drawnAt.find(d => d[1] === y);
    return t ? t[2] : null;
}

/* Scroll to a named row. Walks DOWN from the top, which terminates: the
 * cursor clamps at the last row, so a label that is not there fails by
 * running out of steps rather than by looping. */
function menuGoTo(ui, label, repaint) {
    for (let i = 0; i < 40; i++) ui.onMidiMessageInternal([0xb0, 14, 127]);
    for (let i = 0; i < 40; i++) {
        if (cursorRow(ui, repaint) === label) return true;
        ui.onMidiMessageInternal([0xb0, 14, 1]);
    }
    return false;
}

/* The category palette, read out of the source rather than restated here -
 * a copy would agree with itself and with nothing else. */
function CATS_OF() {
    const m = /const CATS = \[([\s\S]*?)\n\];/.exec(src);
    if (!m) return [];
    return [...m[1].matchAll(/led:\s*\[(\d+),\s*(\d+)\]/g)]
        .map(x => [Number(x[1]), Number(x[2])]);
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
    const seen = [], seenAt = [], seenRects = [];
    h2.print = (x, y, t) => { seen.push(String(t)); seenAt.push([x, y, String(t)]); };
    h2.fill_rect = (x, y, w, h) => seenRects.push([x, y, w, h]);
    h2.draw_rect = () => {};
    h2.clear_screen = () => {};
    const n2 = Object.keys(h2);
    /* buttonLeds has to be here too: the stub closes over it, so leaving
     * it out is a ReferenceError inside tick() - which tickBody catches
     * and paints as an error screen, AFTER the menu has already been
     * drawn. The assertions below would still pass while the module was
     * throwing on every frame. */
    const ui2 = new Function('globalThis', 'leds', 'buttonLeds', ...n2,
        stubs + src + '\nreturn globalThis.chain_ui;')({}, {}, {}, ...n2.map(n => h2[n]));
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

    /* Land ON it by NAME. Counting detents from either end of the menu
     * was right until the menu grew three rows, and then every count was
     * silently off by three - a test that passes for the wrong reason
     * right up until it fails for one. */
    for (let i = 0; i < 30; i++) ui2.onMidiMessageInternal([0xb0, 14, 127]);
    let landed = false;
    for (let i = 0; i < 30 && !landed; i++) {
        seenRects.length = 0; seenAt.length = 0;
        ui2.tick();
        const hl = seenRects.find(r => r[0] === 0 && r[2] === 128 && r[3] === 10);
        if (hl) {
            const row = seenAt.find(d => d[1] === hl[1] + 1);
            if (row && row[2] === 'Swap / Remove...') { landed = true; break; }
        }
        ui2.onMidiMessageInternal([0xb0, 14, 1]);
    }
    ok(landed, `host gives ${what} -> the cursor lands on Swap / Remove`);
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
    /* Into drawnAt as well, or cursorRow() cannot see this instance at all
     * and menuGoTo answers null for every row. */
    h3.print = (x, y, t) => { seen.push(String(t)); drawnAt.push([x, y, String(t)]); };
    h3.clear_screen = () => {};
    const n3 = Object.keys(h3);
    const ui3 = new Function('globalThis', 'leds', 'buttonLeds', ...n3,
        stubs + src + '\nreturn globalThis.chain_ui;')({}, {}, {}, ...n3.map(n => h3[n]));
    ui3.init();
    ui3.onMidiMessageInternal([0xb0, 3, 127]);
    ui3.tick();
    ok(seen.includes('Swap / Remove...'), 'old host -> Swap / Remove offered');
    ok(!seen.includes('Save As'), 'old host -> no inert rows offered');
    /* Scrolled to, not assumed to be row 0. On an old host Swap / Remove is
     * the only row that does anything, which made it row 0 and made this
     * test pass on a coincidence until the Tuner row went in above it. */
    ok(menuGoTo(ui3, 'Swap / Remove...', () => { seen.length = 0; ui3.tick(); }),
       'old host -> the cursor reaches Swap / Remove');
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
    const mk = () => new Function('globalThis', 'leds', 'buttonLeds', ...n4,
        stubs + src + '\nreturn globalThis.chain_ui;')({}, {}, {}, ...n4.map(n => h4[n]));

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

    /* AND THE TUNER SCREEN, at both ends of its needle and with the widest
     * note it can print. It is drawn from arithmetic on a reading, which is
     * exactly the shape that put text off the panel before. */
    const u6 = mk();
    u6.init();
    u6.onMidiMessageInternal([0xb0, 3, 127]);     /* menu; Tuner is row 0 */
    u6.tick();
    u6.onMidiMessageInternal([0xb0, 3, 127]);     /* open it */
    for (const rep of ['{"hz":0,"lvl":0,"conf":0}',
                       '{"hz":0,"lvl":0.02,"conf":0.1}',
                       '{"hz":82.41,"note":"E","oct":2,"cents":0,"conf":1,"lvl":0.2}',
                       '{"hz":80.0,"note":"E","oct":2,"cents":-99,"conf":1,"lvl":0.2}',
                       '{"hz":340.0,"note":"F","oct":4,"cents":99,"conf":1,"lvl":0.9}',
                       '{"hz":4978.03,"note":"D#","oct":8,"cents":-49.9,"conf":1,"lvl":0.5}',
                       '{"hz":12543.85,"note":"G","oct":9,"cents":49.9,"conf":1,"lvl":1}']) {
        params.tuner = rep;
        for (let i = 0; i < 4; i++) { advance(50); u6.tick(); }
    }
    delete params.tuner;
    ok(bad.length === 0, 'nothing is drawn outside 128x64' +
        (bad.length ? ' -- ' + bad.slice(0, 4).join('; ') : ''));
}

/* --- list_gen: a file landed on the card while this screen was up ------ */
/* The lists were read once on entry, and that was sound while the card
 * could not change underneath. It can now - the web manager writes it with
 * the module playing - so the rotation watches one cheap counter.
 *
 * The first claim is why it is a counter and not an unconditional re-read:
 * loadLists() is five IPC reads, ~14 ms, which spent every lap is most of
 * the read budget gone on waiting for something that moves when a FILE is
 * uploaded. `attachNamItems` rebuilding the Amp walk from a new model_list
 * is covered above; what is new here is WHEN it is asked to. */
{
    const mkUi = () => new Function('globalThis', 'leds', 'buttonLeds',
        ...names, stubs + src + '\nreturn globalThis.chain_ui;')(
        {}, leds, buttonLeds, ...names.map(n => host[n]));
    const LAP = 70;        /* a full rotation is NUM_BLOCKS*2+1 = 65 stops */

    params.list_gen = '0';
    params.model_list = JSON.stringify(['Recto', 'Plexi', 'My RAT', 'My Muff']);
    params.model_dirs = JSON.stringify(['amps','amps','dist','fuzz']);
    const u = mkUi();
    u.init();

    reads.length = 0;
    for (let i = 0; i < LAP; i++) { advance(50); u.tick(); }
    ok(reads.includes('list_gen'),
       'list_gen: the rotation asks for it, once a lap');
    ok(!reads.includes('model_list'),
       'list_gen: unchanged -> the lists are NOT re-read');

    /* A capture arrives sorting FIRST, which is what makes this more than
     * cosmetic: every index behind it shifts, which is why the plugin
     * rebinds its blocks by NAME rather than keeping the number. */
    params.model_list = JSON.stringify(['Aaa New', 'Recto', 'Plexi',
                                        'My RAT', 'My Muff']);
    params.model_dirs = JSON.stringify(['amps','amps','amps','dist','fuzz']);
    params.list_gen = '1';
    reads.length = 0;
    for (let i = 0; i < LAP; i++) { advance(50); u.tick(); }
    ok(reads.includes('model_list'),
       'list_gen: moved -> the lists ARE re-read');
    ok(reads.filter(k => k === 'model_list').length === 1,
       'list_gen: re-read ONCE, not once a tick for the rest of the lap');

    /* AN OLD PLUGIN DOES NOT SERVE IT. null is not news about the card -
     * see the tri-state rule - and must not read as "changed again". */
    delete params.list_gen;
    const u3 = mkUi();
    u3.init();
    reads.length = 0;
    for (let i = 0; i < LAP * 2; i++) { advance(50); u3.tick(); }
    ok(!reads.includes('model_list'),
       'list_gen: a host that does not serve it never re-reads');
    params.list_gen = '0';
}

/* --- two pages of eight, moved by the nav arrows ----------------------- */
/* The board is sixteen columns and the pads are eight, so the page is a
 * WINDOW. What has to hold is that nothing except the view knows it
 * exists: the pads address the window, the signal path does not. */
{
    const CC_L = 62, CC_R = 63;
    const CW = 14;
    const colX = (c) => 4 + c * (CW + 1);
    const tap = (cc) => { ui.onMidiMessageInternal([0xb0, cc, 127]);
                          ui.onMidiMessageInternal([0xb0, cc, 0]); };
    const repaint = () => { advance(50); ui.tick(); };

    /* Page 1 columns 1-3 full, page 2 columns 9-10 full. */
    board({ b1: 3, b2: 3, b3: 3, b9: 3, b10: 3 });
    ui.init();
    repaint();

    /* THE ARROWS ARE CLAIMED AT RUNTIME, EVERY TICK.
     *
     * capabilities.claims_ccs cannot reach a module that draws its own
     * screen: the host reads the module id from the HIERARCHY editor's
     * slot/component, which only resetHierarchyEditorFor() writes, and
     * that runs only for a component that HAS a ui_hierarchy - the exact
     * thing this module withholds so ui_chain.js loads at all. The id
     * comes back "", the claim resolves to "", and host_claim_ccs is
     * never called. Without the runtime claim the arrows reach MOVE and
     * the page never moves, which is what the device reported.
     *
     * Restated, never memoised, for pad_block's reason: the shim clears
     * claim_cc_bits itself when the display closes and tells JS nothing,
     * so a mirror latches and the arrows die after the first dismiss. */
    claimed.length = 0;
    repaint();
    ok(claimed.length >= 1 && claimed[0].includes(62) && claimed[0].includes(63),
       'page: the arrows are claimed at runtime (got ' +
       JSON.stringify(claimed[0]) + ')');
    const before = claimed.length;
    repaint(); repaint(); repaint();
    ok(claimed.length >= before + 3,
       'page: and RESTATED every tick, not edged (' + before + ' -> ' +
       claimed.length + ')');

    /* A PAD ADDRESSES THE WINDOW. On page 1 pad 68 is b1; after one right
     * arrow the same pad is b9. Addressing the block instead would make
     * the second page unreachable and the first page's pads wrong. */
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    ok(writes.some(([k]) => k === 'b1_on'),
       'page: on page 1 the first pad is b1');

    tap(CC_R);
    repaint();
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    ok(writes.some(([k]) => k === 'b9_on'),
       'page: after one right arrow the same pad is b9');
    ok(!writes.some(([k]) => k === 'b1_on'),
       'page: and no longer b1');

    /* ONE TAP, ONE PAGE. Acting on the release too would page twice, and
     * with two pages that reads as the arrow doing nothing. */
    tap(CC_R);
    repaint();
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    ok(writes.some(([k]) => k === 'b9_on'),
       'page: right at the last page is a no-op, not a wrap');

    tap(CC_L);
    repaint();
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    ok(writes.some(([k]) => k === 'b1_on'), 'page: left comes back');
    tap(CC_L);
    repaint();
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    ok(writes.some(([k]) => k === 'b1_on'),
       'page: left at page 1 is a no-op, not a wrap');

    /* THE TOP ROW PAGES WITH IT. One rule for both rows, or the parallel
     * lane is only editable on the first half of the board. */
    board({ b1: 3, b9: 3, t9: 3, t10: 3 });
    ui.init();
    tap(CC_R);
    repaint();
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 76, 127]);
    ui.onMidiMessageInternal([0x80, 76, 0]);
    ok(writes.some(([k]) => k === 't9_on'),
       'page: the top row pages too - pad 76 is t9 on page 2');

    /* THE LEDS ARE THE WINDOW. A block off the page has no pad, and
     * colOf() would address the row ABOVE: note 76+8 is 84, which is
     * Move's THIRD pad row and belongs to nobody here. */
    board({ b1: 3, b2: 3, b9: 3 });
    ui.init();
    repaint();
    ok(leds[68] !== 0 && leds[69] !== 0,
       'page: page 1 lights its own two blocks');
    const stray = Object.keys(leds).filter(n => Number(n) >= 84 &&
                                                Number(n) <= 99 &&
                                                leds[n] !== 0);
    ok(stray.length === 0,
       'page: nothing is written to a third pad row' +
       (stray.length ? ' -- ' + stray.join(',') : ''));
    tap(CC_R);
    repaint();
    ok(leds[68] !== 0, 'page: page 2 lights b9 on the first pad');
    ok(leds[69] === 0, 'page: and leaves the empty b10 dark');

    /* THE SELECTION COMES WITH THE WINDOW, or the knobs edit a block that
     * is not on screen and the blinking pad is on neither page. */
    board({ b1: 3, b9: 3 });
    ui.init();
    ui.onMidiMessageInternal([0x90, 68, 127]);
    advance(1000); ui.tick();
    ui.onMidiMessageInternal([0x80, 68, 0]);
    writes.length = 0;
    tap(CC_R);
    repaint();
    const selW = writes.filter(([k]) => k === 'sel_block').map(([, v]) => v);
    ok(selW.length > 0 && Number(selW[selW.length - 1]) >= 16 + 8,
       'page: the selection follows onto page 2 (got ' + selW.join(',') + ')');

    /* NOTHING IS DRAWN OFF-SCREEN ON THE SECOND PAGE. The boxes are placed
     * from the VIEW column, so a board whose blocks sit at columns 9-16
     * must land in the same eight slots as 1-8 - using the board column
     * would put them at x=139 and off the panel with no error. */
    board({ b9: 3, b10: 1, b11: 2, b12: 3, b16: 3, t10: 3, t11: 3 });
    ui.init();
    tap(CC_R);
    rects.length = 0;
    repaint();
    const offRight = rects.filter(r => r[1] + r[3] > 128);
    ok(offRight.length === 0,
       'page: page 2 draws inside the panel' +
       (offRight.length ? ' -- ' + JSON.stringify(offRight.slice(0, 3)) : ''));
    ok(rects.some(r => r[1] === colX(0)),
       'page: and b9 is drawn in the FIRST column, not the ninth');

    /* A FORK THAT CROSSES THE PAGE BOUNDARY, which is the branch the
     * window exists to get right and the one that cannot be reasoned
     * about from one page.
     *
     * Fork at column 3 (page 1), merge at 12 (page 2). Page 1 parts and
     * runs off the right edge still parted; page 2 arrives already parted
     * and joins in its fourth column. The edge where nothing happens must
     * carry NO VERTICAL - one drawn there claims a fork that is on the
     * other page. */
    board({ b1: 3, b2: 3, b3: 3, t3: 3, t4: 3, t11: 3, b12: 3, b13: 3 });
    params['merge'] = '12';
    ui.init();
    rects.length = 0;
    repaint();
    const vline = (x) => rects.some(
        r => r[0] === 'fill' && r[1] === x && r[3] === 1 && r[4] > 4);
    /* Page 1: a vertical AT the fork (column 3 -> view 2), and none at the
     * right edge, because the merge is not on this page. */
    ok(vline(colX(2) - 1), 'split x page: page 1 draws the fork vertical');
    ok(!vline(128 - 3), 'split x page: and none at its right edge');

    tap(CC_R);
    rects.length = 0;
    repaint();
    /* Page 2: a vertical at the merge (column 12 -> view 3), and none at
     * the LEFT edge, where the two rails simply continue in. */
    ok(vline(colX(3) + CW), 'split x page: page 2 draws the merge vertical');
    ok(!vline(4), 'split x page: and none at its left edge');
    /* Both lanes still reach across the page, or a block on the top row
     * of page 2 is sitting on nothing. */
    const laneRuns = rects.filter(
        r => r[0] === 'fill' && r[3] > 40 && r[4] === 1);
    ok(laneRuns.length >= 2,
       'split x page: both lanes run across page 2 (got ' +
       laneRuns.length + ')');
    params['merge'] = '0';
}

/* --- the tuner screen -------------------------------------------------- */
{
    const mkUi = () => new Function('globalThis', 'leds', 'buttonLeds',
        ...names, stubs + src + '\nreturn globalThis.chain_ui;')(
        {}, leds, buttonLeds, ...names.map(n => host[n]));
    const click = (u) => u.onMidiMessageInternal([0xb0, 3, 127]);
    const rp = (u) => { advance(50); u.tick(); };

    board({ b1: 3 });
    params.tuner = '{"hz":0,"lvl":0.0001,"conf":0}';
    const u = mkUi();
    u.init();

    /* It is the FIRST menu row, because reaching it with a guitar in your
     * hands is the one interaction here that has to be short. */
    drawn.length = 0;
    click(u); rp(u);
    ok(cursorRow(u, () => { drawnAt.length = 0; rects.length = 0; advance(50); u.tick(); })
       === 'Tuner', 'tuner: it is the first menu row');

    /* Open it. */
    click(u);
    drawn.length = 0; rp(u);
    ok(drawn.some(t => t === 'Tuner'), 'tuner: the screen opens');
    ok(drawn.some(t => t === 'play a string'),
       'tuner: silence says WHY there is no reading');
    ok(drawn.some(t => t === '--'), 'tuner: and shows no note');

    /* A LEVEL BUT NO PITCH is a different sentence from silence. Both are
     * hz 0, which is why the plugin reports the level beside it. */
    params.tuner = '{"hz":0,"lvl":0.02,"conf":0.1}';
    drawn.length = 0; rp(u); rp(u); rp(u);
    ok(drawn.some(t => t === 'too quiet'),
       'tuner: something inaudible is not the same as nothing');

    /* A note, well out of tune. */
    params.tuner = '{"hz":80.0,"note":"E","oct":2,"cents":-51.4,' +
                   '"conf":0.97,"lvl":0.2}';
    /* Three repaints to let the value land, then ONE more with the rect log
     * cleared: counting rects across repaints counts the same box three
     * times, which is how the clamp assertion below first read as three
     * needles. */
    rp(u); rp(u); rp(u);
    drawn.length = 0; rects.length = 0;
    advance(50); u.tick();
    ok(drawn.some(t => t === 'E2'), 'tuner: it names the note');
    ok(drawn.some(t => t === '-51c'), 'tuner: and how far out');
    ok(drawn.some(t => t === '80.00 Hz'), 'tuner: and the frequency');
    ok(drawn.some(t => t === '6 E'), 'tuner: and which string that is');
    /* THE NEEDLE IS CLAMPED, NOT WRAPPED. -51 cents is past the end of a
     * +-50 scale, and a needle that wrapped would read as nearly sharp. */
    const needleX = rects.filter(r => r[0] === 'fill' && r[3] === 3 &&
                                      r[4] === 13).map(r => r[1]);
    ok(needleX.length === 1 && needleX[0] <= 4,
       'tuner: the needle clamps at the flat end (x=' + needleX.join(',') + ')');

    /* IN TUNE IS A FILLED BOX, not a word - readable from the distance a
     * guitar puts between you and the screen. */
    params.tuner = '{"hz":82.41,"note":"E","oct":2,"cents":0.4,' +
                   '"conf":0.99,"lvl":0.2}';
    rp(u); rp(u); rp(u);
    rects.length = 0; drawn.length = 0;
    advance(50); u.tick();
    const box = rects.filter(r => r[0] === 'fill' && r[2] === 13 && r[5] === 1 &&
                                  r[4] === 11);
    ok(box.length === 1, 'tuner: in tune fills the note box');
    ok(drawn.some(t => t === 'E2'), 'tuner: and still names the note');

    /* Three cents out is NOT in tune, or the box means nothing. */
    params.tuner = '{"hz":82.0,"note":"E","oct":2,"cents":-8.6,' +
                   '"conf":0.99,"lvl":0.2}';
    rp(u); rp(u); rp(u);
    rects.length = 0;
    advance(50); u.tick();
    ok(rects.filter(r => r[0] === 'fill' && r[2] === 13 && r[4] === 11).length === 0,
       'tuner: 8 cents out does not fill it');

    /* THE BOARD IS NOT DRAWN, and its reads stop: the rotation keeps the
     * board's picture current and the board is not on screen, so those
     * ~2.8 ms reads would pay for a picture nobody is looking at. */
    reads.length = 0;
    rp(u); rp(u); rp(u); rp(u);
    ok(reads.every(k => k === 'tuner'),
       'tuner: nothing but the tuner is read (' +
       [...new Set(reads)].join(',') + ')');
    /* And not every tick, because the detector only publishes at 21 Hz. */
    ok(reads.length <= 2,
       'tuner: asked every other tick, not every one (' + reads.length + ')');

    /* NOTHING FALLS THROUGH TO THE BOARD. A pad press behind this screen
     * would stomp a block you cannot see. */
    writes.length = 0;
    u.onMidiMessageInternal([0x90, 68, 127]);
    u.onMidiMessageInternal([0x80, 68, 0]);
    u.onMidiMessageInternal([0xb0, 71, 1]);
    u.onMidiMessageInternal([0xb0, 62, 127]);
    rp(u);
    ok(writes.length === 0,
       'tuner: pads, knobs and arrows are swallowed (' +
       writes.map(w => w[0]).join(',') + ')');

    /* The jog click closes it - not Back, which the host takes before this
     * file is called at all and which leaves the module entirely. */
    click(u);
    drawn.length = 0; rp(u);
    ok(!drawn.some(t => t === 'jog: close'), 'tuner: the jog click closes it');
    ok(drawn.some(t => t === 'B1') || drawn.some(t => t === 'A2C') ||
       rects.length > 0, 'tuner: and the board is back');
    delete params.tuner;
}

console.log(fails ? 'FAILED' : 'PASS');
process.exit(fails ? 1 : 0);

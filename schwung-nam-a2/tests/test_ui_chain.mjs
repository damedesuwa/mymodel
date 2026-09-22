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
    sel_block: '0', cpu: '41', build: 'test',
    split: '0', pan_a: '-1.0', pan_b: '1.0',
    model_list: JSON.stringify(['OCD', 'Recto']),
    cab_list: JSON.stringify(['TF MESA']),
    /* THE PEDAL TABLE COMES FROM THE PLUGIN, not from a copy written out
     * here. tests/run_contract_test.sh compiles the real plugin and writes
     * what it serves into the fixture; this reads it. A hand-written table
     * would pass this test while disagreeing with the binary, which is the
     * exact failure the served spec exists to prevent. */
    fx_list: JSON.stringify(JSON.parse(specsRaw).map(p => p.n)),
    fx_specs: specsRaw,
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
const reads = [];       /* every key the module asks the plugin for */
const drawnAt = [];     /* the same text, with where it landed */
const rects = [];
const leds = {};
const buttonLeds = {};
let padBlock = -1;
const actionsRun = [];

const host = {
    host_module_get_param: (k) => { reads.push(k); return (k in params ? params[k] : null); },
    host_module_set_param: (k, v) => { params[k] = String(v); writes.push([k, String(v)]); },
    host_pad_block: (v) => { padBlock = v; },
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
     * are not detented and the shim coalesces, so a message carries
     * however many ticks arrived in that audio frame. */
    const STEP = 3;
    const turn = (cc, dir, steps = 1) => {
        for (let i = 0; i < steps * STEP; i++)
            ui.onMidiMessageInternal([0xb0, cc, dir > 0 ? 1 : 127]);
    };
    const BAR_Y = 52;
    const K1 = 71, K2 = 72, K3 = 73, K4 = 74, K5 = 75, K6 = 76, K7 = 77, K8 = 78;
    /* draw() is throttled to 33 ms, so a burst of ticks in one millisecond
     * paints ONCE and every capture after it comes back empty - which
     * reads as "the feature draws nothing". Move the clock instead. */
    let clockSkew = 0;
    const realNow2 = Date.now;
    Date.now = () => realNow2() + clockSkew;
    const repaint = (n = 1) => {
        for (let i = 0; i < n; i++) { clockSkew += 50; ui.tick(); }
    };
    /* Off NAM Cab OD Dist Fuzz Boost Dyn Filter Mod Time Pitch */
    const CAT = { OFF:0, NAM:1, CAB:2, OD:3, DIST:4, FUZZ:5, BOOST:6,
                  DYN:7, FILTER:8, MOD:9, TIME:10, PITCH:11 };
    const gotoCat = (c) => { turn(K1, -1, 12); turn(K1, +1, c); };

    /* The sensitivity itself, which is the thing that was wrong: two
     * detents must move NOTHING, and the third must move exactly one. */
    turn(K1, -1, 12);                                /* park on Off */
    writes.length = 0;
    ui.onMidiMessageInternal([0xb0, K1, 1]);
    ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(writes.length === 0, 'knob: two detents do not make a step');
    ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(params['b4_type'] === '1', 'knob: the third detent makes exactly one step (Off->NAM)');

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
    gotoCat(CAT.TIME);
    ok(params['b4_fx'] === '30', 'time: starts at the Digital Delay');
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
     * place you can be lost in. */
    ok(drawn.some(t => String(t) === 'Digital Dly  Time 1/8'),
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

    /* THE ROUTING ROW KEEPS THE BLOCK ROW'S RHYTHM: tap flips a side,
     * hold parts the board. It had three meanings on one tap decided by
     * where the pad sat, and the device's verdict was that there was
     * nothing to learn. */
    const R = (n) => 76 + n;
    const tapR = (n) => {
        ui.onMidiMessageInternal([0x90, R(n), 127]);
        ui.onMidiMessageInternal([0x80, R(n), 0]);
    };
    const holdR = (n) => {
        ui.onMidiMessageInternal([0x90, R(n), 127]);
        clockSkew += 1000; ui.tick();
        ui.onMidiMessageInternal([0x80, R(n), 0]);
    };

    writes.length = 0;
    tapR(5);
    ok(writes.some(([k]) => k === 'b6_lane'), 'route: a tap flips that block side');
    ok(!writes.some(([k]) => k === 'split'), 'route: a tap never moves the split');
    const firstSide = writes.find(([k]) => k === 'b6_lane')[1];
    writes.length = 0;
    tapR(5);
    ok(writes.some(([k, v]) => k === 'b6_lane' && v !== firstSide),
       'route: and tapping again puts it back');

    writes.length = 0;
    holdR(2);
    ok(writes.some(([k, v]) => k === 'split' && v === '3'),
       'route: a hold parts the board there');
    writes.length = 0;
    holdR(2);
    ok(writes.some(([k, v]) => k === 'split' && v === '0'),
       'route: holding the same pad rejoins it');
    holdR(2);

    /* THE BLOCK ROW IS UNTOUCHED - tap stomps, hold edits. */
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 68, 127]);
    ui.onMidiMessageInternal([0x80, 68, 0]);
    ok(writes.some(([k]) => k === 'b1_on'), 'blocks: a tap still stomps');
    writes.length = 0;
    ui.onMidiMessageInternal([0x90, 74, 127]);
    clockSkew += 1000; ui.tick();
    ui.onMidiMessageInternal([0x80, 74, 0]);
    ok(writes.some(([k, v]) => k === 'sel_block' && v === '6'),
       'blocks: a hold still selects it for editing');

    /* EVERY ROUTING PAD IS LIT, because the colour is the side it gives
     * you. Brightness is the other fact: dim before the split, bright in
     * circuit - and the boundary between them IS the split point. */
    for (let b = 1; b <= 8; b++) { params[`b${b}_type`] = '3'; params[`b${b}_lane`] = '0'; }
    params['b4_lane'] = '1';
    ui.init();
    repaint();
    for (let b = 0; b < 8; b++)
        ok(leds[R(b)] !== 0, `route: pad ${b + 1} is lit`);
    ok(leds[R(0)] !== leds[R(2)],
       'route: before the split is dim, at it is bright');
    ok(leds[R(2)] !== leds[R(3)],
       'route: and the two sides are different colours');

    /* AND THE SCREEN SAYS WHAT JUST HAPPENED. The drawing is a picture of
     * the state; it does not answer "did that tap do what I meant", which
     * is the question being asked while you change it. */
    tapR(6);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => /^B7 -> [LR]$/.test(d[2])),
       'route: a flip says itself in the footer');
    holdR(4);
    drawnAt.length = 0; repaint();
    ok(drawnAt.some(d => d[2] === 'Split at 5'), 'route: so does a split');
    /* And it gets out of the way. */
    clockSkew += 2000;
    drawnAt.length = 0; repaint();
    ok(!drawnAt.some(d => /^Split at/.test(d[2])),
       'route: the message is brief, not a mode');
    holdR(2);

    /* THE PICTURE ON SCREEN. A rail above the row for the left lane,
     * below for the right, and a drop where they part company - which is
     * where Move draws it too. */
    params['b3_lane'] = '0'; params['b4_lane'] = '1';
    ui.init();
    rects.length = 0; drawnAt.length = 0; repaint();
    const BOXY = 12, BOXH = 18;
    ok(rects.some(r => r[0] === 'fill' && r[2] === BOXY - 2 && r[4] === 1 && r[1] === 2 * 16),
       'split: the left lane gets a rail above its boxes');
    ok(rects.some(r => r[0] === 'fill' && r[2] === BOXY + BOXH && r[4] === 1 && r[1] === 3 * 16),
       'split: the right lane gets one below');
    ok(rects.some(r => r[0] === 'fill' && r[1] === 2 * 16 && r[2] === BOXY - 2 &&
                       r[3] === 1 && r[4] === BOXH + 3),
       'split: and they part company with a vertical drop');
    ok(drawnAt.some(d => d[2] === 'L') && drawnAt.some(d => d[2] === 'R'),
       'split: each box says which side it is on');
    /* Width matters in this one: the header's own rule is a 128 px fill on
     * exactly this row, so a test that only checked x and y would report
     * the rule as a rail and pass for block 1 forever. */
    ok(!rects.some(r => r[0] === 'fill' && r[2] === BOXY - 2 && r[4] === 1 &&
                        r[1] === 0 && r[3] === 16),
       'split: nothing before it wears a rail');

    /* PAN, in its own units - a position between two names. */
    ui.onMidiMessageInternal([0xb0, 3, 127]);        /* open */
    ok(menuGoTo(ui, 'Pan L', repaint), 'split: Pan L is a row');
    writes.length = 0;
    for (let i = 0; i < 5; i++) ui.onMidiMessageInternal([0xb0, K1, 1]);
    ok(writes.some(([k, v]) => k === 'pan_a' && Number(v) > -1 && Number(v) <= 1),
       'pan: the knob moves it off hard left');
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
    h3.print = (x, y, t) => seen.push(String(t));
    h3.clear_screen = () => {};
    const n3 = Object.keys(h3);
    const ui3 = new Function('globalThis', 'leds', 'buttonLeds', ...n3,
        stubs + src + '\nreturn globalThis.chain_ui;')({}, {}, {}, ...n3.map(n => h3[n]));
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
    ok(bad.length === 0, 'nothing is drawn outside 128x64' +
        (bad.length ? ' -- ' + bad.slice(0, 4).join('; ') : ''));
}

console.log(fails ? 'FAILED' : 'PASS');
process.exit(fails ? 1 : 0);

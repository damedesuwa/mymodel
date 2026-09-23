/*
 * Nam A2c - the pedalboard face.
 *
 * Eight blocks on the bottom pad row, a row of eight boxes on the screen,
 * and the encoders on whichever block is selected.
 *
 *   TAP a pad    stomp it - toggles that block in and out of circuit
 *   HOLD a pad   select it for editing (the encoders follow)
 *
 * Tap is the stomp because that is the gesture the pedal metaphor already
 * owns; hold is the one you do rarely, which is the right way round for a
 * thing you step on.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: re-implement the knob grid. The host's
 * grid is the better editor and it is still there - leaving this screen
 * returns to it. The encoders here drive a short, fixed list per block type,
 * which is small precisely because the DSP is ours: a NAM block has two
 * editable things, a cab has one, a drive has four.
 */

import { setLED, setButtonLED, decodeDelta, invalidateLedCache } from '/data/UserData/schwung/shared/input_filter.mjs';

/*
 * TWO ROWS OF EIGHT, AND BOTH ARE LIVE.
 *
 * The parallel lane used to be carved out of the one row of eight: split
 * at column three and the six blocks after it were SHARED between the two
 * sides. Taking a board stereo therefore shortened both halves of it, and
 * the report was exactly that - "블럭 갯수가 줄잖아". It is a fair
 * complaint about a feature that is supposed to add something.
 *
 * Move has two pad rows and the reference picture (Quad Cortex's grid) is
 * two rows of eight, so that is what this is:
 *
 *   BOTTOM pads 68..75   the MAIN chain   - drawn on the lower rail
 *   TOP    pads 76..83   the PARALLEL row - drawn on the upper rail
 *
 * Both rows take TAP to stomp and HOLD to edit, identically. There is no
 * routing row and no routing gesture left to learn.
 *
 * WHERE THEY PART IS NOT A SETTING. The fork is the first column that
 * carries a TOP-row block, so putting a pedal up there IS the split and
 * taking the last one off rejoins the board. Two things that used to have
 * to agree - a stored split and the blocks either side of it - are now
 * one thing, and the picture cannot disagree with the audio.
 *
 * A block in the main row BEFORE the fork feeds both lanes, which is the
 * amp-into-two-cabs shape without paying for a second amp.
 */
const NUM_COLS = 8;
const NUM_ROWS = 2;
const NUM_BLOCKS = NUM_COLS * NUM_ROWS;      /* slots, not columns */
const ROW_BR = 0;               /* top pad row - the parallel branch */
const ROW_MAIN = 1;             /* bottom pad row - the main chain   */
const PAD_BASE = 68;            /* bottom row, notes 68..75 */
const ROUTE_BASE = 76;          /* top row,    notes 76..83 */

function slotOf(row, col) { return row * NUM_COLS + col; }
function rowOf(b) { return (b / NUM_COLS) | 0; }
function colOf(b) { return b % NUM_COLS; }
/* THE ROW IS A LETTER IN THE KEY, so the column stays one digit: `b1..b8`
 * is the main row and `t1..t8` the branch. A flat b1..b16 would have made
 * every parse two-digit, and the parse that gets it wrong reads b16 as
 * b1 - silently, on the block furthest from where you are looking. */
function keyOf(b, sub) {
    return (rowOf(b) === ROW_BR ? 't' : 'b') + (colOf(b) + 1) + '_' + sub;
}
function blockName(b) {
    return (rowOf(b) === ROW_BR ? 'T' : 'B') + (colOf(b) + 1);
}
/* The column the lanes part at, or -1 for a board that stays mono.
 * DERIVED, never stored - see the note above. */
function forkCol() {
    for (let c = 0; c < NUM_COLS; c++)
        if (st.type[slotOf(ROW_BR, c)] !== TYPE_OFF) return c;
    return -1;
}
/* The LAST column the two lanes are still apart on.
 *
 * DERIVED FROM THE BLOCKS FOR ONE RELEASE, AND IT MOVED UNDER PEOPLE.
 * "Right after the last top-row block" is symmetric with the fork and
 * reads well written down, but it meant adding a pedal to the branch
 * silently relocated the merge - the topology changed as a side effect of
 * editing a block. Reported as exactly that: the split is easy, the way
 * it comes back together is not.
 *
 * It is the `Merge` row on the menu now, and its default is the OUTPUT -
 * a board that forks and never rejoins, which is what this did before the
 * join existed. Nothing moves unless you move it.
 *
 * The fork still wins, so the parallel section is never narrower than one
 * column, and a branch block PAST the merge is off the path - no box, pad
 * dark - exactly as one before the fork is. The setting cannot quietly
 * delete audio. */
function joinCol() {
    const f = forkCol();
    if (f < 0) return -1;
    const m = num(st.val['merge'], 0);
    if (m < 1) return NUM_COLS - 1;                 /* 0 = at the output */
    return Math.max(f, Math.min(NUM_COLS - 1, m - 1));
}
/* Is this slot in the signal path at all? The branch row outside
 * [fork, join] is not - by construction it is empty there. */
function inPath(b) {
    if (rowOf(b) === ROW_MAIN) return true;
    const f = forkCol();
    return f >= 0 && colOf(b) >= f && colOf(b) <= joinCol();
}
const HOLD_MS = 350;

const CC_KNOB_BASE = 71;        /* knobs 1..8 are CC 71..78 */
const CC_JOG_TURN = 14;
const CC_JOG_CLICK = 3;
const CC_SHIFT = 49;

const TYPE_OFF = 0, TYPE_NAM = 1, TYPE_CAB = 2, TYPE_FX = 3;

/*
 * ONE KNOB DECIDES WHAT THE BLOCK IS, and it used to take two.
 *
 * Knob 1 was Type (Off / NAM / Cab / FX) and knob 2 was the FX family, so
 * a block cost three encoders to address - and once the pedals became
 * real ones, several of them needed FIVE of their own. Three for
 * navigation plus five for the pedal plus two for the board's levels is
 * ten knobs on a device with eight.
 *
 * Type and family are the same question asked twice, so they are one knob
 * now: Off, NAM, Cab, and then the six pedal families. That is strictly
 * fewer concepts AND it frees the encoder the 1176 needed - the two
 * reasons agree, which is usually the sign that a merge is right rather
 * than convenient.
 */
/*
 * EVERY CATEGORY HAS ITS OWN COLOUR, and the pair is a hue's bright and
 * dim variants rather than two constants that happen to look related.
 *
 * The palette header in shared/constants.mjs lists a `dim` and a `dark`
 * for every hue, and taking the pair from one hue is what keeps "switched
 * on" and "bypassed" reading as the SAME pedal at two brightnesses. The
 * host's own knob_leds.mjs records what happens otherwise: constants
 * picked by name produced a ramp that went dim, bright, dark, bright,
 * because Mustard and Ochre are different hues, not two levels of one.
 *
 * Hues are spread far enough apart to survive a glance across eight pads
 * at arm's length - the gain family runs warm (yellow, orange, magenta),
 * everything downstream runs cool (teal, green, blue, purple, pink), and
 * the two that are not pedals at all sit outside both: the amp is red and
 * the cab is brown.
 */
const CATS = [
    { name: 'Off',    type: TYPE_OFF, led: [0, 0] },
    { name: 'NAM',    type: TYPE_NAM, led: [1, 65] },     /* red     */
    { name: 'Cab',    type: TYPE_CAB, led: [5, 73] },     /* brown   */
    { name: 'OD',     type: TYPE_FX,  led: [8, 79],       /* yellow  */
      items: [0, 1, 2, 3, 4, 40, 41] },
    { name: 'Dist',   type: TYPE_FX,  led: [3, 69],       /* orange  */
      items: [5, 6, 7, 42, 8, 9] },
    { name: 'Fuzz',   type: TYPE_FX,  led: [26, 115],     /* magenta */
      items: [10, 11, 43] },
    { name: 'Boost',  type: TYPE_FX,  led: [122, 118],    /* white   */
      items: [12, 44] },
    { name: 'Dyn',    type: TYPE_FX,  led: [15, 93],      /* teal    */
      items: [13, 14, 15, 16, 17, 18] },
    { name: 'Filter', type: TYPE_FX,  led: [11, 85],      /* green   */
      items: [19, 47, 20, 21, 22] },
    { name: 'Mod',    type: TYPE_FX,  led: [16, 95],      /* azure   */
      items: [23, 46, 24, 25, 26, 45, 27, 28, 29] },
    { name: 'Time',   type: TYPE_FX,  led: [22, 107],     /* purple  */
      items: [30, 31, 32, 33, 49, 34, 48, 35] },
    { name: 'Pitch',  type: TYPE_FX,  led: [23, 109],     /* pink    */
      items: [36, 37, 38, 39] },
];

function catOfBlock(b) {
    const t = st.type[b];
    if (t !== TYPE_FX) return (t === TYPE_NAM) ? 1 : (t === TYPE_CAB) ? 2 : 0;
    const id = st.fx[b] | 0;
    for (let c = 3; c < CATS.length; c++)
        if (CATS[c].items.indexOf(id) >= 0) return c;
    return 3;
}

const White = 120;

/*
 * KNOBS 7 AND 8 ARE THE BOARD'S - except that a five-knob pedal needs one
 * of them, so only ONE of them is.
 *
 * Out is the level you ride, so it keeps a fixed encoder. In is a
 * set-once control and lives on the menu, where it costs a jog click and
 * cannot be nudged by accident mid-take.
 */
const OUT_KNOB = { key: 'out_level', label: 'Out', kind: 'float', global: true };
const GAP_KNOB = { key: '', label: '', kind: 'gap' };
const NUM_KNOBS = 8;

function fullKey(spec) { return spec.global ? spec.key : selKey(spec.key); }

/* ------------------------------------------------- the served knob table */

/*
 * WHAT A KNOB MEANS COMES FROM THE PLUGIN, not from a copy here.
 *
 * `fx_specs` is FX_PEDALS in a2_fx.h, serialised - every knob's name,
 * unit, ends and curve, read once at load exactly as the model and cab
 * lists are. Formatting happens here; the MAPPING does not. That matters
 * because the screen now prints real quantities - "480ms", "-6.0dB" - and
 * a second copy of those ranges in JavaScript would be a number that
 * looks authoritative and is not the one the delay line is using. This
 * module has already paid for one fact living in two places.
 */
function specKnob(spec, i) {
    if (!spec || !spec.k) return null;
    const kn = spec.k[i];
    if (!kn) return null;
    /* A switch can change what a neighbouring knob asks: the delay's Time
     * is milliseconds until Mode says Note. The alternate travels in the
     * same table, so the label and the number cannot disagree. */
    if (kn.a && spec.aw >= 0) {
        const sw = spec.k[spec.aw];
        if (sw && specValue(sw, st.val[selKey('p' + (spec.aw + 1))]) >= 1) return kn.a;
    }
    return kn;
}

function specValue(kn, p) {
    if (p === undefined || !Number.isFinite(p)) p = 0;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    if (kn.c === 2) {
        const n = Math.max(1, kn.hi | 0);
        return Math.round(p * (n - 1));
    }
    if (kn.c === 1) return kn.lo * Math.pow(kn.hi / kn.lo, p);
    return kn.lo + (kn.hi - kn.lo) * p;
}

/* Six characters is what a 31 px cell holds, so the unit rides with the
 * number and neither gets a space. */
function specText(kn, p) {
    if (kn.c === 2) {
        const opts = String(kn.o || '').split('|');
        const i = specValue(kn, p);
        return opts[i] || String(i);
    }
    const v = specValue(kn, p);
    const u = kn.u || '';
    if (u === 'dB')   return (v >= 0 ? '+' : '') + v.toFixed(1);
    if (u === '%')    return Math.round(v) + '%';
    if (u === 'cent') return Math.round(v) + 'c';
    if (u === 'Hz')   return (v >= 1000) ? (v / 1000).toFixed(1) + 'k' : Math.round(v) + 'Hz';
    if (u === 'ms') {
        /* THE UNIT RIDES ON THE VALUE, NOT THE LABEL. It was on the label
         * - "Rate ms" - which is seven characters against a cell that
         * holds about six, so it clipped to "Rate " and the unit was the
         * part that got cut. The value is shorter and the unit is the half
         * that carries the meaning: "480ms" is a rate you can set against
         * a tempo and "480" is not. Seconds past a second, for the same
         * reason: "1.20s" fits where "1200ms" does not. */
        if (v < 1)    return v.toFixed(2) + 'm';
        if (v < 10)   return v.toFixed(1) + 'm';
        if (v < 1000) return Math.round(v) + 'ms';
        return (v / 1000).toFixed(2) + 's';
    }
    return String(Math.round(v));
}

function specLabel(kn) { return kn.n; }

/* Built per block rather than declared: past the first two encoders the
 * list is a property of the PEDAL and there are forty of them. */
function knobsFor(type, fxId) {
    const CAT_KNOB = { key: 'cat', label: 'Block', kind: 'cat' };
    if (type === TYPE_NAM)
        return [CAT_KNOB,
                { key: 'model', label: 'Model', kind: 'list' },
                { key: 'quality', label: 'Qual', kind: 'enum', n: 3,
                  names: ['Full', 'Slim', 'Lite'] }];
    if (type === TYPE_CAB)
        return [CAT_KNOB, { key: 'cab', label: 'Cab', kind: 'list' }];
    if (type === TYPE_FX) {
        const out = [CAT_KNOB, { key: 'fx', label: 'Pedal', kind: 'fxid' }];
        const spec = st.specs[fxId];
        for (let i = 0; i < 5; i++) {
            const kn = spec ? specKnob(spec, i) : null;
            if (!kn) continue;
            out.push({ key: 'p' + (i + 1), label: specLabel(kn),
                       kind: 'spec', kn: kn });
        }
        return out;
    }
    return [CAT_KNOB];
}

/* The block's knobs, then blanks, then Out on encoder 8 - so the level is
 * in the same place whatever is loaded. A blank claims its encoder and
 * does nothing with it, which is the point: an unused knob between the
 * pedal and the level must not shift the level onto it. */
function padToBoard(list) {
    const out = list.slice(0, NUM_KNOBS - 1);
    while (out.length < NUM_KNOBS - 1) out.push(GAP_KNOB);
    out.push(OUT_KNOB);
    return out;
}


let sel = NUM_COLS;              /* main row, column 1 - see init */
/* THE HOST'S OWN MENU, borrowed.
 *
 * "My Presets" and "Module" are pages the PLANNER appends to every
 * component's knob grid - and this module has no grid, so dropping the
 * hierarchy for the pads took Swap Module, Remove Module and Module Help
 * with it. There was then no way to get the module out of the slot from
 * its own screen, which is worse than having no pads.
 *
 * They cannot be reimplemented here: the actions reach the user preset
 * store, the preset browser, the component picker and the help viewer,
 * none of which a module can address. The host binds them for exactly
 * this case (shadow_ui.js:6763), with the slot and component already
 * applied. */
let menuOpen = false;
let menuRows = [];
let menuCursor = 0;
let menuTop = 0;
const MENU_VISIBLE = 5;

let shiftHeld = false;
let padDownAt = [];             /* when each pad went down, or 0 */
let padHandled = [];            /* hold already fired, so the release is not a tap */
let routeDownAt = [];           /* the same two, for the routing row */
let routeHandled = [];
let lastPaint = 0;
let lastKnob = 0;   /* which cell the row is following */
/*
 * WHAT JUST HAPPENED, IN WORDS, FOR A MOMENT.
 *
 * The routing is drawn - rails above and below the row, L or R in each box
 * - and that is a picture of the STATE. It is not an answer to "did that
 * tap do what I meant", which is the question you are asking while you are
 * changing it, and which a picture you have to find in a 15 px box does
 * not answer quickly. So a routing change also says itself, in the footer,
 * for long enough to read and not long enough to be in the way.
 */
let flashText = '';
let flashUntil = 0;
const FLASH_MS = 1100;

function flash(t) {
    flashText = t;
    flashUntil = Date.now() + FLASH_MS;
    lastPaint = 0;
}

/*
 * THE ENCODERS ARE NOT DETENTED AND THE SHIM COALESCES, so one CC can carry
 * ten ticks.
 *
 * decodeDelta returns the MAGNITUDE (1..63), and this screen was stepping
 * every discrete control by exactly one per MESSAGE - so a list of models
 * moved one entry for a flick of the wrist and six for a turn, with no
 * relationship between how far the knob went and how far the list did.
 * Reported from the device as the knobs being too sensitive to set
 * anything. The float case had the opposite half of the same bug: it
 * multiplied by the magnitude, so a fast turn jumped 20 units at once.
 *
 * Both are fixed by ACCUMULATING the ticks and spending them:
 *
 *   - a discrete control costs DETENTS_PER_STEP ticks per step, with the
 *     remainder kept, so a slow turn advances one at a time and a fast one
 *     still covers ground in proportion to the turn;
 *   - a float moves FLOAT_STEP per tick, which is one unit of the 0-100 the
 *     cell shows. What you see move is what you turned.
 *
 * The remainder is per KNOB and is dropped whenever the knob list changes
 * underneath, or a half-turn saved up for Pedal lands on Drive.
 */
/* AND ONE MESSAGE IS STILL NOT ONE TICK.
 *
 * The accumulator fixed the relationship between the turn and the move;
 * it did not fix the SCALE. At ten ticks a message and three ticks a
 * step, a discrete control advanced three items per CC - and the shim
 * emits one CC per audio frame, so a real turn crossed a fifty-entry list
 * faster than it could be read. Reported again, the same words: the knobs
 * are too sensitive to find anything with.
 *
 * Three numbers, and they are independent:
 *
 *   DETENT_CAP       the most ticks ONE message may spend. A burst past
 *                    this is a turn faster than anyone means, and the
 *                    remainder is dropped rather than banked - banking it
 *                    would just move the overshoot one message later.
 *   DETENTS_PER_STEP ticks per item on a discrete control.
 *   FLOAT_STEP       how far one tick moves a continuous one, as a
 *                    fraction of its range.
 */
const DETENT_CAP = 4;
const DETENTS_PER_STEP = 5;
const FLOAT_STEP = 0.005;
const knobAcc = new Array(NUM_KNOBS).fill(0);

function resetKnobAcc() { for (let i = 0; i < NUM_KNOBS; i++) knobAcc[i] = 0; }

/* decodeDelta, with the burst clipped. Every read of an encoder on this
 * screen goes through here so the cap cannot apply to some knobs and not
 * others - which is how "the pedal list is too fast but the drive knob is
 * fine" would have been true. */
function detents(ccValue) {
    const d = decodeDelta(ccValue);
    if (!d) return 0;
    return (d > DETENT_CAP) ? DETENT_CAP : (d < -DETENT_CAP) ? -DETENT_CAP : d;
}

function knobSteps(idx, d) {
    knobAcc[idx] += d;
    const steps = (knobAcc[idx] / DETENTS_PER_STEP) | 0;   /* toward zero */
    knobAcc[idx] -= steps * DETENTS_PER_STEP;
    return steps;
}

/* A cache, because an IPC read is ~2.8 ms and a whole page render is 1.68 -
 * so a read costs more than redrawing the screen. Nothing is read on the
 * draw path: types and states are refreshed on a slow rotation and written
 * through immediately when WE change them, which is the only way they can
 * change while this screen is up. */
const st = {
    type: new Array(NUM_BLOCKS).fill(0),
    on: new Array(NUM_BLOCKS).fill(1),
    fx: new Array(NUM_BLOCKS).fill(0),
    cpu: 0,
    peak: 0,                    /* out level, %% of full scale - see drawMeter */
    blockCpu: new Array(NUM_BLOCKS).fill(0),
    val: {},                    /* "b3_drive" -> number */
    names: { model: [], cab: [], fx: [] },
    specs: [],                  /* fx_specs, read once - see specKnob */
    build: '?',
    rot: 0,
};

/* The lists are read ONCE. They change only when a file lands on the card,
 * which cannot happen while this screen is up. */
function loadLists() {
    for (const k of ['model', 'cab', 'fx']) {
        try {
            const j = getp(k + '_list');
            if (j) st.names[k] = JSON.parse(j);
        } catch (e) { st.names[k] = []; }
    }
    /* The knob table. ONE read, like the lists, and for the same reason:
     * it cannot change while this screen is up, and an IPC read is ~2.8 ms
     * against a 1.68 ms whole-page render. A pedal whose spec is missing
     * falls back to no knobs rather than to invented ones - an invented
     * range is a number on screen that is not the number in the DSP. */
    try {
        const j = getp('fx_specs');
        if (j) st.specs = JSON.parse(j);
    } catch (e) { st.specs = []; }
}

function listNameFor(key, idx) {
    const arr = (key === 'model') ? st.names.model : st.names.cab;
    if (!arr || !arr.length) return String(idx + 1);
    const n = arr[idx];
    return (typeof n === 'string' && n.length) ? n : String(idx + 1);
}

function getp(key) {
    try { return host_module_get_param(key); } catch (e) { return null; }
}
function setp(key, val) {
    try { host_module_set_param(key, String(val)); } catch (e) { }
}
function num(v, dflt) {
    if (v === null || v === undefined || v === '') return dflt;
    const n = Number(v);
    return Number.isFinite(n) ? n : dflt;
}

/* One read per tick, cycling. Eight blocks x four facts is 32 reads; doing
 * them every frame would be 90 ms of IPC per second and the screen would
 * crawl. A stop lands on each fact about twice a second, which is faster
 * than anything here changes by itself. */
function rotateRead() {
    /* THE METER IS NOT ON THE ROTATION, and it cannot be.
     *
     * A stop on the rotation comes round about twice a second, which is
     * fine for "which pedal is in block 6" and useless for a clip light.
     * The plugin peak-HOLDS between reads and reading consumes the hold,
     * so nothing is missed however slowly this is asked - but it still
     * has to be asked often enough to look live. Every fourth tick is
     * ~8 Hz for one 2.8 ms read, against a 33 ms frame. */
    if ((st.rot & 3) === 0) st.peak = num(getp('peak'), st.peak);

    const steps = NUM_BLOCKS * 2 + 1;
    const i = st.rot % steps;
    st.rot++;
    if (i === steps - 1) {
        st.cpu = num(getp('cpu'), st.cpu);
        return;
    }
    const b = i >> 1;
    if ((i & 1) === 0) {
        st.type[b] = num(getp(keyOf(b, 'type')), st.type[b]);
        st.on[b] = num(getp(keyOf(b, 'on')), st.on[b] ? 0 : 1) === 0 ? 1 : 0;
        st.fx[b] = num(getp(keyOf(b, 'fx')), st.fx[b]);
    } else {
        st.blockCpu[b] = num(getp(keyOf(b, 'cpu')), st.blockCpu[b]);
    }
}

/* WHAT TO OFFER, GIVEN WHAT THE HOST ACTUALLY HAS.
 *
 * `shadow_component_trailing_menus` and `shadow_component_run_action` are
 * RECENT bindings, and the device reported `no binding` for the first -
 * this host predates them, so the second is missing too and every row that
 * went through it did nothing. A menu whose rows are inert is worse than no
 * menu: it looks like the feature works.
 *
 * `host_swap_module` is the OLD one (the host's own comment calls the new
 * pair "exactly as host_swap_module above is"), and it opens the component
 * picker - whose first row is None. So on an old host one row does both
 * jobs, and it is named for both rather than promising a Remove that is
 * really a pick.
 */
const FALLBACK_ROWS = [
    ['Preset...',     'up_load'],
    ['Save As',       'up_save_as'],
    ['Add to List',   'module_lists'],
    ['Module Help',   'module_help'],
];

const SWAP_ROW = '\u0000swap';   /* handled here, not by the host */

function openMenu() {
    menuRows = [];
    const haveMenus = (typeof shadow_component_trailing_menus === 'function');
    const haveRun   = (typeof shadow_component_run_action === 'function');
    const haveSwap  = (typeof host_swap_module === 'function');
    let why = (haveMenus ? 'menus ' : '') + (haveRun ? 'run ' : '') +
              (haveSwap ? 'swap' : '');

    if (haveMenus && haveRun) {
        try {
            for (const sec of (shadow_component_trailing_menus() || [])) {
                for (const e of (sec.entries || [])) {
                    if (!e || !e.label) continue;
                    menuRows.push({
                        label: e.value ? (e.label + ': ' + e.value) : e.label,
                        action: e.action || null,
                    });
                }
            }
        } catch (err) {
            why += ' threw:' + ((err && err.message) ? err.message : String(err));
            menuRows = [];
        }
    }
    /* Only offer rows that CAN run. A key with no binding behind it is a
     * row that does nothing, which is the bug this is fixing. */
    if (menuRows.length === 0 && haveRun) {
        for (const r of FALLBACK_ROWS) menuRows.push({ label: r[0], action: r[1] });
        why += ' -> fallback';
    }
    /* Last, and always, because on an old host it is the only thing here
     * that works - and it is the one people need. */
    if (haveSwap) menuRows.push({ label: 'Swap / Remove...', action: SWAP_ROW });

    /* THE BOARD'S INPUT LEVEL LIVES HERE, and that is a decision rather
     * than an overflow. Out keeps encoder 8 because it is the one you ride
     * mid-take; In is set once against the guitar and then wants to be
     * somewhere it cannot be nudged. The jog edits it in place - no click,
     * no sub-screen - so it is still two gestures away. */
    menuRows.push({ label: 'Input', action: null, edit: 'in_level' });
    menuRows.push({ label: 'Output', action: null, edit: 'out_level' });
    /* NO SPLIT ROW. The fork is wherever the top row starts, so setting
     * it here would be a second way to say a thing the board already
     * says - and the two could disagree.
     *
     * The pans are named for the RAIL they move, not for a side, because
     * the side is what they SET. "Pan L" on a row you can drag to the
     * right was the question the device kept asking back. */
    /* WHERE THE BRANCH ENDS. The split is a placement and this is a
     * setting, and they are different kinds of thing on purpose: you
     * choose where to fork by putting a pedal there, and the merge then
     * STAYS where you put it however many pedals you add afterwards. */
    /* Still here as a READOUT and a fallback - the gesture is Shift + a
     * pad, and a row that can only be reached by scrolling is not where
     * anyone will look for it. */
    menuRows.push({ label: 'Merge', action: null, edit: 'merge', kind: 'merge' });
    menuRows.push({ label: 'Pan Top', action: null, edit: 'pan_a', kind: 'pan' });
    menuRows.push({ label: 'Pan Btm', action: null, edit: 'pan_b', kind: 'pan' });

    try { console.log('A2c menu: ' + why + ', ' + menuRows.length + ' rows'); } catch (e) {}

    menuRows.push({ label: 'Close', action: null });
    menuCursor = 0;
    menuTop = 0;
    menuOpen = true;
    lastPaint = 0;      /* repaint NOW, not up to 33 ms from now */
}

/* Three shapes of value on these rows and none of them is a percentage of
 * the same thing: a level is 0-100, a pan is a position between two names,
 * and the split is a block number or the word Off. */
function menuValueText(row, v) {
    if (v === undefined || !Number.isFinite(v)) return '-';
    /* "Out" is a place, not an off switch - the lanes do merge, at the
     * output, and calling it Off would say they never meet. */
    if (row.kind === 'merge')
        return (v < 1) ? 'Out' : ('after ' + Math.round(v));
    if (row.kind === 'pan') {
        const p = Math.round(v * 100);
        if (p === 0) return 'C';
        return (p < 0 ? 'L' : 'R') + Math.abs(p);
    }
    return Math.round(v * 100) + '%';
}

function runMenuRow() {
    const row = menuRows[menuCursor];
    menuOpen = false;
    lastPaint = 0;
    if (!row || !row.action) return false;
    if (row.action === SWAP_ROW) {
        /* Opens the component picker. Its first row is None, which is how
         * a module is removed on this host. */
        try { host_swap_module(); } catch (e) {}
        return true;
    }
    try {
        /* True means the action opened a screen, and this frame must not be
         * drawn over it. The host stops ticking us once the view moves, so
         * closing the menu and returning is the whole of it. */
        return !!(typeof shadow_component_run_action === 'function' &&
                  shadow_component_run_action(row.action));
    } catch (err) { return false; }
}

function drawMenu() {
    clear_screen();
    ptext(1, 1, 'Nam A2c', 1, 80);
    pright(126, 1, st.build, 1, 44);
    fill_rect(0, 10, 128, 1, 1);
    if (menuCursor < menuTop) menuTop = menuCursor;
    if (menuCursor >= menuTop + MENU_VISIBLE) menuTop = menuCursor - MENU_VISIBLE + 1;
    for (let i = 0; i < MENU_VISIBLE; i++) {
        const idx = menuTop + i;
        if (idx >= menuRows.length) break;
        const y = 14 + i * 10;
        if (idx === menuCursor) fill_rect(0, y - 1, 128, 10, 1);
        const row = menuRows[idx];
        const ink = idx === menuCursor ? 0 : 1;
        if (row.edit) {
            const v = st.val[row.edit];
            ptext(2, y, row.label, ink, 80);
            pright(126, y, menuValueText(row, v), ink, 44);
        } else {
            ptext(2, y, row.label, ink, 124);
        }
    }
    /* The picker's FIRST row is None, and picking it is how a module is
     * removed on this host. Saying "Swap / Remove" without saying where the
     * Remove is leaves you scrolling a list of thirty modules looking for
     * a row that is above all of them. */
    const cur = menuRows[menuCursor];
    if (cur && cur.action === SWAP_ROW)
        ptext(2, 55, 'pick None (top) to remove', 1, 124);
    else if (cur && cur.edit)
        ptext(2, 55, 'turn any knob to set', 1, 124);
}

function selKey(k) { return keyOf(sel, k); }

function knobList() { return padToBoard(knobsFor(st.type[sel], st.fx[sel])); }

function readSelected() {
    for (const s of knobList()) {
        /* A DERIVED CONTROL IS NOT A PARAMETER, and asking for one costs
         * a give-up every time.
         *
         * The Block knob reads `cat`, which the plugin does not serve and
         * never could - the category is computed from `type` and `fx`,
         * which it DOES serve. Asking for `b4_cat` got -1, which the wire
         * calls "the read did not complete", so the host retried it and
         * then logged `param_giveup ... last_key=fx1:b4_cat`, over and
         * over, on every block change. Not audible, not harmless: those
         * are real IPC round trips on a 2.8 ms channel, spent on a key
         * that cannot answer. */
        if (s.kind === 'gap' || s.kind === 'cat') continue;
        const k = fullKey(s);
        const v = getp(k);
        if (v !== null && v !== '') st.val[k] = Number(v);
    }
    /* The pedal changed under the encoders, so a half-turn that was being
     * accumulated for the OLD knob must not land on the new one. */
    resetKnobAcc();
}

/* ------------------------------------------------------------------ draw */

/*
 * THE SCREEN IS 128 x 64 AND EVERY STRING HERE IS VARIABLE.
 *
 * The first cut positioned text by arithmetic on assumed character widths
 * and let it run off the end: the footer hint was 30 characters against a
 * 128 px line, eight 14 px boxes with a 2 px gap came to 129 px, and a
 * three-character label centred in a 14 px box started at a negative
 * offset and spilled into its neighbour. Nothing clips itself.
 *
 * So there is no arithmetic on text anywhere below. Everything goes
 * through these three, which ask text_width() and SHORTEN until it fits.
 * A name that does not fit is cut; nothing is ever drawn over.
 */
function clipTo(t, w) {
    let str = String(t === undefined || t === null ? '' : t);
    while (str.length > 1 && text_width(str) > w) str = str.slice(0, -1);
    return str;
}
function ptext(x, y, t, ink, w) { print(x, y, clipTo(t, w), ink); }
function pcenter(x, w, y, t, ink) {
    const str = clipTo(t, w);
    print(x + Math.max(0, (w - text_width(str)) >> 1), y, str, ink);
}
function pright(xr, y, t, ink, w) {
    const str = clipTo(t, w);
    print(xr - text_width(str), y, str, ink);
}

/* The layout, in one place, so a change to one band cannot silently land on
 * top of another. */
/*
 * THE GRID IS A SIGNAL PATH, AND AN EMPTY SLOT IS A WIRE.
 *
 * Eight boxes were drawn edge to edge whether or not anything was in them,
 * each carrying a name AND a number AND, after the split, a side - so a
 * board with three pedals on it drew five empty boxes with a dash in each,
 * and the three that mattered had to be found among them. Reported, twice,
 * as messy and hard to read. It is.
 *
 * Axe-Edit's grid is the answer and it is not a styling choice: an unused
 * slot there is a SHUNT - a plain wire - so the picture only contains what
 * is in the chain, and the wire between the input and the output is
 * continuous whatever is on it. Everything else follows from that:
 *
 *   nothing there   a wire passes through, no box at all
 *   in circuit      a FILLED box with its name knocked out
 *   bypassed        an OUTLINED box, name in ink - the signal is visibly
 *                   not being taken by it
 *   selected        corner ticks outside the box, so the mark is separate
 *                   from what the box is saying about itself
 *
 * Four states, four different pictures, none of them a letter you have to
 * read in fifteen pixels.
 */
const GRID_X = 4;                  /* the input bar sits left of this  */
const COL_W = 14, COL_GAP = 1;     /* 8*14 + 7*1 = 119, 4..122         */
const GRID_Y = 12, GRID_H = 16;    /* 12..27 when there is no split    */
const LANE_H = 9, LANE_GAP = 1;    /* 12..20 and 22..30 when there is  */
const BAND_H = LANE_H * 2 + LANE_GAP;            /* 12..30, clear of 32 */
const IO_W = 2;

function colX(b) { return GRID_X + b * (COL_W + COL_GAP); }
const CELL_Y = 34, CELL_H = 18, CELL_W = 31;             /* 4 * 32 = 128    */
/* THE BAR SITS BELOW THE CELL'S HIGHLIGHT, NOT INSIDE IT. The selected
 * cell is filled white, and a bar drawn inside that would have to invert
 * with it - two rules for one picture. Outside, it is ink 1 on black
 * always, and the highlight ends one row above it.
 *
 * 34..41 label, 43..50 value, highlight 33..50, bar 52..54, footer 56..63. */
const BAR_Y = 52, BAR_H = 2, BAR_TRACK_Y = 54;
const FOOT_Y = 56;   /* 56 + 8 = 64 exactly; 57 runs a pixel off the bottom */

/*
 * A KNOB IS A POSITION, AND +0.0 IS NOT A POSITION.
 *
 * Reported from the device: the pedals whose controls read as signed
 * numbers are harder to read than the ones that read 0-100. They are -
 * "+0.0" tells you where a band is set only after you have thought about
 * it, and a Graphic EQ is five of them in a row, which nobody is going to
 * read as a shape.
 *
 * The number stays, because dB is the information and throwing it away to
 * make a 0-100 would be making the display worse to make it easier. What
 * changes is that every cell now also draws its position, and a BIPOLAR
 * control fills from the CENTRE rather than from the left - so a row of
 * five bands is a picture of the curve, at a glance, before any number is
 * read. It costs three rows of pixels that were empty.
 */
function drawBar(x, w, kind, kn, v) {
    if (kind === 'gap') return;
    /* The track, so an empty bar is still visibly a bar. */
    fill_rect(x, BAR_TRACK_Y, w, 1, 1);
    if (v === undefined || !Number.isFinite(v)) return;
    const p = Math.max(0, Math.min(1, v));
    const bipolar = !!(kn && kn.c !== 2 && kn.lo < 0 && kn.hi > 0);
    if (bipolar) {
        const mid = x + (w >> 1);
        /* The centre tick is taller than the fill, so "flat" reads as flat
         * rather than as an empty bar somebody forgot to draw. */
        fill_rect(mid, BAR_Y - 1, 1, 4, 1);
        const span = (w >> 1) - 1;
        const d = Math.round((p - 0.5) * 2 * span);
        if (d > 0) fill_rect(mid, BAR_Y, d, BAR_H, 1);
        else if (d < 0) fill_rect(mid + d, BAR_Y, -d, BAR_H, 1);
    } else {
        const fw = Math.round(w * p);
        if (fw > 0) fill_rect(x, BAR_Y, fw, BAR_H, 1);
    }
}

function blockLabel(b) {
    const t = st.type[b];
    if (t === TYPE_FX)  return st.names.fx[st.fx[b]] || 'FX';
    if (t === TYPE_NAM) return 'NAM';
    if (t === TYPE_CAB) return 'CAB';
    return '';
}


/*
 * THE OUTPUT METER, AND WHY IT IS ON SCREEN RATHER THAN IN THE LOG.
 *
 * The device log carried `out pk=2.02` for a whole session - the board was
 * six decibels into the quantiser's clamp, every peak was being squared
 * off, and nothing on the panel said so. There is no way to hear that as
 * clipping either: a NAM amp model is already distorting, so the thing
 * the clamp adds sounds like more of what the amp is doing.
 *
 * So: a meter, with the full-scale point marked a third in from the right
 * rather than at the end. That headroom is the whole design - a meter that
 * ends at 1.0 reads the same at 0.99 and at 2.0, which is exactly the
 * distinction worth drawing. Past the mark the meter INVERTS, so clipping
 * is a change of colour and not a change of length.
 */
const MET_X = 70, MET_W = 30, MET_Y = 2, MET_H = 6;
const MET_UNITY = 20;          /* 1.0 sits here; 10 px of "over" beyond it */

function drawMeter() {
    const pk = st.peak / 100;
    const over = st.peak >= 100;
    if (over) {
        /* Filled, with the bar knocked out of it - the same length it
         * would have been, unmistakably not the same thing. */
        fill_rect(MET_X, MET_Y, MET_W, MET_H, 1);
        const w = Math.min(MET_W, Math.round(pk * MET_UNITY));
        if (w < MET_W) fill_rect(MET_X + w, MET_Y + 1, MET_W - w, MET_H - 2, 0);
    } else {
        draw_rect(MET_X, MET_Y, MET_W, MET_H, 1);
        const w = Math.max(0, Math.min(MET_UNITY, Math.round(pk * MET_UNITY)));
        if (w > 0) fill_rect(MET_X, MET_Y + 1, w, MET_H - 2, 1);
        /* The full-scale mark, drawn last so the fill cannot hide it. */
        fill_rect(MET_X + MET_UNITY, MET_Y, 1, MET_H, 1);
    }
}

function drawHeader() {
    const cpu = Math.round(st.cpu) + '%';
    const cw = text_width(cpu) + 3;
    if (Math.round(st.cpu) >= 70) {
        fill_rect(128 - cw, 0, cw, 9, 1);
        pright(126, 1, cpu, 0, cw);
    } else {
        pright(126, 1, cpu, 1, cw);
    }
    drawMeter();
    /* WHAT IS SELECTED, WHAT IT IS, AND WHICH SIDE - the one line worth
     * reading first, and the side belongs in it. The row a box sits in
     * says which lane it is on; this says which lane YOU are on, which is
     * a different question and the one that was going unanswered. */
    const what = blockLabel(sel);
    /* The row letter says which rail, and while the board is forked the
     * pan word says where that rail is going. Both, because "T" is a
     * position on the screen and "L" is a position in the room. */
    const f = forkCol();
    const side = (f >= 0 && colOf(sel) >= f && colOf(sel) <= joinCol())
                 ? (rowOf(sel) === ROW_BR ? ' L' : ' R') : '';
    ptext(1, 1, blockName(sel) + side + (what ? ' ' + what : ' --'),
          1, MET_X - 3);
    fill_rect(0, 10, 128, 1, 1);
}

/* Where a block's box sits, and where its wire runs through the column. */
function blockGeom(b) {
    const split = forkCol(), join = joinCol();
    const col = colOf(b), row = rowOf(b);
    const forked = split >= 0 && col >= split && col <= join;
    /* A MAIN-ROW BLOCK OUTSIDE THE PARALLEL SECTION SPANS BOTH RAILS,
     * because that is what it does: one amp feeding two cabs is drawn as
     * one tall block meeting two short ones, and a reverb after them is
     * the same picture the other way round. The fork and the join then
     * need no arrows to explain them. */
    const y = !forked ? GRID_Y
            : (row === ROW_BR ? GRID_Y : GRID_Y + LANE_H + LANE_GAP);
    const h = forked ? LANE_H : (split >= 0 ? BAND_H : GRID_H);
    return { x: colX(col), y: y, h: h, wire: y + (h >> 1), forked: forked };
}

function drawBoxes() {
    const split = forkCol(), join = joinCol();
    const bandH = split >= 0 ? BAND_H : GRID_H;
    const midWire = GRID_Y + (bandH >> 1);
    const laneWire = [GRID_Y + (LANE_H >> 1),
                      GRID_Y + LANE_H + LANE_GAP + (LANE_H >> 1)];

    /* THE INPUT AND THE OUTPUT ARE PART OF THE PICTURE. Without them the
     * row is eight things in a line; with them it is a path, and which end
     * the signal comes in at stops being something you have to know. */
    fill_rect(0, GRID_Y, IO_W, bandH, 1);
    fill_rect(128 - IO_W, GRID_Y, IO_W, bandH, 1);

    /* THE WIRE IS DRAWN WHOLE, THEN THE BOXES SIT ON IT.
     *
     * It used to be laid down per column, and an EMPTY slot assigned to
     * the lower lane punched a hole in the UPPER lane's wire: both lanes
     * run to the output whatever is or is not on either of them, and a
     * per-column loop cannot know that. Runs and verticals first; the
     * boxes clear their own interiors afterwards.
     *
     * THE JOIN IS DRAWN EXACTLY AS THE FORK IS - a vertical joining the
     * two rails - because it is the same event read the other way round.
     * Drawing the parting and leaving the meeting implied was the shape
     * that made "where does this come back together" a question at all. */
    const fx = colX(split) - 1;                  /* the gap before the fork */
    const jx = colX(join) + COL_W;               /* the gap after the join  */
    if (split < 0) {
        fill_rect(IO_W, midWire, 128 - IO_W * 2, 1, 1);
    } else {
        fill_rect(IO_W, midWire, fx - IO_W + 1, 1, 1);
        fill_rect(fx, laneWire[0], 1, laneWire[1] - laneWire[0] + 1, 1);
        fill_rect(jx, laneWire[0], 1, laneWire[1] - laneWire[0] + 1, 1);
        for (let r = 0; r < 2; r++)
            fill_rect(fx, laneWire[r], jx - fx + 1, 1, 1);
        fill_rect(jx, midWire, 128 - IO_W - jx, 1, 1);
    }

    for (let b = 0; b < NUM_BLOCKS; b++) {
        /* A SLOT OFF THE PATH IS NOT DRAWN AT ALL, not drawn faintly.
         * The branch row before the fork is empty by construction - it is
         * what DEFINES the fork - so there is no wire there and a box
         * would be sitting on nothing. */
        if (!inPath(b)) continue;
        const t = st.type[b];
        const g = blockGeom(b);

        if (t !== TYPE_OFF) {
            /* THE FILL IS THE SELECTION, NOT THE STATE.
             *
             * It was the other way round - every block in circuit drawn
             * solid - and with eight columns of fourteen pixels that is a
             * row of black slabs with two clipped characters knocked out
             * of each. Rendered and looked at rather than reasoned about,
             * which is how it should have been done three rounds ago.
             *
             * Exactly ONE box is ever filled, so the screen is mostly
             * light and the thing you are editing is the only thing
             * shouting. */
            const chosen = (b === sel);
            const live = !!st.on[b];
            if (chosen) fill_rect(g.x, g.y, COL_W, g.h, 1);
            else        fill_rect(g.x, g.y, COL_W, g.h, 0);   /* off the wire */
            const ink = chosen ? 0 : 1;

            /* A BYPASSED BLOCK HAS NO SIDE WALLS, and the wire runs
             * straight through where they would be.
             *
             * A whole session went to "no sound, the amp is dead" with the
             * board's one loaded block bypassed. It was drawn then as a
             * closed box with the letter B in it - a difference of two
             * characters from a working one. This is a difference of
             * SHAPE: the signal visibly does not turn aside for it. */
            fill_rect(g.x, g.y, COL_W, 1, ink);
            fill_rect(g.x, g.y + g.h - 1, COL_W, 1, ink);
            if (live) {
                fill_rect(g.x, g.y, 1, g.h, ink);
                fill_rect(g.x + COL_W - 1, g.y, 1, g.h, ink);
            } else {
                fill_rect(g.x, g.wire, COL_W, 1, ink);
            }
            pcenter(g.x, COL_W, g.y + ((g.h - 7) >> 1), blockLabel(b), ink);
        } else if (b === sel) {
            /* An empty slot still has to be selectable - that is how a
             * block is given a type at all - so it gets the frame without
             * the fill. */
            draw_rect(g.x, g.y, COL_W, g.h, 1);
        }

    }
}

function drawCells() {
    const list = knobList();
    /* Four cells across; the row follows the knob last touched, so a
     * pedal's own controls are never off the end of the tree. */
    let first = 0;
    if (lastKnob >= 4) first = Math.min(lastKnob - 3, Math.max(0, list.length - 4));

    for (let i = first; i < list.length && i < first + 4; i++) {
        const col = i - first;
        const x = col * 32;
        const spec = list[i];
        /* A blank encoder draws nothing at all - not a box, not a dash.
         * Its ring is dark for the same reason and the two have to agree. */
        if (spec.kind === 'gap') continue;
        /* THE SAME INVERSION AS THE GRID: a filled 31x18 cell is the
         * second biggest black slab on the screen, and it is marking the
         * one thing you already know - the knob your hand is on. A frame
         * says it and leaves the panel light. */
        const on = (i === lastKnob);
        if (on) draw_rect(x, CELL_Y - 2, CELL_W, CELL_H, 1);
        const ink = 1;

        let shown;
        const v = st.val[fullKey(spec)];
        if (spec.kind === 'cat')        shown = CATS[catOfBlock(sel)].name;
        else if (spec.kind === 'fxid')  shown = st.names.fx[st.fx[sel]] || String(st.fx[sel]);
        else if (spec.kind === 'spec')  shown = (v === undefined) ? '-' : specText(spec.kn, v);
        else if (spec.kind === 'float') shown = (v === undefined) ? '-' : String(Math.round(v * 100));
        else if (spec.kind === 'enum')  shown = (v === undefined) ? '-'
            : ((spec.names ? spec.names[v] : String(v)) || String(v));
        else                            shown = (v === undefined) ? '-' : listNameFor(spec.key, v);

        ptext(x + 1, CELL_Y, spec.label, ink, CELL_W - 2);
        ptext(x + 1, CELL_Y + 9, shown, ink, CELL_W - 2);
        drawBar(x, CELL_W, spec.kind, spec.kn, knobFill(spec));
    }
    return list;
}

function drawFooter(list) {
    /* The last routing change wins the line while it is fresh: it is the
     * thing you just did, and the pedal's name is not going anywhere. */
    if (flashText && Date.now() < flashUntil) {
        ptext(1, FOOT_Y, flashText, 1, 126);
        return;
    }
    /* One line, and it prefers the thing that had to be cut above: a model
     * or pedal name is longer than a 31 px cell and is the one string here
     * you actually need in full.
     *
     * WHERE YOU ARE IN THE TREE, not just what is under the cursor. Twenty
     * six pedals behind two knobs is a place you can be lost in, and the
     * screen was not saying you were in one: the group cell clips `Filter`
     * to `Filte` at 31 px, and nothing anywhere said how many pedals the
     * group held or which of them this was. Asked from the device as
     * "where did the EQ go" - it had not gone anywhere, it is the first of
     * four in Filter, and that is the sentence the screen now prints. */
    if (st.type[sel] === TYPE_FX) {
        const id = st.fx[sel] | 0;
        const c = catOfBlock(sel);
        const items = CATS[c].items || [id];
        const at = items.indexOf(id);
        ptext(1, FOOT_Y, (st.names.fx[id] || String(id)) + '  ' +
              CATS[c].name + ' ' + (at + 1) + '/' + items.length, 1, 126);
        return;
    }
    const full = list.find(k => k.kind === 'list');
    if (full) {
        const v = st.val[fullKey(full)];
        if (v !== undefined) {
            ptext(1, FOOT_Y, listNameFor(full.key, v), 1, 126);
            return;
        }
    }
    ptext(1, FOOT_Y, 'tap:stomp  hold:edit', 1, 126);
}

function draw() {
    if (menuOpen) { drawMenu(); return; }
    clear_screen();
    drawHeader();
    drawBoxes();
    drawFooter(drawCells());
}

/* ------------------------------------------------------------------ LEDs */

/*
 * THE RINGS SAY WHICH ENCODERS DO SOMETHING.
 *
 * Eight knobs, and on most screens three or four of them are wired to
 * nothing - which is invisible until you turn one and nothing happens.
 * A dark ring is "this knob will do nothing", which is the whole of what
 * was asked for, and a lit one rides its value as brightness so the row
 * also reads as a rough picture of where everything is set.
 *
 * The two ramps follow the host's own grid (knobs 1-4 white, 5-8 amber) so
 * this screen does not teach a second colour language, and they are ordered
 * by LUMINANCE rather than by name - the host's knob_leds.mjs records that
 * picking amber constants by what they are called produces a sweep that
 * goes dim, bright, dark, bright. The hex is in constants.mjs:
 *
 *     white  #141414  #404040  #CCCCCC  #FFFFFF
 *     amber  #200D00  #5D1700  #AC1F00  #C93C00
 *
 * Colour 0 is reserved for unbound, so a bound knob at its minimum still
 * shows its floor - the row identity has to survive a value sitting at 0.
 */
const KNOB_CC = 71;
const RING_WHITE = [124, 118, 122, 120];   /* DarkGrey2 LightGrey OffWhite White */
const RING_AMBER = [70, 69, 4, 3];         /* DarkBrown BurntSienna Tan BrightOrange */
const ringCache = new Array(NUM_KNOBS).fill(-1);

/* How far along its range a knob is, 0..1, or null for "nothing here".
 * null and 0 are different answers and must stay different: an unread key
 * lit at the bottom of its range would be a confident lie about a control
 * that may not exist. */
function knobFill(spec) {
    if (!spec || spec.kind === 'gap') return null;
    if (spec.kind === 'cat')
        return catOfBlock(sel) / (CATS.length - 1);
    if (spec.kind === 'fxid') {
        const items = CATS[catOfBlock(sel)].items || [];
        const i = items.indexOf(st.fx[sel] | 0);
        return (items.length < 2 || i < 0) ? 1 : i / (items.length - 1);
    }
    const v = st.val[fullKey(spec)];
    if (v === undefined || !Number.isFinite(v)) return null;
    if (spec.kind === 'float' || spec.kind === 'spec') return v;
    const n = (spec.kind === 'enum') ? spec.n
        : Math.max(1, ((spec.key === 'model') ? st.names.model
                                              : st.names.cab).length);
    return (n < 2) ? 1 : Math.max(0, Math.min(1, v / (n - 1)));
}

function paintKnobLeds() {
    const list = knobList();
    for (let k = 0; k < NUM_KNOBS; k++) {
        const f = knobFill(list[k]);
        let color = 0;
        if (f !== null) {
            const ramp = (k < 4) ? RING_WHITE : RING_AMBER;
            const i = Math.min(ramp.length - 1,
                               Math.floor(Math.max(0, Math.min(1, f)) * ramp.length));
            color = ramp[i];
        }
        if (ringCache[k] === color) continue;
        ringCache[k] = color;
        /* force, and diff here. input_filter's own cache cannot be
         * invalidated per key and the shim repaints the surface underneath
         * it on the way in, so a cache we do not own would suppress the
         * write that corrects it. */
        try { setButtonLED(KNOB_CC + k, color, true); } catch (e) {}
    }
}

function paintLeds() {
    paintKnobLeds();
    /* BOTH ROWS ARE THE SAME KIND OF THING NOW, so they are painted by
     * one loop with one rule. The old top row carried a second language -
     * two brightnesses of pink meaning which side of a split a block was
     * on - which was three things to decode on one row and was reported,
     * twice, as unreadable. There is nothing left to decode: a pad shows
     * its own block's category, dim when bypassed, dark when empty. */
    const pathFork = forkCol();
    for (let b = 0; b < NUM_BLOCKS; b++) {
        const note = (rowOf(b) === ROW_BR ? ROUTE_BASE : PAD_BASE) + colOf(b);
        /* Selection BLINKS rather than taking a colour of its own, because
         * "which one am I editing" and "what is this block" are different
         * questions and one colour cannot answer both - and now that the
         * colour carries the category, spending it on selection would be
         * spending the more useful of the two. */
        let c;
        if (b === sel && (Date.now() % 700) < 350) {
            c = White;
        } else if (rowOf(b) === ROW_BR && pathFork >= 0 && colOf(b) < pathFork) {
            /* Off the path: before the fork the branch row carries no
             * signal, and lighting it would offer a stomp with nothing
             * to stomp. It is still HOLDable - that is how you move the
             * fork earlier. */
            c = 0;
        } else {
            const led = CATS[catOfBlock(b)].led;
            c = (st.type[b] === TYPE_OFF) ? 0 : (st.on[b] ? led[0] : led[1]);
        }
        setLED(note, c);
    }
}

/* ----------------------------------------------------------------- input */

/*
 * A STOMP SAYS WHICH WAY IT WENT.
 *
 * A session was spent on "there is no sound, the amp is dead" with the
 * board's only loaded block bypassed - the log said so plainly and the
 * screen said it with the single letter B inside a fifteen-pixel box,
 * next to a number that is also one or two characters. The thing that
 * silences a block should not be the most easily missed thing on the
 * panel.
 */
/* SHIFT + A PAD SAYS HOW FAR THE BRANCH RUNS.
 *
 * The merge was a menu row for one release and came back as still hard:
 * it is six rows down a list that scrolls at five, and it is the only
 * thing about the routing that is not under your hands. The split is a
 * pad, so this is a pad.
 *
 * The COLUMN you press is the last parallel one - "it runs this far" -
 * which is what the lit run on the top row already shows, so the gesture
 * changes the length of a thing you can see. Pressing the column it is
 * already set to puts it back to Out.
 *
 * Either row, because a column is what you are choosing and making the
 * top row the only one that works would be a rule to learn for nothing.
 */
function setMergeCol(c) {
    const cur = num(st.val['merge'], 0);
    const v = (cur === c + 1) ? 0 : c + 1;
    st.val['merge'] = v;
    setp('merge', v);
    flash(v === 0 ? 'Merge: at Out' : ('Merge after ' + v));
}

function stomp(b) {
    const nowOn = st.on[b];
    st.on[b] = nowOn ? 0 : 1;
    flash(blockName(b) + (st.on[b] ? ' ON' : ' BYPASS'));
    /* Written through rather than waiting for the rotation to notice: the
     * pad has to answer under the finger. */
    setp(keyOf(b, 'on'), st.on[b] ? 0 : 1);
}

function select(b) {
    sel = b;
    /* The header carries this too, but the header is where it ALWAYS is -
     * which makes it a state and not an answer to "did that hold take".
     * Same reasoning as the routing messages. */
    flash('Edit ' + blockName(b) + (blockLabel(b) ? ' ' + blockLabel(b) : ''));
    setp('sel_block', b);
    readSelected();
}

function onKnob(idx, ccValue) {
    const list = knobList();
    if (idx >= list.length) return;
    const s = list[idx];
    /* A blank claims its encoder and does nothing - including not moving
     * the cell row onto itself, which would scroll the strip to show four
     * empty cells. */
    if (s.kind === 'gap') return;
    lastKnob = idx;
    lastPaint = 0;
    const d = detents(ccValue);
    if (!d) return;
    const k = fullKey(s);
    let v = st.val[k];

    if (s.kind === 'cat') {
        const step = knobSteps(idx, d);
        if (!step) return;
        const was = st.type[sel];
        let c = catOfBlock(sel) + step;
        if (c < 0) c = 0;
        if (c > CATS.length - 1) c = CATS.length - 1;
        if (c === catOfBlock(sel)) return;
        const cat = CATS[c];
        /* Writing the pedal BEFORE the type, because the type write is
         * what makes the plugin reach for a default - and a block that
         * lands on its default for a frame is a block that makes a
         * different noise for a frame. */
        if (cat.items) {
            st.fx[sel] = cat.items[0];
            st.val[selKey('fx')] = cat.items[0];
            setp(selKey('fx'), cat.items[0]);
        }
        /* A NEW BLOCK ARRIVES BYPASSED.
         *
         * Turning knob 1 past Off used to put a pedal straight into the
         * signal at whatever its defaults were, and a .nam is the worst
         * case: the device log has `out pk` stepping from 0.08 to 1.32
         * across one load. Through headphones that is "오디오가 팍 튀어서
         * 귀아파", and it happens while you are still LOOKING for the
         * pedal, so it happens once per candidate.
         *
         * Only on the way OUT of Off. Changing a live block's category
         * must not silence it - you are auditioning, and having to stomp
         * it back in every time would be its own complaint. */
        if (was === TYPE_OFF && cat.type !== TYPE_OFF) {
            st.on[sel] = 0;
            st.val[selKey('on')] = 1;
            setp(selKey('on'), 1);
        }
        st.type[sel] = cat.type;
        st.val[selKey('type')] = cat.type;
        setp(selKey('type'), cat.type);
        if (st.on[sel] === 0) flash(blockName(sel) + ' ADDED - BYPASSED');
        readSelected();
        return;
    }

    if (s.kind === 'fxid') {
        const step = knobSteps(idx, d);
        if (!step) return;
        const items = CATS[catOfBlock(sel)].items || [];
        const cur = st.fx[sel] | 0;
        let i = items.indexOf(cur) + step;
        if (i < 0) i = 0;
        if (i > items.length - 1) i = items.length - 1;
        const want = items[i];
        if (want === undefined || want === cur) return;
        st.fx[sel] = want;
        st.val[selKey('fx')] = want;
        setp(selKey('fx'), want);
        /* The pedal changed, so its knobs are a different set - what they
         * are pointing at has to be re-read rather than carried over. */
        readSelected();
        return;
    }

    if (s.kind === 'spec') {
        /* A SWITCH STEPS, A CONTINUOUS KNOB SLIDES, and which it is comes
         * from the served table rather than from anything here. */
        if (s.kn.c === 2) {
            const step = knobSteps(idx, d);
            if (!step) return;
            const n = Math.max(1, s.kn.hi | 0);
            let cur = Math.round((v === undefined ? 0 : v) * (n - 1)) + step;
            if (cur < 0) cur = 0;
            if (cur > n - 1) cur = n - 1;
            const nv = (n < 2) ? 0 : cur / (n - 1);
            if (nv === v) return;
            st.val[k] = nv;
            setp(k, nv.toFixed(4));
            /* A mode switch re-labels its neighbours, so the row has to be
             * rebuilt rather than redrawn. */
            lastPaint = 0;
        } else {
            let nv = (v === undefined ? 0.5 : v) + d * FLOAT_STEP;
            if (nv < 0) nv = 0;
            if (nv > 1) nv = 1;
            st.val[k] = nv;
            setp(k, nv.toFixed(4));
        }
        return;
    }

    if (s.kind === 'float') {
        v = (v === undefined ? 0.5 : v) + d * FLOAT_STEP;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        st.val[k] = v;
        setp(k, v.toFixed(4));
    } else {
        const step = knobSteps(idx, d);
        if (!step) return;
        const n = (s.kind === 'enum') ? s.n
            : Math.max(1, ((s.key === 'model') ? st.names.model : st.names.cab).length);
        v = (v === undefined ? 0 : v) + step;
        if (v < 0) v = 0;
        if (v > n - 1) v = n - 1;
        if (v === st.val[k]) return;
        st.val[k] = v;
        setp(k, v);
    }
}

/* ------------------------------------------------------------ lifecycle */

globalThis.chain_ui = {
    init() {
        for (let i = 0; i < NUM_COLS; i++) {
            padDownAt[i] = 0; padHandled[i] = false;
            routeDownAt[i] = 0; routeHandled[i] = false;
        }
        /* The shim replays Move's own LED state on the way in, so what the
         * cache believes is stale. Re-emit everything once. */
        invalidateLedCache();
        for (let i = 0; i < NUM_KNOBS; i++) ringCache[i] = -1;
        resetKnobAcc();
        loadLists();
        st.build = getp('build') || '?';
        /* THE MAIN ROW'S FIRST BLOCK IS WHERE THIS OPENS. Slot 0 is the
         * top row, which on a mono board is off the path entirely - so
         * defaulting to it lands the editor on a block that is not in the
         * signal and draws no box. */
        sel = num(getp('sel_block'), slotOf(ROW_MAIN, 0));
        for (let b = 0; b < NUM_BLOCKS; b++) {
            st.type[b] = num(getp(keyOf(b, 'type')), 0);
            st.on[b] = num(getp(keyOf(b, 'on')), 0) === 0 ? 1 : 0;
            st.fx[b] = num(getp(keyOf(b, 'fx')), 0);
        }
        st.cpu = num(getp('cpu'), 0);
        for (const key of ['in_level', 'out_level', 'merge', 'pan_a', 'pan_b']) {
            const v = getp(key);
            if (v !== null && v !== '') st.val[key] = Number(v);
        }
        readSelected();
    },

    tick() {
        try { tickBody(); }
        catch (e) {
            /* There is no host grid behind this screen any more, so an
             * uncaught throw here is a black panel with working knobs and
             * nothing anywhere saying why. Draw the reason instead. */
            try {
                clear_screen();
                print(2, 2, 'Nam A2c UI error', 1);
                print(2, 14, String(e && e.message ? e.message : e).slice(0, 24), 1);
                print(2, 30, 'Back to leave', 1);
            } catch (e2) { }
        }
    },

    _tick() { tickBody(); },
};

/* Standalone, so `this` is never part of whether the screen draws. */
function tickBody() {
        /* RESTATED, never memoised. The shim drops pad_block unilaterally
         * from four SPI-callback sites that never tell JS, so a mirror
         * latches and the pads die silently after the first Menu dismiss.
         * It is idempotent against the SHM, so a per-frame restate is free. */
        if (typeof host_pad_block === 'function') host_pad_block(1);

        rotateRead();

        /* A hold fires while the finger is still down - waiting for the
         * release would make selecting feel like a slow tap. */
        const now = Date.now();
        for (let b = 0; b < NUM_COLS; b++) {
            if (padDownAt[b] && !padHandled[b] && now - padDownAt[b] >= HOLD_MS) {
                padHandled[b] = true;
                select(slotOf(ROW_MAIN, b));
            }
            if (routeDownAt[b] && !routeHandled[b] && now - routeDownAt[b] >= HOLD_MS) {
                routeHandled[b] = true;
                select(slotOf(ROW_BR, b));
            }
        }

        paintLeds();
        if (now - lastPaint >= 33) { lastPaint = now; draw(); }
}

globalThis.chain_ui.onMidiMessageInternal = function(data) {
        if (!data || data.length < 3) return;
        const status = data[0] & 0xf0;
        const d1 = data[1], d2 = data[2];

        /* Shift is a MODIFIER again, for one thing: Shift + a pad sets how
         * far the branch runs. Tracked rather than ignored, and still
         * swallowed, so it cannot fall through to a branch reading CC
         * numbers it does not own. */
        if (status === 0xb0 && d1 === CC_SHIFT) { shiftHeld = d2 > 0; return; }

        /* Jog: click opens the menu, turn scrolls it. The jog is free here -
         * the host's own COMPONENT_EDIT jog handler never runs, because MIDI
         * is routed to a loaded module UI before it (shadow_ui.js:27167). */
        if (status === 0xb0 && d1 === CC_JOG_CLICK && d2 > 0) {
            /* Two rounds were spent unable to tell "the menu did not open"
             * from "the click never reached the module". One line settles
             * it; console.log from a component UI is routed to the unified
             * log. */
            try { console.log('A2c: jog click -> ' + (menuOpen ? 'run' : 'open menu')); } catch (e) {}
            if (menuOpen) runMenuRow(); else openMenu();
            return;
        }
        if (status === 0xb0 && d1 === CC_JOG_TURN && menuOpen) {
            const d = detents(d2);
            if (d) {
                menuCursor += (d > 0 ? 1 : -1);
                if (menuCursor < 0) menuCursor = 0;
                if (menuCursor > menuRows.length - 1) menuCursor = menuRows.length - 1;
                lastPaint = 0;   /* the cursor moved; show it */
            }
            return;
        }
        /* While the menu is up the encoders and pads are ITS business, not
         * the pedalboard's - a stomp landing behind an open menu is a
         * change you cannot see, and a knob turn is a block edit you are
         * not looking at.
         *
         * ITS business includes the two rows that hold a value: with the
         * cursor on Input or Output, ANY encoder sets it. No new gesture
         * and no sub-screen - the row is already highlighted, so the thing
         * a knob would obviously do is the thing it does. */
        if (menuOpen) {
            if (status === 0xb0 && d1 >= CC_KNOB_BASE && d1 < CC_KNOB_BASE + 8) {
                const row = menuRows[menuCursor];
                const dd = detents(d2);
                if (row && row.edit && dd) {
                    let v = st.val[row.edit];
                    if (row.kind === 'merge') {
                        /* A COLUMN, so it steps - at the same three
                         * detents everything discrete on this screen
                         * costs. Column 1 is skipped rather than clamped:
                         * nothing can be one signal again before the
                         * earliest column it could have parted at. */
                        const step = knobSteps(d1 - CC_KNOB_BASE, dd);
                        if (!step) return;
                        v = Math.round(v === undefined ? 0 : v) + step;
                        if (v < 0) v = 0;
                        if (v > NUM_COLS) v = NUM_COLS;
                        st.val[row.edit] = v;
                        setp(row.edit, v);
                    } else if (row.kind === 'pan') {
                        v = (v === undefined ? 0 : v) + dd * FLOAT_STEP * 2;
                        if (v < -1) v = -1;
                        if (v > 1) v = 1;
                        st.val[row.edit] = v;
                        setp(row.edit, v.toFixed(4));
                    } else {
                        v = (v === undefined ? 0.5 : v) + dd * FLOAT_STEP;
                        if (v < 0) v = 0;
                        if (v > 1) v = 1;
                        st.val[row.edit] = v;
                        setp(row.edit, v.toFixed(4));
                    }
                    lastPaint = 0;
                }
            }
            return;
        }

        if (status === 0xb0 && d1 >= CC_KNOB_BASE && d1 < CC_KNOB_BASE + 8) {
            onKnob(d1 - CC_KNOB_BASE, d2);
            return;
        }

        /* THE TOP ROW IS BLOCKS, AND IT TAKES THE SAME TWO GESTURES.
         * One rule for sixteen pads: tap stomps, hold edits. */
        if (d1 >= ROUTE_BASE && d1 < ROUTE_BASE + NUM_COLS) {
            const r = d1 - ROUTE_BASE;
            if (status === 0x90 && d2 > 0) {
                /* Handled on the DOWN edge and marked done, so the
                 * release cannot also stomp the block underneath it. */
                if (shiftHeld) { routeDownAt[r] = 0; routeHandled[r] = true;
                                 setMergeCol(r); return; }
                routeDownAt[r] = Date.now();
                routeHandled[r] = false;
            } else if (status === 0x80 || (status === 0x90 && d2 === 0)) {
                if (routeDownAt[r] && !routeHandled[r]) stomp(slotOf(ROW_BR, r));
                routeDownAt[r] = 0;
                routeHandled[r] = false;
            }
            return;
        }

        if (d1 < PAD_BASE || d1 >= PAD_BASE + NUM_COLS) return;
        const b = d1 - PAD_BASE;

        if (status === 0x90 && d2 > 0) {
            if (shiftHeld) { padDownAt[b] = 0; padHandled[b] = true;
                             setMergeCol(b); return; }
            padDownAt[b] = Date.now();
            padHandled[b] = false;
        } else if (status === 0x80 || (status === 0x90 && d2 === 0)) {
            /* A block pad stomps, full stop. Shift used to move it
             * between lanes here as well, which was one gesture doing two
             * jobs - and a lane is the ROW a pad is in now, so there is
             * nothing left for it to mean. */
            if (padDownAt[b] && !padHandled[b]) stomp(slotOf(ROW_MAIN, b));
            padDownAt[b] = 0;
            padHandled[b] = false;
        }
};

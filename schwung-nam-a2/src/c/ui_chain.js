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

const NUM_BLOCKS = 8;
const PAD_BASE = 68;            /* bottom row, notes 68..75 */
const HOLD_MS = 350;

const CC_KNOB_BASE = 71;        /* knobs 1..8 are CC 71..78 */
const CC_JOG_TURN = 14;
const CC_JOG_CLICK = 3;

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


let sel = 0;
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

let padDownAt = [];             /* when each pad went down, or 0 */
let padHandled = [];            /* hold already fired, so the release is not a tap */
let lastPaint = 0;
let lastKnob = 0;   /* which cell the row is following */

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
const DETENTS_PER_STEP = 3;
const FLOAT_STEP = 0.01;
const knobAcc = new Array(NUM_KNOBS).fill(0);

function resetKnobAcc() { for (let i = 0; i < NUM_KNOBS; i++) knobAcc[i] = 0; }

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
    const steps = NUM_BLOCKS * 2 + 1;
    const i = st.rot % steps;
    st.rot++;
    if (i === steps - 1) {
        st.cpu = num(getp('cpu'), st.cpu);
        return;
    }
    const b = i >> 1;
    if ((i & 1) === 0) {
        st.type[b] = num(getp('b' + (b + 1) + '_type'), st.type[b]);
        st.on[b] = num(getp('b' + (b + 1) + '_on'), st.on[b] ? 0 : 1) === 0 ? 1 : 0;
        st.fx[b] = num(getp('b' + (b + 1) + '_fx'), st.fx[b]);
    } else {
        st.blockCpu[b] = num(getp('b' + (b + 1) + '_cpu'), st.blockCpu[b]);
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

    try { console.log('A2c menu: ' + why + ', ' + menuRows.length + ' rows'); } catch (e) {}

    menuRows.push({ label: 'Close', action: null });
    menuCursor = 0;
    menuTop = 0;
    menuOpen = true;
    lastPaint = 0;      /* repaint NOW, not up to 33 ms from now */
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
            pright(126, y, (v === undefined) ? '-'
                   : (Math.round(v * 100) + '%'), ink, 44);
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

function selKey(k) { return 'b' + (sel + 1) + '_' + k; }

function knobList() { return padToBoard(knobsFor(st.type[sel], st.fx[sel])); }

function readSelected() {
    for (const s of knobList()) {
        if (s.kind === 'gap') continue;
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
const BOX_Y = 12, BOX_H = 18, BOX_W = 15, BOX_GAP = 1;   /* 8*15 + 7*1 = 127 */
const SELBAR_Y = BOX_Y + BOX_H + 2;                      /*  32            */
const CELL_Y = 36, CELL_H = 19, CELL_W = 31;             /* 4 * 32 = 128    */
const FOOT_Y = 56;   /* 56 + 8 = 64 exactly; 57 runs a pixel off the bottom */

function blockLabel(b) {
    const t = st.type[b];
    if (t === TYPE_FX)  return st.names.fx[st.fx[b]] || 'FX';
    if (t === TYPE_NAM) return 'NAM';
    if (t === TYPE_CAB) return 'CAB';
    return '';
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
    /* What is selected and what it is - the one line worth reading first. */
    const what = blockLabel(sel);
    ptext(1, 1, 'B' + (sel + 1) + (what ? ' ' + what : ' --'), 1, 128 - cw - 2);
    fill_rect(0, 10, 128, 1, 1);
}

function drawBoxes() {
    for (let b = 0; b < NUM_BLOCKS; b++) {
        const x = b * (BOX_W + BOX_GAP);
        const t = st.type[b];
        const live = t !== TYPE_OFF && st.on[b];

        if (live) fill_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        else      draw_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        const ink = live ? 0 : 1;

        if (t === TYPE_OFF) {
            pcenter(x, BOX_W, BOX_Y + 6, '-', ink);
        } else {
            pcenter(x, BOX_W, BOX_Y + 2, blockLabel(b), ink);
            /* A block that is switched off says so; one that is on shows
             * what it costs. Empty and bypassed have to be tellable apart -
             * that is most of what the picture is for. */
            pcenter(x, BOX_W, BOX_Y + 10,
                    st.on[b] ? String(Math.round(st.blockCpu[b])) : 'B', ink);
        }

        /* Selection is a bar UNDER the box, not a border around it: a border
         * needs a pixel on each side and there is not one to spare. */
        if (b === sel) fill_rect(x, SELBAR_Y, BOX_W, 2, 1);
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
        const on = (i === lastKnob);
        if (on) fill_rect(x, CELL_Y - 2, CELL_W, CELL_H, 1);
        const ink = on ? 0 : 1;

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
    }
    return list;
}

function drawFooter(list) {
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
    for (let b = 0; b < NUM_BLOCKS; b++) {
        /* Selection BLINKS rather than taking a colour of its own, because
         * "which one am I editing" and "what is this block" are different
         * questions and one colour cannot answer both - and now that the
         * colour carries the category, spending it on selection would be
         * spending the more useful of the two. */
        let c;
        if (b === sel && (Date.now() % 700) < 350) {
            c = White;
        } else {
            const led = CATS[catOfBlock(b)].led;
            c = (st.type[b] === TYPE_OFF) ? 0 : (st.on[b] ? led[0] : led[1]);
        }
        setLED(PAD_BASE + b, c);
    }
}

/* ----------------------------------------------------------------- input */

function stomp(b) {
    const nowOn = st.on[b];
    st.on[b] = nowOn ? 0 : 1;
    /* Written through rather than waiting for the rotation to notice: the
     * pad has to answer under the finger. */
    setp('b' + (b + 1) + '_on', st.on[b] ? 0 : 1);
}

function select(b) {
    sel = b;
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
    const d = decodeDelta(ccValue);
    if (!d) return;
    const k = fullKey(s);
    let v = st.val[k];

    if (s.kind === 'cat') {
        const step = knobSteps(idx, d);
        if (!step) return;
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
        st.type[sel] = cat.type;
        st.val[selKey('type')] = cat.type;
        setp(selKey('type'), cat.type);
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
        for (let i = 0; i < NUM_BLOCKS; i++) { padDownAt[i] = 0; padHandled[i] = false; }
        /* The shim replays Move's own LED state on the way in, so what the
         * cache believes is stale. Re-emit everything once. */
        invalidateLedCache();
        for (let i = 0; i < NUM_KNOBS; i++) ringCache[i] = -1;
        resetKnobAcc();
        loadLists();
        st.build = getp('build') || '?';
        sel = num(getp('sel_block'), 0);
        for (let b = 0; b < NUM_BLOCKS; b++) {
            st.type[b] = num(getp('b' + (b + 1) + '_type'), 0);
            st.on[b] = num(getp('b' + (b + 1) + '_on'), 0) === 0 ? 1 : 0;
            st.fx[b] = num(getp('b' + (b + 1) + '_fx'), 0);
        }
        st.cpu = num(getp('cpu'), 0);
        for (const key of ['in_level', 'out_level']) {
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
        for (let b = 0; b < NUM_BLOCKS; b++) {
            if (padDownAt[b] && !padHandled[b] && now - padDownAt[b] >= HOLD_MS) {
                padHandled[b] = true;
                select(b);
            }
        }

        paintLeds();
        if (now - lastPaint >= 33) { lastPaint = now; draw(); }
}

globalThis.chain_ui.onMidiMessageInternal = function(data) {
        if (!data || data.length < 3) return;
        const status = data[0] & 0xf0;
        const d1 = data[1], d2 = data[2];

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
            const d = decodeDelta(d2);
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
                const dd = decodeDelta(d2);
                if (row && row.edit && dd) {
                    let v = st.val[row.edit];
                    v = (v === undefined ? 0.5 : v) + dd * FLOAT_STEP;
                    if (v < 0) v = 0;
                    if (v > 1) v = 1;
                    st.val[row.edit] = v;
                    setp(row.edit, v.toFixed(4));
                    lastPaint = 0;
                }
            }
            return;
        }

        if (status === 0xb0 && d1 >= CC_KNOB_BASE && d1 < CC_KNOB_BASE + 8) {
            onKnob(d1 - CC_KNOB_BASE, d2);
            return;
        }

        if (d1 < PAD_BASE || d1 >= PAD_BASE + NUM_BLOCKS) return;
        const b = d1 - PAD_BASE;

        if (status === 0x90 && d2 > 0) {
            padDownAt[b] = Date.now();
            padHandled[b] = false;
        } else if (status === 0x80 || (status === 0x90 && d2 === 0)) {
            if (padDownAt[b] && !padHandled[b]) stomp(b);
            padDownAt[b] = 0;
            padHandled[b] = false;
        }
};

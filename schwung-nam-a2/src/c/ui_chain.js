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

import { setLED, decodeDelta, invalidateLedCache } from '/data/UserData/schwung/shared/input_filter.mjs';

const NUM_BLOCKS = 8;
const PAD_BASE = 68;            /* bottom row, notes 68..75 */
const HOLD_MS = 350;

const CC_KNOB_BASE = 71;        /* knobs 1..8 are CC 71..78 */
const CC_JOG_TURN = 14;
const CC_JOG_CLICK = 3;

const TYPE_OFF = 0, TYPE_NAM = 1, TYPE_CAB = 2, TYPE_FX = 3;
const TYPE_ABBREV = ['--', 'NAM', 'CAB', 'FX'];

/* THE TREE. Coarse to fine, one knob per level, which is the whole of what
 * was asked for: knob 1 picks what KIND of thing this block is, knob 2 the
 * family, knob 3 the pedal. Sixteen pedals on one knob would be a knob you
 * have to count on.
 *
 * The ids are the DSP's flat wire values and the grouping is ours - the
 * plugin does not know these families exist, so adding a pedal is a number
 * in one of these rows plus a case in fx_block_process. */
const FX_TREE = [
    { name: 'Drive',  items: [0, 1, 2, 3] },      /* OD, Dist, Fuzz, Boost   */
    { name: 'Dynamic', items: [4, 5] },           /* Comp, Gate              */
    { name: 'Filter', items: [6, 7] },            /* EQ, Auto Wah            */
    { name: 'Mod',    items: [8, 9, 10] },        /* Chorus, Phaser, Trem    */
    { name: 'Time',   items: [11, 12, 13] },      /* Delay, Slap, Reverb     */
    { name: 'Pitch',  items: [14, 15] },          /* Doubler, Detune         */
];

/* What each pedal's five knobs are called. Unnamed ones are not offered -
 * a knob that does nothing is worse than a knob that is not there. */
const FX_PARAM_NAMES = [
    ['Drive', 'Tone', 'Level'],      /* 0  Overdrive  */
    ['Dist', 'Tone', 'Level'],       /* 1  Distortion */
    ['Fuzz', 'Tone', 'Level'],       /* 2  Fuzz       */
    ['Boost', 'Tone', 'Level'],      /* 3  Boost      */
    ['Thresh', 'Ratio', 'Makeup'],   /* 4  Compressor */
    ['Thresh', 'Release'],           /* 5  Gate       */
    ['Bass', 'Mid', 'Treble'],       /* 6  EQ         */
    ['Sens', 'Range', 'Mix'],        /* 7  Auto Wah   */
    ['Rate', 'Depth', 'Mix'],        /* 8  Chorus     */
    ['Rate', 'Depth', 'Mix'],        /* 9  Phaser     */
    ['Rate', 'Depth'],               /* 10 Tremolo    */
    ['Time', 'Fdbk', 'Mix'],         /* 11 Delay      */
    ['Time', 'Fdbk', 'Mix'],         /* 12 Slapback   */
    ['Size', 'Damp', 'Mix'],         /* 13 Reverb     */
    ['Amount', '', 'Mix'],           /* 14 Doubler    */
    ['Amount', '', 'Mix'],           /* 15 Detune     */
];

function fxCategoryOf(id) {
    for (let c = 0; c < FX_TREE.length; c++)
        if (FX_TREE[c].items.indexOf(id) >= 0) return c;
    return 0;
}

/* Pad colour says the TYPE; brightness says whether it is in circuit.
 * Selected blinks, because "which one am I editing" is a different question
 * from "what is switched on" and one colour cannot answer both. */
const Black = 0, White = 120;
const TYPE_LED = [
    { on: 0,   off: 0   },   /* Off    - dark */
    { on: 127, off: 68  },   /* NAM    - red / dark red */
    { on: 3,   off: 70  },   /* Cab    - orange / dark orange */
    { on: 8,   off: 80  },   /* Drive  - yellow / dark yellow */
];

/* The encoders, per type. Short because the DSP is ours. Knob 1 is always
 * Type, so a block can be re-typed without leaving the pedalboard. */
const TYPE_KNOB = { key: 'type', label: 'Type', kind: 'enum', n: 4 };

/* Built per block rather than declared, because past knob 3 the list is a
 * property of the PEDAL and there are sixteen of them. */
function knobsFor(type, fxId) {
    if (type === TYPE_NAM)
        return [TYPE_KNOB,
                { key: 'model', label: 'Model', kind: 'list' },
                { key: 'quality', label: 'Qual', kind: 'enum', n: 3,
                  names: ['Full', 'Slim', 'Lite'] }];
    if (type === TYPE_CAB)
        return [TYPE_KNOB, { key: 'cab', label: 'Cab', kind: 'list' }];
    if (type === TYPE_FX) {
        const out = [TYPE_KNOB,
                     { key: 'fx', label: 'Group', kind: 'fxcat' },
                     { key: 'fx', label: 'Pedal', kind: 'fxid' }];
        const names = FX_PARAM_NAMES[fxId] || [];
        for (let i = 0; i < names.length; i++) {
            if (!names[i]) continue;
            out.push({ key: 'p' + (i + 1), label: names[i], kind: 'float' });
        }
        return out;
    }
    return [TYPE_KNOB];
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
        ptext(2, y, menuRows[idx].label, idx === menuCursor ? 0 : 1, 124);
    }
    /* The picker's FIRST row is None, and picking it is how a module is
     * removed on this host. Saying "Swap / Remove" without saying where the
     * Remove is leaves you scrolling a list of thirty modules looking for
     * a row that is above all of them. */
    const cur = menuRows[menuCursor];
    if (cur && cur.action === SWAP_ROW)
        ptext(2, 55, 'pick None (top) to remove', 1, 124);
}

function selKey(k) { return 'b' + (sel + 1) + '_' + k; }

function knobList() { return knobsFor(st.type[sel], st.fx[sel]); }

function readSelected() {
    for (const s of knobList()) {
        const k = selKey(s.key);
        const v = getp(k);
        if (v !== null && v !== '') st.val[k] = Number(v);
    }
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
        const on = (i === lastKnob);
        if (on) fill_rect(x, CELL_Y - 2, CELL_W, CELL_H, 1);
        const ink = on ? 0 : 1;

        let shown;
        const v = st.val[selKey(spec.key)];
        if (spec.kind === 'fxcat')      shown = FX_TREE[fxCategoryOf(st.fx[sel])].name;
        else if (spec.kind === 'fxid')  shown = st.names.fx[st.fx[sel]] || String(st.fx[sel]);
        else if (spec.kind === 'float') shown = (v === undefined) ? '-' : String(Math.round(v * 100));
        else if (spec.kind === 'enum')  shown = (v === undefined) ? '-'
            : ((spec.names ? spec.names[v] : TYPE_ABBREV[v]) || String(v));
        else                            shown = (v === undefined) ? '-' : listNameFor(spec.key, v);

        ptext(x + 1, CELL_Y, spec.label, ink, CELL_W - 2);
        ptext(x + 1, CELL_Y + 9, shown, ink, CELL_W - 2);
    }
    return list;
}

function drawFooter(list) {
    /* One line, and it prefers the thing that had to be cut above: a model
     * or pedal name is longer than a 31 px cell and is the one string here
     * you actually need in full. */
    const full = list.find(k => k.kind === 'list' || k.kind === 'fxid');
    if (full) {
        const v = (full.kind === 'fxid') ? st.fx[sel] : st.val[selKey(full.key)];
        if (v !== undefined) {
            ptext(1, FOOT_Y, (full.kind === 'fxid')
                    ? (st.names.fx[v] || String(v))
                    : listNameFor(full.key, v), 1, 126);
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

function paintLeds() {
    for (let b = 0; b < NUM_BLOCKS; b++) {
        const t = st.type[b];
        let c;
        if (b === sel && (Date.now() % 700) < 350) c = White;
        else if (t === TYPE_OFF) c = Black;
        else c = st.on[b] ? TYPE_LED[t].on : TYPE_LED[t].off;
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
    lastKnob = idx;
    lastPaint = 0;
    const s = list[idx];
    const d = decodeDelta(ccValue);
    if (!d) return;
    const k = selKey(s.key);
    let v = st.val[k];

    if (s.kind === 'fxcat' || s.kind === 'fxid') {
        const cur = st.fx[sel] | 0;
        const cat = fxCategoryOf(cur);
        let want;
        if (s.kind === 'fxcat') {
            /* A family, not a pedal: land on its first. Carrying the
             * position across would mean turning one knob changed two
             * things, and the second one invisibly. */
            let c = cat + (d > 0 ? 1 : -1);
            if (c < 0) c = 0;
            if (c > FX_TREE.length - 1) c = FX_TREE.length - 1;
            if (c === cat) return;
            want = FX_TREE[c].items[0];
        } else {
            const items = FX_TREE[cat].items;
            let i = items.indexOf(cur) + (d > 0 ? 1 : -1);
            if (i < 0) i = 0;
            if (i > items.length - 1) i = items.length - 1;
            want = items[i];
        }
        if (want === cur) return;
        st.fx[sel] = want;
        st.val[selKey('fx')] = want;
        setp(selKey('fx'), want);
        /* The pedal changed, so its knobs are a different set - what they
         * are pointing at has to be re-read rather than carried over. */
        readSelected();
        return;
    }

    if (s.kind === 'float') {
        v = (v === undefined ? 0.5 : v) + d * 0.02;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        st.val[k] = v;
        setp(k, v.toFixed(4));
    } else {
        const n = (s.kind === 'enum') ? s.n
            : Math.max(1, ((s.key === 'model') ? st.names.model : st.names.cab).length);
        v = (v === undefined ? 0 : v) + (d > 0 ? 1 : -1);
        if (v < 0) v = 0;
        if (v > n - 1) v = n - 1;
        st.val[k] = v;
        setp(k, v);
        if (s.key === 'type') {
            st.type[sel] = v;
            /* The knob list just changed under the encoders, so what they
             * are pointing at has to be re-read rather than carried over
             * from the type that is gone. */
            readSelected();
        }
    }
}

/* ------------------------------------------------------------ lifecycle */

globalThis.chain_ui = {
    init() {
        for (let i = 0; i < NUM_BLOCKS; i++) { padDownAt[i] = 0; padHandled[i] = false; }
        /* The shim replays Move's own LED state on the way in, so what the
         * cache believes is stale. Re-emit everything once. */
        invalidateLedCache();
        loadLists();
        st.build = getp('build') || '?';
        sel = num(getp('sel_block'), 0);
        for (let b = 0; b < NUM_BLOCKS; b++) {
            st.type[b] = num(getp('b' + (b + 1) + '_type'), 0);
            st.on[b] = num(getp('b' + (b + 1) + '_on'), 0) === 0 ? 1 : 0;
            st.fx[b] = num(getp('b' + (b + 1) + '_fx'), 0);
        }
        st.cpu = num(getp('cpu'), 0);
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
         * the pedalboard's - a stomp landing behind an open menu is a change
         * you cannot see. */
        if (menuOpen) return;

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

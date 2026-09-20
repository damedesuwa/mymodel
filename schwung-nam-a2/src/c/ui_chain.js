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

const TYPE_OFF = 0, TYPE_NAM = 1, TYPE_CAB = 2, TYPE_DRIVE = 3;
const TYPE_ABBREV = ['--', 'NAM', 'CAB', 'DRV'];

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
const KNOBS = {};
KNOBS[TYPE_OFF]   = [{ key: 'type', label: 'Type', kind: 'enum', n: 4 }];
KNOBS[TYPE_NAM]   = [{ key: 'type', label: 'Type', kind: 'enum', n: 4 },
                     { key: 'model', label: 'Model', kind: 'list' },
                     { key: 'quality', label: 'Qual', kind: 'enum', n: 3,
                       names: ['Full', 'Slim', 'Lite'] }];
KNOBS[TYPE_CAB]   = [{ key: 'type', label: 'Type', kind: 'enum', n: 4 },
                     { key: 'cab', label: 'Cab', kind: 'list' }];
KNOBS[TYPE_DRIVE] = [{ key: 'type', label: 'Type', kind: 'enum', n: 4 },
                     { key: 'dmode', label: 'Mode', kind: 'enum', n: 3,
                       names: ['OD', 'Dist', 'Fuzz'] },
                     { key: 'drive', label: 'Drive', kind: 'float' },
                     { key: 'tone', label: 'Tone', kind: 'float' },
                     { key: 'level', label: 'Level', kind: 'float' }];

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

/* A cache, because an IPC read is ~2.8 ms and a whole page render is 1.68 -
 * so a read costs more than redrawing the screen. Nothing is read on the
 * draw path: types and states are refreshed on a slow rotation and written
 * through immediately when WE change them, which is the only way they can
 * change while this screen is up. */
const st = {
    type: new Array(NUM_BLOCKS).fill(0),
    on: new Array(NUM_BLOCKS).fill(1),
    cpu: 0,
    blockCpu: new Array(NUM_BLOCKS).fill(0),
    val: {},                    /* "b3_drive" -> number */
    names: { model: [], cab: [] },
    build: '?',
    rot: 0,
};

/* The lists are read ONCE. They change only when a file lands on the card,
 * which cannot happen while this screen is up. */
function loadLists() {
    for (const k of ['model', 'cab']) {
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
    } else {
        st.blockCpu[b] = num(getp('b' + (b + 1) + '_cpu'), st.blockCpu[b]);
    }
}

/* WHAT TO OFFER WHEN THE HOST WILL NOT SAY.
 *
 * `shadow_component_trailing_menus()` builds its rows from the chain config,
 * the user preset store and a blocking `<prefix>:state` read, and on the
 * device it came back with nothing - so the menu drew its Close row and
 * nothing else, which is a menu that cannot remove the module it is a menu
 * for.
 *
 * The ACTIONS are a different binding and a much simpler one: it takes a
 * key and runs a case. The keys are fixed in the host's own switch
 * (runComponentActionFromGrid / moduleMenuEntries), so the rows can be
 * stated here and the door still opens. The host's list is still preferred
 * when it answers - it knows which preset is loaded and whether the module
 * ships help - but it is no longer the only way in. */
const FALLBACK_ROWS = [
    ['Preset...',     'up_load'],
    ['Save As',       'up_save_as'],
    ['Add to List',   'module_lists'],
    ['Module Help',   'module_help'],
    ['Swap Module',   'swap_module'],
    ['Remove Module', 'remove_module'],
];

function openMenu() {
    menuRows = [];
    let why;
    try {
        if (typeof shadow_component_trailing_menus !== 'function') {
            why = 'no binding';
        } else {
            const secs = shadow_component_trailing_menus() || [];
            why = secs.length + ' sections';
            for (const sec of secs) {
                for (const e of (sec.entries || [])) {
                    if (!e || !e.label) continue;
                    menuRows.push({
                        label: e.value ? (e.label + ': ' + e.value) : e.label,
                        action: e.action || null,
                    });
                }
            }
        }
    } catch (err) {
        why = 'threw: ' + ((err && err.message) ? err.message : String(err));
    }

    if (menuRows.length === 0) {
        for (const r of FALLBACK_ROWS) menuRows.push({ label: r[0], action: r[1] });
        why += ' -> fallback';
    }
    /* Says WHICH of the three happened, so "only Close" stops being one
     * report covering an absent binding, an empty answer and a throw. */
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
    print(2, 1, 'Nam A2c  ' + st.build, 1);
    fill_rect(0, 10, 128, 1, 1);
    if (menuCursor < menuTop) menuTop = menuCursor;
    if (menuCursor >= menuTop + MENU_VISIBLE) menuTop = menuCursor - MENU_VISIBLE + 1;
    for (let i = 0; i < MENU_VISIBLE; i++) {
        const idx = menuTop + i;
        if (idx >= menuRows.length) break;
        const y = 14 + i * 10;
        if (idx === menuCursor) fill_rect(0, y - 1, 128, 10, 1);
        const t = menuRows[idx].label;
        print(2, y, t.length > 24 ? t.slice(0, 24) : t, idx === menuCursor ? 0 : 1);
    }
}

function selKey(k) { return 'b' + (sel + 1) + '_' + k; }

function knobList() { return KNOBS[st.type[sel]] || KNOBS[TYPE_OFF]; }

function readSelected() {
    for (const s of knobList()) {
        const k = selKey(s.key);
        const v = getp(k);
        if (v !== null && v !== '') st.val[k] = Number(v);
    }
}

/* ------------------------------------------------------------------ draw */

function drawBoxes() {
    /* Eight boxes across 128 px: 14 wide, 2 apart, starting at 3. */
    const W = 14, GAP = 2, X0 = 3, Y = 12, H = 20;
    for (let b = 0; b < NUM_BLOCKS; b++) {
        const x = X0 + b * (W + GAP);
        const t = st.type[b];
        const live = t !== TYPE_OFF && st.on[b];

        if (b === sel) fill_rect(x - 1, Y - 2, W + 2, H + 4, 1);

        if (live) {
            fill_rect(x, Y, W, H, b === sel ? 0 : 1);
        } else {
            draw_rect(x, Y, W, H, b === sel ? 0 : 1);
        }

        const ink = live ? (b === sel ? 1 : 0) : (b === sel ? 0 : 1);
        const lbl = TYPE_ABBREV[t];
        print(x + ((W - text_width(lbl)) >> 1), Y + 3, lbl, ink);

        /* A bypassed block that HAS a type still says so - it is the
         * difference between "empty" and "switched off", and on a
         * pedalboard that is the whole point of the picture. */
        if (t !== TYPE_OFF && !st.on[b]) print(x + ((W - text_width('B')) >> 1), Y + 11, 'B', ink);
        else if (t !== TYPE_OFF) {
            const pc = String(Math.round(st.blockCpu[b]));
            print(x + ((W - text_width(pc)) >> 1), Y + 11, pc, ink);
        }
    }
}

function drawHeader() {
    const cpu = Math.round(st.cpu);
    const left = 'A2c ' + st.build + ' blk' + (sel + 1);
    print(2, 1, left, 1);
    const right = cpu + '%';
    /* Over budget is the one thing on this screen worth inverting for. */
    if (cpu >= 70) {
        fill_rect(126 - text_width(right) - 2, 0, text_width(right) + 4, 9, 1);
        print(128 - text_width(right) - 2, 1, right, 0);
    } else {
        print(128 - text_width(right) - 2, 1, right, 1);
    }
}

function drawSelected() {
    const list = knobList();
    let x = 2;
    const y = 40;
    for (let i = 0; i < list.length && i < 4; i++) {
        const s = list[i];
        const v = st.val[selKey(s.key)];
        let shown;
        if (s.kind === 'float') shown = (v === undefined) ? '-' : String(Math.round(v * 100));
        else if (s.kind === 'enum') shown = (v === undefined) ? '-'
            : (s.names ? s.names[v] : TYPE_ABBREV[v]) || String(v);
        else shown = (v === undefined) ? '-' : listNameFor(s.key, v);
        print(x, y, s.label, 1);
        /* A model name is longer than a cell, so the cell gets a clipped
         * form and the full one goes on the line below - which is empty
         * whenever nothing on this page is a list. */
        print(x, y + 9, shown.length > 5 ? shown.slice(0, 5) : shown, 1);
        x += 32;
    }
    const full = list.find(s => s.kind === 'list');
    if (full) {
        const v = st.val[selKey(full.key)];
        if (v !== undefined) {
            const nm = listNameFor(full.key, v);
            print(2, 56, nm.length > 24 ? nm.slice(0, 24) : nm, 1);
            return;
        }
    }
    print(2, 56, 'tap=stomp hold=edit click=menu', 1);
}

function draw() {
    if (menuOpen) { drawMenu(); return; }
    clear_screen();
    drawHeader();
    drawBoxes();
    drawSelected();
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
    const s = list[idx];
    const d = decodeDelta(ccValue);
    if (!d) return;
    const k = selKey(s.key);
    let v = st.val[k];

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

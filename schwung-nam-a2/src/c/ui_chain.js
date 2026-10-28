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
/* TWO PAGES OF EIGHT. Move has eight pads a row, so eight is what a hand
 * can reach and what fits across 128 px at a legible column width - the
 * chain is not limited by either, so the board is sixteen wide and the
 * nav arrows move the window over it.
 *
 * PAGE_COLS is the VIEW: pads, LEDs, boxes, the eight columns on screen.
 * NUM_COLS is the BOARD. Every use of the old single NUM_COLS read the
 * same and meant one or the other, which is why they are named apart here
 * instead of being worked out again at each call site. */
const PAGE_COLS = 8;
const NUM_PAGES = 2;
const NUM_COLS = PAGE_COLS * NUM_PAGES;
const NUM_ROWS = 2;
const NUM_BLOCKS = NUM_COLS * NUM_ROWS;      /* slots, not columns */
const ROW_BR = 0;               /* top pad row - the parallel branch */
const ROW_MAIN = 1;             /* bottom pad row - the main chain   */
const PAD_BASE = 68;            /* bottom row, notes 68..75 */
const TOP_BASE = 76;            /* top row,    notes 76..83 */

function slotOf(row, col) { return row * NUM_COLS + col; }
function rowOf(b) { return (b / NUM_COLS) | 0; }
function colOf(b) { return b % NUM_COLS; }

/* WHICH EIGHT OF THE SIXTEEN ARE UNDER YOUR HANDS. The page is a property
 * of the VIEW only - nothing in the signal path, the fork, the merge or
 * the state knows it exists, which is what keeps a block on page 2 an
 * ordinary block rather than a second kind of one. */
let page = 0;
function pageBase() { return page * PAGE_COLS; }
function onPage(b) {
    const c = colOf(b);
    return c >= pageBase() && c < pageBase() + PAGE_COLS;
}
function viewCol(b) { return colOf(b) - pageBase(); }

/* IS THERE ANYTHING ON A PAGE OTHER THAN THIS ONE? What decides whether
 * the window is worth mentioning at all. */
function anyBlockBeyondPage() {
    for (let b = 0; b < NUM_BLOCKS; b++)
        if (!onPage(b) && st.type[b] !== TYPE_OFF) return true;
    return false;
}
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
 * [fork, join] is not. */
function inPath(b) {
    if (rowOf(b) === ROW_MAIN) return true;
    const f = forkCol();
    return f >= 0 && colOf(b) >= f && colOf(b) <= joinCol();
}

/* A BRANCH SLOT PAST THE MERGE IS DEAD, AND THE TWO OFF-PATH REGIONS ARE
 * NOT THE SAME THING.
 *
 * Before the fork a branch slot is empty by construction, and holding it
 * is the ONLY way to put a block there - which moves the fork earlier and
 * puts it in the signal. So it stays holdable, dark but alive.
 *
 * Past the merge there is nothing you can do to the slot that would put
 * it in the signal; the merge is a setting and placing a block does not
 * move it. `inPath` was checked by the DRAW path alone, so those slots
 * drew no box while their pads stayed lit and took both gestures -
 * reported from the device as exactly that. Dead means dead: no light,
 * no stomp, no edit. */
function branchDead(b) {
    return rowOf(b) === ROW_BR && forkCol() >= 0 && colOf(b) > joinCol();
}
const HOLD_MS = 350;

const CC_KNOB_BASE = 71;        /* knobs 1..8 are CC 71..78 */
const CC_JOG_TURN = 14;
const CC_JOG_CLICK = 3;
/* The two arrows under the jog. NOT up and down (55/54): those are Move's
 * OCTAVE PAIR, and claiming half of a pair is how a splittable synth ends
 * up able to shift up and never come back - see the bus-list note in
 * docs/SHADOW_UI.md. Left and right are free, and the shim withholds them
 * from Move only while this screen is up (capabilities.claims_ccs). */
const CC_NAV_LEFT = 62;
const CC_NAV_RIGHT = 63;
const CC_SHIFT = 49;

const TYPE_OFF = 0, TYPE_NAM = 1, TYPE_CAB = 2, TYPE_FX = 3, TYPE_IR = 4;

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
/*
 * A CAPTURE IS NOT A SEPARATE KIND OF THING FROM A PEDAL.
 *
 * `models/` was flat and had one picker called NAM, so a capture of a
 * RAT sat behind a menu entry that said "amp" while the modelled RAT sat
 * in Dist - the same pedal, two places, by file type rather than by what
 * it is. There are far more pedal captures in the wild than amp ones, so
 * the picker nobody would look in was the one holding most of them.
 *
 * A SUBFOLDER OF models/ IS THE CATEGORY, and `nam_cat` says which one
 * this list takes. Loose files stay Amp, so nothing has to be moved. A
 * capture then appears INSIDE Dist, next to the RAT, and knob 2 walks
 * both - which is also one control fewer than before, because the old
 * separate Model picker is gone.
 */
const CATS = [
    { name: 'Off',    type: TYPE_OFF, led: [0, 0] },
    { name: 'Amp',    type: TYPE_NAM, led: [1, 65],       /* red     */
      items: [], folder: 'amps', from: 'nam' },
    /* THE TWO AMP STAGES A CAPTURE CANNOT HAVE, in a category of their
     * own. They were the Amp list's `tail`, sitting past the captures -
     * so walking Amp looking for an amp went through every capture and
     * then two things that are not amps, which was reported as confusing
     * and is. They belong together and they belong next to Amp, which is
     * what the name says: more amp, after the capture.
     *
     * Moving them changes nothing that is saved - a board stores the
     * PEDAL id, and a category only decides which list you walk. */
    { name: 'Amp+',   type: TYPE_FX,  led: [7, 77],       /* v.yellow */
      items: [50, 51] },
    /* The Cab picker is its own knob rather than the shared item list -
     * a cabinet is not something you walk past on the way to a pedal -
     * so `from: 'cab'` only records which folder feeds it. */
    { name: 'Cab',    type: TYPE_CAB, led: [5, 73],       /* brown   */
      folder: 'cabs', from: 'cab' },
    { name: 'OD',     type: TYPE_FX,  led: [8, 79],       /* yellow  */
      items: [0, 1, 2, 3, 4, 40, 41], folder: 'od', from: 'nam' },
    { name: 'Dist',   type: TYPE_FX,  led: [3, 69],       /* orange  */
      items: [5, 6, 7, 42, 8, 9], folder: 'dist', from: 'nam' },
    { name: 'Fuzz',   type: TYPE_FX,  led: [26, 115],     /* magenta */
      items: [10, 11, 43], folder: 'fuzz', from: 'nam' },
    { name: 'Boost',  type: TYPE_FX,  led: [122, 118],    /* white   */
      items: [12, 44], folder: 'boost', from: 'nam' },
    /* PEDAL dynamics. The 1176 and the Limiter moved OUT of here into
     * Outbrd: a 1176 is a rack unit and these four are stomp boxes, and
     * the split is what makes "where do I look for a compressor" have one
     * answer per kind rather than one list holding both. Moving an id
     * between categories changes nothing that is saved - a board stores
     * the PEDAL id, and the category only decides which list you walk to
     * reach it. */
    { name: 'Dyn',    type: TYPE_FX,  led: [15, 93],      /* teal    */
      items: [14, 15, 16, 17] },
    /* OUTBOARD: the rack, and the one category here whose whole point is
     * your own captures. A .nam of a 1176, a Pultec, a console preamp or a
     * tape machine is the same architecture as an amp capture - it is only
     * what was on the other end of the cable that differs - so it needs a
     * folder of its own and not a block type of its own.
     *
     * The two modelled ones are here so the category is not empty on a
     * fresh install, which is a dead end that reads as a broken list. */
    { name: 'Outbrd', type: TYPE_FX,  led: [9, 81],       /* lime    */
      items: [13, 18], folder: 'outboard', from: 'nam' },
    { name: 'Filter', type: TYPE_FX,  led: [11, 85],      /* green   */
      items: [19, 47, 20, 21, 22] },
    { name: 'Mod',    type: TYPE_FX,  led: [16, 95],      /* azure   */
      items: [23, 46, 24, 25, 26, 45, 27, 28, 29] },
    /* DELAY AND REVERB ARE TWO LISTS, because `reverbs/` on the card has
     * to land somewhere with the same name on screen - and eight items
     * behind one knob, half of which are the wrong half, was the walk
     * nobody wanted anyway. */
    { name: 'Delay',  type: TYPE_FX,  led: [22, 107],     /* purple  */
      items: [30, 31, 32, 33] },
    { name: 'Reverb', type: TYPE_FX,  led: [17, 97],      /* blue    */
      items: [34, 48, 35, 49], folder: 'reverbs', from: 'ir' },
    /* Tuning leads the list because it is the one here you reach for on
     * purpose rather than as an effect, and it wants to be in b1. */
    { name: 'Pitch',  type: TYPE_FX,  led: [23, 109],     /* pink    */
      items: [52, 36, 37, 38, 39] },
];

/* AN ITEM IS A PEDAL, A CAPTURE *OR* AN IR, and a band of the integer
 * says which.
 *
 * One tagged integer rather than a pair, because `items` is walked,
 * indexed and compared in a dozen places and every one of them would have
 * had to learn a shape. It was a SIGN for one release and ran out the
 * moment a third kind arrived; bands cost nothing extra and the FX ids,
 * which are written down by hand in CATS, keep their own numbers. */
const ITEM_NAM = 10000, ITEM_IR = 20000;
function namItem(i)      { return ITEM_NAM + i; }
function irItem(i)       { return ITEM_IR + i; }
function itemType(item)  {
    return item >= ITEM_IR  ? TYPE_IR
         : item >= ITEM_NAM ? TYPE_NAM : TYPE_FX;
}
function itemIndex(item) { return item % ITEM_NAM; }

function itemOfBlock(b) {
    if (st.type[b] === TYPE_NAM) return namItem(st.model[b] | 0);
    if (st.type[b] === TYPE_IR)  return irItem(st.ir[b] | 0);
    return st.fx[b] | 0;
}

/* The one place a block's identity is written, so the type and the thing
 * it names can never be set apart from each other. */
const ITEM_KEY = { [TYPE_NAM]: 'model', [TYPE_IR]: 'ir', [TYPE_FX]: 'fx' };
const ITEM_CACHE = { [TYPE_NAM]: 'model', [TYPE_IR]: 'ir', [TYPE_FX]: 'fx' };

function setItem(b, item) {
    const t = itemType(item);
    const v = itemIndex(item);
    st[ITEM_CACHE[t]][b] = v;
    st.val[keyOf(b, ITEM_KEY[t])] = v;
    setp(keyOf(b, ITEM_KEY[t]), v);
    st.type[b] = t;
    st.val[keyOf(b, 'type')] = t;
    setp(keyOf(b, 'type'), t);
}

/* A CATEGORY BY NAME, NEVER BY COUNTING.
 *
 * catOfBlock used to carry the indices as literals - Cab 2, Reverb 11, OD
 * 3 - which is a copy of the CATS order sitting inside the function that
 * reads it. Inserting Amp+ at index 2 moved every one of them one
 * category to the left, and the symptom was a SWALLOWED STEP rather than
 * anything that looked like a wrong number: the walk lands on Cab, this
 * answers 2, the next step computes 2+1 = Cab again, and the `c ===
 * catOfBlock(sel)` guard returns before moving. Four steps up the list
 * went three.
 *
 * The name is what the code means and it does not shift. */
function catNamed(n) {
    for (let c = 0; c < CATS.length; c++) if (CATS[c].name === n) return c;
    return 1;
}

function catOfBlock(b) {
    const t = st.type[b];
    if (t === TYPE_OFF) return 0;
    if (t === TYPE_CAB) return catNamed('Cab');
    const it = itemOfBlock(b);
    for (let c = 1; c < CATS.length; c++)
        if ((CATS[c].items || []).indexOf(it) >= 0) return c;
    /* A capture in no folder we know, or an id from a newer build: Amp
     * for a capture, Reverb for an IR, the first pedal family for
     * anything else. Never a category whose TYPE disagrees with the
     * block's. */
    if (t === TYPE_NAM) return catNamed('Amp');
    if (t === TYPE_IR)  return catNamed('Reverb');
    return catNamed('OD');
}

/* Rebuilt whenever the model list is read, which is once. A category's
 * captures go AFTER its pedals: the modelled ones are the same on every
 * device and the captures are yours, so the list reads the same way on
 * any two boards up to the point where it starts being personal. */
/* Matched on the FOLDER NAME the plugin reports, not on an index into a
 * copy of its table - the two lists would have had to be kept in step by
 * hand, and the failure is a capture that silently lands in the wrong
 * picker. */
function attachNamItems() {
    const dirs = st.names.modelDir || [];
    for (const c of CATS) {
        /* REBUILT FROM THE ORIGINAL LIST, not filtered out of the last
         * one. Filtering kept whatever was already TYPE_FX, so a `tail`
         * entry survived and was appended AGAIN - the Tone Stack grew a
         * duplicate on every init, and since items[0] decides where the
         * category LANDS, `Amp` stopped landing on an amp. The captures
         * never showed it: they are not TYPE_FX, so the filter did
         * remove them. */
        if (!c.base) c.base = (c.items || []).slice();
        c.items = c.base.slice();
        if (c.from === 'nam')
            for (let m = 0; m < dirs.length; m++)
                if (dirs[m] === c.folder) c.items.push(namItem(m));
        /* Yours go AFTER the built-ins: those are the same on every
         * device, so two boards read alike up to where it gets personal. */
        if (c.from === 'ir')
            for (let i = 0; i < (st.names.ir || []).length; i++)
                c.items.push(irItem(i));
        /* AFTER the captures, because landing a category on its tail is
         * landing it on the wrong thing: what you reach `Amp` for is an
         * amp, and the Tone Stack is the knobs that go beside one. With
         * no captures on the card it IS the list, which is the other
         * case this has to be right about. */
        if (c.tail) for (const i of c.tail) c.items.push(i);
    }
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
    if (u === 'cm')   return (v > 0 ? '+' : '') + Math.round(v) + 'cm';
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
        /* The same encoder as a pedal's, because a capture IS one of the
         * things that category holds. The separate Model picker is gone
         * with it. */
        return [CAT_KNOB,
                { key: 'fx', label: 'Capture', kind: 'fxid' },
                { key: 'quality', label: 'Qual', kind: 'enum', n: 3,
                  names: ['Full', 'Slim', 'Lite'] }];
    if (type === TYPE_CAB)
        /* WHERE THE MIC IS, relative to wherever the IR was captured -
         * which is the only zero a capture can offer, so Dist is centred
         * rather than bottomed. Declared as `kn` specs so they get the
         * served table's formatting, bars and stepping without a second
         * set of rules: a cab knob reads +12cm or Ribbn for the same
         * reason a Delay reads 480ms. */
        return [CAT_KNOB, { key: 'cab', label: 'Cab', kind: 'list' },
                { key: 'p1', label: 'Pos', kind: 'spec',
                  kn: { n: 'Pos', u: '%', lo: 0, hi: 100, c: 0 } },
                { key: 'p2', label: 'Dist', kind: 'spec',
                  kn: { n: 'Dist', u: 'cm', lo: -40, hi: 40, c: 0 } },
                { key: 'p3', label: 'Mic', kind: 'spec',
                  kn: { n: 'Mic', u: '', lo: 0, hi: 5, c: 2,
                        o: 'Off|57|421|Ribbn|Cond' } },
                { key: 'p4', label: 'Phase', kind: 'spec',
                  kn: { n: 'Phase', u: '', lo: 0, hi: 2, c: 2, o: 'Norm|Inv' } },
                /* Which POWER STAGE is driving the cab, which an IR
                 * cannot contain - it was measured through one amp. */
                { key: 'p5', label: 'Damp', kind: 'spec',
                  kn: { n: 'Damp', u: '%', lo: 0, hi: 100, c: 0 } }];
    if (type === TYPE_IR)
        /* A ROOM, NOT A SPEAKER: the dry survives, so there is a Mix;
         * and Len is a CPU knob with a time label - ~0.09 us a tap
         * against a 2370 us frame, which is why it tops out at 372 ms
         * rather than at whatever the file holds. */
        return [CAT_KNOB, { key: 'ir', label: 'IR', kind: 'fxid' },
                { key: 'p1', label: 'Mix', kind: 'spec',
                  kn: { n: 'Mix', u: '%', lo: 0, hi: 100, c: 0 } },
                { key: 'p2', label: 'Len', kind: 'spec',
                  kn: { n: 'Len', u: '', lo: 0, hi: 4, c: 2,
                        o: '46ms|93ms|186ms|372ms' } },
                { key: 'p3', label: 'Pre', kind: 'spec',
                  kn: { n: 'Pre', u: 'ms', lo: 0, hi: 104, c: 0 } },
                { key: 'p4', label: 'LoCut', kind: 'spec',
                  kn: { n: 'LoCut', u: 'Hz', lo: 20, hi: 400, c: 0 } }];
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
/* THE TUNER IS ITS OWN SCREEN, not a widget on the board.
 *
 * A needle wants the width and a note name wants the height, and neither
 * fits in a 14 px column beside seven others. It is also the one screen
 * here where the pedalboard is not what you are looking at, which is what
 * lets it have the whole read budget - see tickBody. */
let tunerOpen = false;
let tunerVal = null;      /* the last parsed report, or null */
let tunerAsk = 0;         /* tick counter: the detector only moves at 21 Hz */
let menuRows = [];
let menuCursor = 0;
let menuTop = 0;
const MENU_VISIBLE = 5;

/* Two jog detents per column. One is a brush of the wrist re-routing the
 * board; three is a control nobody believes is doing anything. */
const JOG_PER_STEP = 2;
let jogAcc = 0;
let padDownAt = [];             /* when each pad went down, or 0 */
let padHandled = [];            /* hold already fired, so the release is not a tap */
let topDownAt = [];             /* the same two, for the TOP block row */
let topHandled = [];
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
    model: new Array(NUM_BLOCKS).fill(0),
    ir: new Array(NUM_BLOCKS).fill(0),
    cpu: 0,
    peak: 0,                    /* out level, %% of full scale - see drawMeter */
    blockCpu: new Array(NUM_BLOCKS).fill(0),
    val: {},                    /* "b3_drive" -> number */
    names: { model: [], cab: [], ir: [], fx: [], modelDir: [] },
    specs: [],                  /* fx_specs, read once - see specKnob */
    build: '?',
    rot: 0,
    /* The plugin's scan counter as of the last time the lists were read.
     * null = never asked; see rotateRead. */
    listGen: null,
};

/* READ ON ENTRY, AND AGAIN WHEN `list_gen` MOVES.
 *
 * This used to say the lists change only when a file lands on the card,
 * which cannot happen while this screen is up - and that was true until
 * the card could be written from the web manager with the module playing.
 * It is still five IPC reads (~14 ms), so it is not on the rotation: the
 * plugin bumps one counter when it publishes a new scan, the rotation
 * reads THAT, and this runs only on a change. */
function loadLists() {
    try {
        const j = getp('model_dirs');
        if (j !== null && j !== '') {
            const a = JSON.parse(j);
            if (Array.isArray(a)) st.names.modelDir = a;
        }
    } catch (e) { }
    for (const k of ['model', 'cab', 'ir', 'fx']) {
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

/* What a tagged item is called: a capture by its file name, a pedal by
 * the served table's. */
function itemName(item) {
    const t = itemType(item);
    if (t === TYPE_NAM) return listNameFor('model', itemIndex(item));
    if (t === TYPE_IR)  return listNameFor('ir', itemIndex(item));
    return st.names.fx[item] || String(item);
}

function listNameFor(key, idx) {
    const arr = (key === 'model') ? st.names.model
              : (key === 'ir')    ? st.names.ir : st.names.cab;
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
        /* ONCE PER ROTATION, on the stop that is already paying for a read
         * of its own. A new .nam does not need noticing inside 100 ms, and
         * a read per tick for something that moves when a FILE is uploaded
         * would be the whole read budget spent on waiting. */
        const g = getp('list_gen');
        if (g !== null && g !== '' && g !== st.listGen) {
            /* FIRST ANSWER IS NOT A CHANGE. st.listGen starts null, so
             * without this the module re-reads four lists on the first
             * rotation after every entry - 14 ms for an answer it already
             * has from init(). */
            if (st.listGen !== null) {
                loadLists();
                attachNamItems();
            }
            st.listGen = g;
        }
        return;
    }
    const b = i >> 1;
    if ((i & 1) === 0) {
        st.type[b] = num(getp(keyOf(b, 'type')), st.type[b]);
        st.on[b] = num(getp(keyOf(b, 'on')), st.on[b] ? 0 : 1) === 0 ? 1 : 0;
        st.fx[b] = num(getp(keyOf(b, 'fx')), st.fx[b]);
    } else {
        st.blockCpu[b] = num(getp(keyOf(b, 'cpu')), st.blockCpu[b]);
        /* On the ODD stop, which reads one key, rather than beside the
         * three on the even one - a fourth read there is 11 ms of IPC in
         * a single tick. */
        st.model[b] = num(getp(keyOf(b, 'model')), st.model[b]);
        st.ir[b] = num(getp(keyOf(b, 'ir')), st.ir[b]);
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
const TUNER_ROW = '\u0000tuner'; /* likewise - it is our own screen */

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
    /* FIRST, because it is the thing you want before you want anything
     * else on this screen, and because scrolling to it with a guitar in
     * your hands is the one interaction here that has to be short. */
    menuRows.unshift({ label: 'Tuner', action: TUNER_ROW });

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
    if (row.action === TUNER_ROW) {
        tunerOpen = true;
        tunerVal = null;
        tunerAsk = 0;
        lastPaint = 0;
        return true;
    }
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
/* A NAME TOO LONG TO FIT SCROLLS, rather than being cut.
 *
 * A capture is named by its file and a file name is as long as whoever
 * made it felt like: "Recto Modern - G8 Tube Spongy - DI" is 34
 * characters against a footer that fits about 21. Cutting it hides the
 * END, which is exactly the half that tells two captures of the same amp
 * apart - "...- DI" and "...- FAT CAB" clip to the same five words.
 *
 * PING-PONG WITH A PAUSE AT EACH END, not a wrap. On 128 pixels a
 * wrapping marquee never holds the start still long enough to read it,
 * and the start is what you look at first. The pause at the left is
 * longer than the one at the right for the same reason.
 *
 * STATELESS, driven off the clock. A per-call-site offset would need a
 * key, and the two places this is used (the header and the footer) scroll
 * different strings at the same time - so a shared offset would make a
 * short name on one line drag a long one on the other. Deriving it from
 * the time and the string's own travel gives each line its own cycle for
 * free. */
const MQ_HOLD_L = 1500, MQ_HOLD_R = 900, MQ_STEP = 160;
function marqueeTo(t, w, nowMs) {
    const full = String(t === undefined || t === null ? '' : t);
    if (text_width(full) <= w) return full;
    /* The least offset at which the REMAINDER fits, measured rather than
     * counted in characters: the font is proportional, so "WWW" and
     * "iii" are not the same number of pixels. */
    let span = 0;
    while (span < full.length - 1 && text_width(full.slice(span)) > w) span++;
    if (span <= 0) return clipTo(full, w);
    const travel = span * MQ_STEP;
    const cycle = MQ_HOLD_L + travel + MQ_HOLD_R + travel;
    let at = ((nowMs % cycle) + cycle) % cycle;
    let off;
    if (at < MQ_HOLD_L) off = 0;
    else if ((at -= MQ_HOLD_L) < travel) off = Math.floor(at / MQ_STEP);
    else if ((at -= travel) < MQ_HOLD_R) off = span;
    else off = span - Math.floor((at - MQ_HOLD_R) / MQ_STEP);
    if (off < 0) off = 0;
    if (off > span) off = span;
    return clipTo(full.slice(off), w);
}

function ptext(x, y, t, ink, w) { print(x, y, clipTo(t, w), ink); }
/* Same as ptext, for a line whose whole job is carrying a NAME. */
function pmarq(x, y, t, ink, w) { print(x, y, marqueeTo(t, w, Date.now()), ink); }
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
    if (t === TYPE_IR)  return 'IR';
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
    /* THE POSITION IS PINNED AND ONLY THE NAME TRAVELS. `B5` is two
     * characters and is what tells you which box the knobs are on, so
     * scrolling the whole line would push the one part you need off the
     * left edge to make room for the part that is already clipped. */
    const pre = blockName(sel) + side + ' ';
    const pw0 = text_width(pre);
    ptext(1, 1, pre, 1, MET_X - 3);
    pmarq(1 + pw0, 1, what || '--', 1, MET_X - 3 - pw0);
    fill_rect(0, 10, 128, 1, 1);
}

/* Where a block's box sits, and where its wire runs through the column. */
/* IS ANY COLUMN OF THIS PAGE INSIDE THE PARALLEL SECTION?
 *
 * A mono block's height depends on whether the board is split, because an
 * unsplit board gives it the full 16 px and a split one has to leave room
 * for two 9 px lanes. Asking the BOARD would draw page 2 short for a fork
 * and join that both happened on page 1 - correct arithmetic, wrong
 * question. */
function pageForked() {
    const split = forkCol();
    if (split < 0) return false;
    const join = joinCol();
    return join >= pageBase() && split < pageBase() + PAGE_COLS;
}

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
    const h = forked ? LANE_H : (pageForked() ? BAND_H : GRID_H);
    return { x: colX(viewCol(b)), y: y, h: h, wire: y + (h >> 1),
             forked: forked };
}

function drawBoxes() {
    const split = forkCol(), join = joinCol();
    const bandH = pageForked() ? BAND_H : GRID_H;
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
    /* THE PAGE IS A WINDOW, AND THE FORK CAN BE OUTSIDE IT.
     *
     * In view columns the fork and the join can land left of 0 or right of
     * PAGE_COLS-1, which means the page is ALREADY parted when it begins or
     * STILL parted when it ends. Those edges carry no vertical - there is
     * nothing happening there, the rails simply continue off-screen - and
     * drawing one would claim a fork that is on the other page.
     *
     * Clamping the x and keeping the vertical is the whole difference
     * between a window and a second board. */
    const vs = split - pageBase();
    const vj = join - pageBase();
    const forkHere = vs >= 0 && vs < PAGE_COLS;
    const joinHere = vj >= 0 && vj < PAGE_COLS;
    const fx = forkHere ? colX(vs) - 1 : IO_W;               /* left edge  */
    const jx = joinHere ? colX(vj) + COL_W : 128 - IO_W - 1; /* right edge */
    if (!pageForked()) {
        fill_rect(IO_W, midWire, 128 - IO_W * 2, 1, 1);
    } else {
        if (forkHere) {
            fill_rect(IO_W, midWire, fx - IO_W + 1, 1, 1);
            fill_rect(fx, laneWire[0], 1, laneWire[1] - laneWire[0] + 1, 1);
        }
        if (joinHere) {
            fill_rect(jx, laneWire[0], 1, laneWire[1] - laneWire[0] + 1, 1);
            fill_rect(jx, midWire, 128 - IO_W - jx, 1, 1);
        }
        for (let r = 0; r < 2; r++)
            fill_rect(fx, laneWire[r], jx - fx + 1, 1, 1);
    }

    for (let b = 0; b < NUM_BLOCKS; b++) {
        /* A SLOT OFF THE PATH IS NOT DRAWN AT ALL, not drawn faintly.
         * The branch row before the fork is empty by construction - it is
         * what DEFINES the fork - so there is no wire there and a box
         * would be sitting on nothing. */
        if (!onPage(b)) continue;
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
        else if (spec.kind === 'fxid')  shown = itemName(itemOfBlock(sel));
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
    /* WHERE THE LANES MEET, ALWAYS ON SCREEN WHILE THERE ARE TWO.
     *
     * The jog sets it and the flash says so for a second, which answers
     * "did that take" and not "where is it now". A forked board has a
     * number worth carrying, and it is the only thing about the routing
     * that is not already a picture. Unforked there is nothing to say,
     * so the line is the full width again. */
    let right = 0;
    if (forkCol() >= 0) {
        const m = num(st.val['merge'], 0);
        const txt = 'M:' + (m < 1 ? 'Out' : String(m));
        right = text_width(txt) + 3;
        pright(126, FOOT_Y, txt, 1, right);
    }
    /* WHICH EIGHT OF THE SIXTEEN YOU ARE LOOKING AT. Left of the merge
     * number, because that one is about the board and this is about the
     * screen, and the merge was there first.
     *
     * Only drawn when the OTHER page has something on it. A board that
     * fits on one page has no second page to be on, and a permanent "1/2"
     * would be a line of chrome saying nothing - the same reason the merge
     * number is absent while the board is mono. */
    if (page > 0 || anyBlockBeyondPage()) {
        const ptxt = 'P' + (page + 1);
        const pw = text_width(ptxt) + 3;
        pright(126 - right, FOOT_Y, ptxt, 1, pw);
        right += pw;
    }
    const w = 126 - right;

    /* A CAPTURE GETS THE SAME LINE A PEDAL DOES, because it is in the
     * same list - the footer's whole job is saying where in that list you
     * are, and a capture that fell through to the generic branch would
     * lose the one place its full name fits. */
    if (st.type[sel] === TYPE_FX || st.type[sel] === TYPE_NAM ||
        st.type[sel] === TYPE_IR) {
        const it = itemOfBlock(sel);
        const c = catOfBlock(sel);
        const items = CATS[c].items || [it];
        const at = items.indexOf(it);
        /* THE COUNTER IS CHROME AND STAYS STILL; the name scrolls.
         * `Dist 3/26` is where you are in the list and is read at a
         * glance - carrying it along on a marquee would make the one
         * stable fact on the line the moving one, and it is short enough
         * to always fit. So it is right-aligned and the name gets
         * whatever is left. */
        const cnt = CATS[c].name + ' ' + (at + 1) + '/' + items.length;
        const cw = text_width(cnt) + 4;
        pright(126 - right, FOOT_Y, cnt, 1, cw);
        pmarq(1, FOOT_Y, itemName(it), 1, w - cw);
        return;
    }
    const full = list.find(k => k.kind === 'list');
    if (full) {
        const v = st.val[fullKey(full)];
        if (v !== undefined) {
            pmarq(1, FOOT_Y, listNameFor(full.key, v), 1, w);
            return;
        }
    }
    ptext(1, FOOT_Y, 'hold:edit  jog:merge', 1, w);
}

/* NO STRING LABEL, and that is the whole of being chromatic.
 *
 * This read the nearest of the six standard open strings and printed it
 * top right - which is a guitar-specific tuner's feature, not a chromatic
 * one, and it is WRONG the moment the guitar is not in standard tuning.
 * In drop C the low string is C2; the nearest standard string is E2, 400
 * cents away, so the label either says nothing or - on the tunings where
 * it lands inside a semitone of some other string - says the wrong number
 * with confidence. A tuner that has to be told the tuning is the thing a
 * chromatic tuner exists not to be.
 *
 * So the answer is the NOTE and the cents, which is what every chromatic
 * pedal shows and what works for drop D, drop C, open G, DADGAD, a
 * 7-string, an 8-string and a bass without any of them being a case. The
 * octave is in the name (E2 vs E4), which is what tells you whether you
 * are on the right string anyway.
 */
const TUN_BAR_Y = 28, TUN_BAR_H = 13;
const TUN_BAR_X = 4, TUN_BAR_W = 120;      /* centre lands on 64 */
const TUN_RANGE = 50;                      /* cents, each way */
/* How close counts as in tune. Three cents is below what anyone hears on a
 * guitar and inside what the detector is good for (the two-stage search
 * measures to under 3 cents across the range), so it is honest both ways. */
const TUN_IN_TUNE = 3;

function drawTuner() {
    clear_screen();
    const v = tunerVal;
    const hz = (v && Number.isFinite(v.hz)) ? v.hz : 0;
    const live = hz > 0;
    const cents = live && Number.isFinite(v.cents) ? v.cents : 0;
    const inTune = live && Math.abs(cents) <= TUN_IN_TUNE;

    ptext(1, 1, 'Tuner', 1, 60);
    /* Says what kind of tuner it is, which is the one thing a player needs
     * to know before trusting it in a tuning that is not standard. */
    pright(126, 1, 'chromatic', 1, 64);
    fill_rect(0, 10, 128, 1, 1);

    /* THE NOTE, and the in-tune answer is the BOX around it rather than a
     * word. A needle says how far and a word would say the same thing
     * again; the fill is readable from further away than either, which is
     * the distance a guitar puts between you and the screen. */
    const note = live ? (v.note + String(v.oct)) : '--';
    const nw = text_width(note) + 6;
    const nx = 64 - (nw >> 1);
    if (inTune) {
        fill_rect(nx, 13, nw, 11, 1);
        pcenter(nx, nw, 15, note, 0);
    } else {
        pcenter(nx, nw, 15, note, 1);
    }
    if (live) {
        pright(126, 15, (cents > 0 ? '+' : '') + cents.toFixed(0) + 'c', 1, 40);
    }

    /* The scale. Ticks every 10 cents, a taller one at the centre, so the
     * needle has something to be read against - a bare moving block says
     * how far but not how far out of what. */
    fill_rect(TUN_BAR_X, TUN_BAR_Y + TUN_BAR_H, TUN_BAR_W + 1, 1, 1);
    for (let c = -TUN_RANGE; c <= TUN_RANGE; c += 10) {
        const x = 64 + Math.round(c * (TUN_BAR_W / 2) / TUN_RANGE);
        const h = (c === 0) ? TUN_BAR_H : 3;
        fill_rect(x, TUN_BAR_Y + TUN_BAR_H - h, 1, h, 1);
    }

    if (live) {
        /* CLAMPED, NOT WRAPPED. Past the ends the needle sits ON the end:
         * the note name has already changed by then, so the bar's job is
         * only to say "a long way, that direction". */
        let c = cents;
        if (c < -TUN_RANGE) c = -TUN_RANGE;
        if (c > TUN_RANGE) c = TUN_RANGE;
        const x = 64 + Math.round(c * (TUN_BAR_W / 2) / TUN_RANGE);
        fill_rect(x - 1, TUN_BAR_Y, 3, TUN_BAR_H, 1);
        pcenter(0, 128, 44, hz.toFixed(2) + ' Hz', 1);
    } else {
        /* WHY there is no reading, which is a different sentence from a
         * blank screen. The plugin answers with hz 0 and a level, so the
         * two silences can be told apart: nothing played, or something
         * played too quietly to measure. */
        const lvl = (v && Number.isFinite(v.lvl)) ? v.lvl : 0;
        pcenter(0, 128, 44, lvl > 0.0005 ? 'too quiet' : 'play a string', 1);
    }

    ptext(1, FOOT_Y, 'jog: close', 1, 126);
}

/* ------------------------------------------------------- the mic diagram */

/*
 * WHERE THE MIC IS, AS A PICTURE.
 *
 * Five numbers on five 31-pixel cells, and from the device: "cab 에
 * distance 랑 옵션들이 너무 헷갈려". They are confusing for a reason the
 * cells cannot fix - every one of them is a POSITION, and a position
 * printed as a percentage has no zero you can see. Worse, `Dist` is
 * SIGNED against a reference nothing on screen mentions: -8cm is eight
 * centimetres nearer than the place the IR was recorded from, and
 * nobody can be expected to read that off the word "Dist".
 *
 * So: one peek, raised by touching any of the cab's five knobs, showing
 * the cone from the side with the mic where the knobs put it, the
 * capture point drawn as the zero it is, and the five values spelled out
 * beside it in full rather than clipped to five characters.
 *
 * NO ANGLE, which is what was asked about and is the one thing that
 * cannot be drawn honestly. An off-axis angle needs a distance from the
 * CONE, and `Dist` is measured from the capture point - a distance the
 * IR does not record and the file does not carry. Every angle this could
 * print would be computed from a number we invented. What IS known is
 * drawn: how far across the cone (Pos), how far from the capture point
 * (Dist), and the line between the two, which is the angle without a
 * number attached to it.
 */
const MIC_PEEK_MS = 1600;
const MIC_CAP_X = 44;          /* where the IR was mic'd = the drawn zero */
const MIC_SPAN  = 22;          /* +-40 cm maps onto +-this many pixels    */
const MIC_AXIS_Y = 30;
let micPeekUntil = 0;

/* A ONE-PIXEL LINE, because the host draws rectangles and text and
 * nothing else. Bresenham, clamped to the panel - a diagram that can
 * compute a coordinate off-screen has to be the thing that refuses it,
 * since fill_rect will happily take one. */
function pline(x0, y0, x1, y1, ink) {
    let x = x0 | 0, y = y0 | 0;
    const xe = x1 | 0, ye = y1 | 0;
    const dx = Math.abs(xe - x), dy = -Math.abs(ye - y);
    const sx = x < xe ? 1 : -1, sy = y < ye ? 1 : -1;
    let err = dx + dy;
    for (let guard = 0; guard < 400; guard++) {
        if (x >= 0 && x < 128 && y >= 0 && y < 64) fill_rect(x, y, 1, 1, ink);
        if (x === xe && y === ye) break;
        const e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}
/* Dotted, for the things that are REFERENCES rather than objects: the
 * axis and the capture point are where the mic is measured from, not
 * parts of the cabinet, and a solid line would read as more hardware. */
function pdots(x0, y0, x1, y1, ink) {
    if (y0 === y1) {
        for (let x = x0; x <= x1; x += 2)
            if (x >= 0 && x < 128) fill_rect(x, y0, 1, 1, ink);
    } else {
        for (let y = y0; y <= y1; y += 2)
            if (y >= 0 && y < 64) fill_rect(x0, y, 1, 1, ink);
    }
}

function micSpecOf(key) {
    for (const k of knobsFor(TYPE_CAB, 0)) if (k.key === key) return k;
    return null;
}
/* The number the CELL would print, by the same path the cell prints it.
 * Formatting it a second time here is how the diagram and the cells come
 * to disagree about the same knob. */
function micShown(key) {
    const sp = micSpecOf(key);
    if (!sp) return '-';
    const v = st.val[selKey(key)];
    return (v === undefined || !Number.isFinite(v)) ? '-' : specText(sp.kn, v);
}
/* THROUGH selKey, not a spelled-out `b<n>_`. The same cab on the top
 * rail is `t3_p2`, and a hand-built key would have made this diagram
 * read the bottom row's cab while you turned the top row's. */
function micRaw(key, dflt) {
    const v = st.val[selKey(key)];
    return (v === undefined || !Number.isFinite(v)) ? dflt : v;
}

function drawMicView() {
    clear_screen();
    ptext(1, 1, 'Mic', 1, 24);
    /* Which cabinet this is the mic on - the diagram is about placement
     * and placement on WHAT is the other half of the sentence. */
    pmarq(26, 1, listNameFor('cab', micRaw('cab', 0)) || '-', 1, 100);
    fill_rect(0, 10, 128, 1, 1);

    const pos  = Math.max(0, Math.min(1, micRaw('p1', 0)));
    const dist = Math.max(0, Math.min(1, micRaw('p2', 0.5)));
    const cm   = (dist * 2 - 1) * 40;

    /* ---- the cabinet, side on. Baffle, cone, dust cap. ---- */
    fill_rect(8, 16, 2, 29, 1);                 /* the baffle            */
    pline(10, 16, 19, MIC_AXIS_Y, 1);           /* cone, upper half      */
    pline(10, 44, 19, MIC_AXIS_Y, 1);           /* cone, lower half      */
    fill_rect(18, MIC_AXIS_Y - 1, 3, 3, 1);     /* the dust cap          */

    /* ---- the two references ---- */
    pdots(22, MIC_AXIS_Y, 70, MIC_AXIS_Y, 1);   /* on-axis               */
    pdots(MIC_CAP_X, 18, MIC_CAP_X, 42, 1);     /* where it was captured */

    /* ---- the mic ---- */
    /* POS IS RADIAL and the cone is symmetric, so it is drawn going one
     * way only: 0 at the cap, 1 at the surround. Drawing it both ways
     * would need a sign the knob does not have. */
    const mx = MIC_CAP_X + Math.round((cm / 40) * MIC_SPAN);
    const my = MIC_AXIS_Y - Math.round(pos * 11);
    pline(20, MIC_AXIS_Y, mx, my, 1);           /* cap -> mic            */
    draw_rect(mx - 2, my - 2, 5, 5, 1);
    fill_rect(mx + 3, my, 3, 1, 1);             /* the body, pointing in */

    /* THE TWO ZEROS, NAMED. Without them the dotted lines are decoration
     * and the sign on Dist still means nothing.
     *
     * Two separate draws rather than one sentence, so each is clipped on
     * its own: a single line long enough to say both is long enough to
     * lose the end of itself, which is the defect this screen exists to
     * fix. And `off-axis` because that is what Pos is - the standard
     * word for leaving the dust cap, and the one the diagram's vertical
     * offset is drawing. */
    ptext(1, FOOT_Y, '0=capture', 1, 60);
    pright(126, FOOT_Y, '^=off-axis', 1, 62);

    /* ---- the five values, in full ---- */
    const rows = [['Pos', 'p1'], ['Dst', 'p2'], ['Mic', 'p3'],
                  ['Phs', 'p4'], ['Dmp', 'p5']];
    for (let i = 0; i < rows.length; i++) {
        const y = 12 + i * 9;
        ptext(76, y, rows[i][0], 1, 18);
        pright(126, y, micShown(rows[i][1]), 1, 32);
    }
}

function draw() {
    if (tunerOpen) { drawTuner(); return; }
    if (menuOpen) { drawMenu(); return; }
    /* AFTER the menu and the tuner, which are MODES - this is a peek, and
     * a peek must never be the thing standing between you and a screen
     * you deliberately opened. */
    if (micPeekUntil && Date.now() < micPeekUntil) { drawMicView(); return; }
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
        const i = items.indexOf(itemOfBlock(sel));
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
        /* THE PADS ARE THE PAGE. Sixteen columns over eight pads means a
         * block off the window has no pad to light, and colOf() would
         * address the row above - note 76+8 is 84, which is Move's THIRD
         * pad row. */
        if (!onPage(b)) continue;
        const note = (rowOf(b) === ROW_BR ? TOP_BASE : PAD_BASE) + viewCol(b);
        /* Selection BLINKS rather than taking a colour of its own, because
         * "which one am I editing" and "what is this block" are different
         * questions and one colour cannot answer both - and now that the
         * colour carries the category, spending it on selection would be
         * spending the more useful of the two. */
        let c;
        if (b === sel && (Date.now() % 700) < 350) {
            c = White;
        } else if (rowOf(b) === ROW_BR && pathFork >= 0 &&
                   (colOf(b) < pathFork || colOf(b) > joinCol())) {
            /* Off the path, either side. Before the fork it is still
             * HOLDable - that is how you move the fork earlier. Past the
             * merge it is inert; see branchDead. */
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
/* HOW FAR THE BRANCH RUNS. The jog sets it; see the jog branch in
 * onMidiMessageInternal for why it is the jog and not a modifier.
 *
 * Shift + a pad did this too for one release and is GONE. It could not be
 * shown to arrive - the report was "안되는데" with nothing in the log
 * either way - and once the jog worked it was strictly downside: a stray
 * Shift under a pad press would silently re-route the board instead of
 * stomping the block. One gesture, one meaning.
 */
function setMergeCol(c) {
    const cur = num(st.val['merge'], 0);
    const v = c + 1;                  /* -1 is Out; the jog is a position */
    if (v === cur) return;
    st.val['merge'] = v;
    setp('merge', v);
    flash(v === 0 ? 'Merge: at Out' : ('Merge after ' + v));
    /* MOVING THE MERGE CAN KILL THE SLOT YOU ARE EDITING, and an editor
     * pointing at a dead block is eight encoders that change nothing you
     * can hear. Step down to the main row in the same column, which is
     * always in the signal. */
    if (branchDead(sel)) {
        sel = slotOf(ROW_MAIN, colOf(sel));
        setp('sel_block', sel);
        readSelected();
    }
}

function stomp(b) {
    if (branchDead(b)) { flash(blockName(b) + ' PAST MERGE'); return; }
    const nowOn = st.on[b];
    st.on[b] = nowOn ? 0 : 1;
    flash(blockName(b) + (st.on[b] ? ' ON' : ' BYPASS'));
    /* Written through rather than waiting for the rotation to notice: the
     * pad has to answer under the finger. */
    setp(keyOf(b, 'on'), st.on[b] ? 0 : 1);
}

/* MOVE THE WINDOW. The arrows are the only thing that writes `page`.
 *
 * The SELECTION COMES WITH IT. Leaving it behind would leave the knobs
 * editing a block that is not on screen - eight cells describing something
 * you cannot see, and the blinking pad on neither page - which is the same
 * class of fault as a lane nothing writes: correct state, no way to read
 * it. */
function setPage(p) {
    const np = p < 0 ? 0 : (p > NUM_PAGES - 1 ? NUM_PAGES - 1 : p);
    if (np === page) return;
    page = np;
    if (!onPage(sel)) {
        sel = slotOf(rowOf(sel), pageBase());
        /* A top-row slot off the path is not a place to land: it draws no
         * box, lights no pad and refuses the knobs. Step down to the main
         * row, which is always in circuit. */
        if (!inPath(sel)) sel = slotOf(ROW_MAIN, colOf(sel));
        setp('sel_block', sel);
        readSelected();
    }
    resetKnobAcc();
    invalidateLedCache();
    flash('PAGE ' + (page + 1) + '/' + NUM_PAGES);
}

function select(b) {
    if (branchDead(b)) { flash(blockName(b) + ' PAST MERGE'); return; }
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
    /* RAISED BY A TOUCH, not by a turn, which is why it is above the
     * detent gate. Move sends a capacitive touch note before the first
     * detent arrives, so arming here means the diagram is already up by
     * the time the value starts moving - and a touch that never turns
     * into a turn still answers "which knob is this and where is it",
     * which is the question. */
    if (st.type[sel] === TYPE_CAB && s.key && s.key[0] === 'p')
        micPeekUntil = Date.now() + MIC_PEEK_MS;
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
        /* THE ITEM DECIDES THE TYPE, not the category: an Amp category
         * holding no captures has nothing to land on, and a Dist one
         * lands on a modelled pedal or on your own capture depending on
         * which is first. `setItem` writes the pair together so they
         * cannot be set apart - the write order used to matter here
         * because the type write is what makes the plugin reach for a
         * default, and a block on its default for a frame is a block
         * that makes a different noise for a frame. */
        const first = (cat.items && cat.items.length) ? cat.items[0] : undefined;
        if (first !== undefined) {
            setItem(sel, first);
        } else {
            st.type[sel] = cat.type;
            st.val[selKey('type')] = cat.type;
            setp(selKey('type'), cat.type);
        }
        if (st.on[sel] === 0) flash(blockName(sel) + ' ADDED - BYPASSED');
        readSelected();
        return;
    }

    if (s.kind === 'fxid') {
        const step = knobSteps(idx, d);
        if (!step) return;
        const items = CATS[catOfBlock(sel)].items || [];
        const cur = itemOfBlock(sel);
        let i = items.indexOf(cur) + step;
        if (i < 0) i = 0;
        if (i > items.length - 1) i = items.length - 1;
        const want = items[i];
        if (want === undefined || want === cur) return;
        setItem(sel, want);
        /* The pedal changed, so its knobs are a different set - and a
         * capture has a different set again. What they point at has to be
         * re-read rather than carried over. */
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
        /* EIGHT, because these are indexed by PAD, not by block. */
        for (let i = 0; i < PAGE_COLS; i++) {
            padDownAt[i] = 0; padHandled[i] = false;
            topDownAt[i] = 0; topHandled[i] = false;
        }
        page = 0;
        /* The shim replays Move's own LED state on the way in, so what the
         * cache believes is stale. Re-emit everything once. */
        invalidateLedCache();
        for (let i = 0; i < NUM_KNOBS; i++) ringCache[i] = -1;
        resetKnobAcc();
        loadLists();
        attachNamItems();
        /* Stamped here, with the lists it goes with, or the first rotation
         * reads them all again for nothing. */
        st.listGen = getp('list_gen');
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
            st.model[b] = num(getp(keyOf(b, 'model')), 0);
            st.ir[b] = num(getp(keyOf(b, 'ir')), 0);
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

        /* AND THE ARROWS ARE CLAIMED THE SAME WAY, because the STATIC
         * declaration cannot reach us.
         *
         * capabilities.claims_ccs is read by the host's reconcileCcClaim(),
         * which gets the module id from hierarchyActiveModuleIdRaw() -
         * and that opens `if (hierEditorSlot < 0 || !hierEditorComponent)
         * return ""`. Those two are set in ONE place,
         * resetHierarchyEditorFor(), which runs only when a component
         * HAS a ui_hierarchy. This module deliberately publishes none,
         * because that is the condition for ui_chain.js loading at all
         * (the two are exclusive and the hierarchy wins). So the id comes
         * back "", moduleClaimedCcs("") answers "" immediately, and
         * `claim === ccClaimed` makes the host return without ever
         * calling host_claim_ccs. The static claim is unreachable for any
         * module that draws its own screen - not misconfigured, but
         * structurally out of reach, and silent about it.
         *
         * host_claim_ccs() is the runtime complement and is a plain JS
         * global in the same QuickJS context this file loads into, so the
         * module can state its own claim. Same contract as pad_block:
         * idempotent against the SHM (the binding memcmps before it
         * writes and logs only on a transition), so a per-frame restate
         * costs a 16-byte compare - and it must NOT be memoised, because
         * the shim clears claim_cc_bits itself when the shadow display
         * closes and never tells JS.
         *
         * The static declaration stays in module.json: it is the honest
         * statement of intent and it starts working the day the host asks
         * the component rather than the hierarchy editor. */
        if (typeof host_claim_ccs === 'function')
            host_claim_ccs([CC_NAV_LEFT, CC_NAV_RIGHT]);

        /* THE TUNER TAKES THE WHOLE READ BUDGET, and the rotation stops.
         *
         * An IPC read is ~2.8 ms. The rotation exists to keep the BOARD's
         * picture current, and the board is not on screen - so spending
         * those reads on it would be paying for a picture nobody is
         * looking at and then not being able to afford the one they are.
         *
         * Asked every OTHER tick, because the detector only publishes at
         * 21 Hz (one analysis every 16 blocks) and reading faster than the
         * producer buys nothing but IPC. */
        if (tunerOpen) {
            /* ITS OWN CLOCK READ. `now` below is declared after this point
             * with `const`, so reading it here is a temporal dead zone -
             * which throws, and a throw inside tick() is swallowed by the
             * host, so the screen simply never painted while every other
             * assertion about this branch passed. */
            const tnow = Date.now();
            if ((tunerAsk++ & 1) === 0) {
                const j = getp('tuner');
                /* null is the read not completing, which is news about the
                 * CHANNEL - keep the last reading rather than blanking the
                 * screen on one slow frame. "" would be served-and-empty,
                 * which this key never answers: no note is hz 0. */
                if (j !== null && j !== '') {
                    try { tunerVal = JSON.parse(j); } catch (e) { }
                }
            }
            if (tnow - lastPaint >= 33) { lastPaint = tnow; draw(); }
            return;
        }

        rotateRead();

        /* A hold fires while the finger is still down - waiting for the
         * release would make selecting feel like a slow tap. */
        const now = Date.now();
        for (let b = 0; b < PAGE_COLS; b++) {
            if (padDownAt[b] && !padHandled[b] && now - padDownAt[b] >= HOLD_MS) {
                padHandled[b] = true;
                select(slotOf(ROW_MAIN, pageBase() + b));
            }
            if (topDownAt[b] && !topHandled[b] && now - topDownAt[b] >= HOLD_MS) {
                topHandled[b] = true;
                select(slotOf(ROW_BR, pageBase() + b));
            }
        }

        paintLeds();
        if (now - lastPaint >= 33) { lastPaint = now; draw(); }
}

globalThis.chain_ui.onMidiMessageInternal = function(data) {
        if (!data || data.length < 3) return;
        const status = data[0] & 0xf0;
        const d1 = data[1], d2 = data[2];

        /* Nothing here is modified by Shift. It is swallowed rather than
         * ignored, so it cannot fall through to a branch that reads CC
         * numbers it does not own. */
        if (status === 0xb0 && d1 === CC_SHIFT) return;

        /* WHILE THE TUNER IS UP IT IS THE WHOLE SCREEN.
         *
         * Everything is swallowed and the jog click closes it. NOT Back:
         * the host handles Back itself before we are called at all
         * (shadow_ui.js routes everything except Back to a loaded module),
         * so Back leaves the module entirely - which is the right thing
         * for Back to do and the wrong thing for closing a sub-screen.
         * The jog click is the only button this file can be sure of, and
         * it is what opens the menu on the way in.
         *
         * Swallowed rather than ignored, so a pad press cannot fall
         * through to the board and stomp a block you cannot see. */
        if (tunerOpen) {
            if (status === 0xb0 && d1 === CC_JOG_CLICK && d2 > 0) {
                tunerOpen = false;
                tunerVal = null;
                lastPaint = 0;
            }
            return;
        }

        /* THE ARROWS MOVE THE WINDOW, and only on the PRESS.
         *
         * Acting on the release as well would page twice per tap. The
         * release still has to be consumed rather than fallen through,
         * because below this are branches that read CC numbers they do
         * not own.
         *
         * The shim withholds both edges from Move while this screen is up
         * - that is what capabilities.claims_ccs buys, and it is latched
         * there, so Move never sees a lone button-up for a key it never
         * saw go down. Nothing here needs to help with that. */
        if (status === 0xb0 && (d1 === CC_NAV_LEFT || d1 === CC_NAV_RIGHT)) {
            if (d2 > 0) setPage(page + (d1 === CC_NAV_RIGHT ? 1 : -1));
            return;
        }

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
        if (status === 0xb0 && d1 === CC_JOG_TURN) {
            const d = detents(d2);
            if (!d) return;
            if (menuOpen) {
                menuCursor += (d > 0 ? 1 : -1);
                if (menuCursor < 0) menuCursor = 0;
                if (menuCursor > menuRows.length - 1) menuCursor = menuRows.length - 1;
                lastPaint = 0;   /* the cursor moved; show it */
                return;
            }
            /* THE JOG IS THE MERGE, and it is the jog because the jog is
             * the one control on this screen that is PROVEN to arrive.
             *
             * Shift + a pad was the gesture and came back as not working,
             * with nothing in the device log either way - Shift is a
             * modifier several layers below us claim, and a gesture that
             * may or may not reach the module is not a gesture. The jog
             * CLICK is in that same log opening this module's own menu,
             * so the jog TURN is on the same wire. It is also free here:
             * with the menu closed it did nothing at all.
             *
             * Shift + a pad is kept beside it. If it works it is faster;
             * if it does not, nothing is lost. */
            jogAcc += d;
            const step = (jogAcc / JOG_PER_STEP) | 0;
            if (!step) return;
            jogAcc -= step * JOG_PER_STEP;
            let v = num(st.val['merge'], 0) + step;
            if (v < 0) v = 0;
            if (v > NUM_COLS) v = NUM_COLS;
            setMergeCol(v - 1, true);
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
        if (d1 >= TOP_BASE && d1 < TOP_BASE + PAGE_COLS) {
            const r = d1 - TOP_BASE;
            if (status === 0x90 && d2 > 0) {
                topDownAt[r] = Date.now();
                topHandled[r] = false;
            } else if (status === 0x80 || (status === 0x90 && d2 === 0)) {
                if (topDownAt[r] && !topHandled[r])
                    stomp(slotOf(ROW_BR, pageBase() + r));
                topDownAt[r] = 0;
                topHandled[r] = false;
            }
            return;
        }

        if (d1 < PAD_BASE || d1 >= PAD_BASE + PAGE_COLS) return;
        const b = d1 - PAD_BASE;

        if (status === 0x90 && d2 > 0) {
            padDownAt[b] = Date.now();
            padHandled[b] = false;
        } else if (status === 0x80 || (status === 0x90 && d2 === 0)) {
            /* A block pad stomps, full stop. Shift used to move it
             * between lanes here as well, which was one gesture doing two
             * jobs - and a lane is the ROW a pad is in now, so there is
             * nothing left for it to mean. */
            if (padDownAt[b] && !padHandled[b])
                stomp(slotOf(ROW_MAIN, pageBase() + b));
            padDownAt[b] = 0;
            padHandled[b] = false;
        }
};

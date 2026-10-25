/*
 * a2_fx.h - the pedals, as the real ones are laid out.
 *
 * WHAT CHANGED IN 0.6.0, AND WHY IT IS A REWRITE RATHER THAN A RENAME.
 *
 * The first cut had one "Overdrive" with Drive/Tone/Level. That is not a
 * pedal, it is a category - and a category has no voice, so every setting
 * of it was a guess. A TS808 and an SD-1 have the same three knobs and do
 * not sound alike, and the difference is entirely in things the old code
 * had no place to put: where the bass is cut relative to the clipper,
 * whether the clipping is symmetric, how much clean is blended back, and
 * where the mid hump sits.
 *
 * So a pedal is now a ROW IN A TABLE, and the table carries both halves:
 * the circuit constants the DSP reads, and the knob's name, unit and range
 * as the screen prints it. One table, so a knob that says "480 ms" is
 * reading the same number the delay line is.
 *
 * THAT IS THE LOAD-BEARING PART. The alternative - a mapping in C and a
 * matching one in ui_chain.js - is two copies of one fact in two
 * languages, and this module has already paid for that once: chain_params
 * and module.json disagreed for four builds and the CPU meter silently
 * read as an ordinary knob. The UI reads `fx_specs` from here, once, at
 * load.
 *
 * COST. The expensive thing in this module is the amp model (~1100 us of a
 * 2370 us frame); a pedal has to fit in what is left, seven at a time, so
 * none may cost more than a few tens of microseconds. Nothing here
 * allocates, and every recursive state is flushed - a filter decaying
 * toward zero passes through the subnormal range, where the FPU traps to
 * microcode, measured on this device at 50x on a block that should cost
 * 18 us. It bites when nothing is playing, so it reads as a random
 * dropout rather than as load.
 */

#ifndef A2_FX_H
#define A2_FX_H

#include "a2_common.h"

/* ====================================================================== */
/* The pedals                                                              */
/* ====================================================================== */

/* Flat ids. The UI groups them; the DSP does not care. The order is the
 * wire value, so it is appended to rather than reordered - though a module
 * id change (which every release of this module gets, because an in-place
 * .so overwrite gets ETXTBSY and silently does not land) means no board
 * from an older build is reading these anyway. */
enum {
    /* Overdrive */
    FX_TS808 = 0, FX_TS9, FX_SD1, FX_BLUESBRK, FX_CENTAUR,
    /* Distortion */
    FX_RAT, FX_DS1, FX_DISTPLUS, FX_FULLBORE, FX_SUPERBAD,
    /* Fuzz */
    FX_BIGMUFF, FX_FUZZFACE,
    /* Boost */
    FX_BOOST,
    /* Dynamics */
    FX_C1176, FX_DYNACOMP, FX_SPCOMP, FX_ROSSCOMP,
    FX_STARGATE, FX_LIMITER,
    /* Filter */
    FX_EQ, FX_AUTOWAH, FX_WAH, FX_LOFI,
    /* Modulation */
    FX_CHORUS, FX_FLANGER, FX_PHASER, FX_PHASE90,
    FX_VIBRATO, FX_TREMOLO, FX_ROTARY,
    /* Time */
    FX_DELAY, FX_DUALDELAY, FX_ANALOGDLY, FX_TAPE, FX_REVERB, FX_SPRING,
    /* Pitch */
    FX_DOUBLER, FX_DETUNE, FX_OCTAVE, FX_RINGMOD,
    /* Appended 0.7.0. Ten that a guitarist reaches for and that nothing
     * here could do: a Marshall in a box, a parametric mid, an octave
     * fuzz, a treble booster (which is NOT the clean boost - it is a
     * different circuit and a different job), a Univibe, a chorus with no
     * audible rate, a graphic EQ, a plate, and a delay that plays
     * backwards. The order is the wire value, so they go on the END and
     * the UI's categories put them where they belong. */
    FX_BLUESDRV, FX_PLEXI, FX_METALZONE, FX_OCTAVIA, FX_RANGEMASTER,
    FX_UNIVIBE, FX_DIMENSION, FX_GRAPHEQ, FX_PLATE, FX_REVERSE,
    /* Appended 1.5.0. The amp's own tone controls, which a CAPTURE cannot
     * have: a .nam is one setting of one amp and the Bass knob was turned
     * before it was taken. */
    FX_TONESTACK, FX_POWERAMP,
    /* Appended 1.7.0. A RETUNE, which is not a detune with a bigger
     * number - see fx_tuning. */
    FX_TUNING,
    FX_COUNT
};

/* 1250 ms at 44100 - the digital delay's 1200 ms ceiling with room for the
 * tape's wow to read behind it. 220 KB a block, 1.8 MB across the eight,
 * which is a fifth of what the chain instance already carries. */
#define FX_MAX_DELAY 55125
#define FX_PARAMS 5

/* ---------------------------------------------------------------- knobs */

/* How a knob is SPELLED and what its two ends mean. `curve` decides how
 * the 0..1 the hardware sends becomes the number under the name:
 *
 *   FXC_LIN   lo + (hi-lo) * p          a level, a depth, a percentage
 *   FXC_EXP   lo * (hi/lo)^p            a time or a frequency, where the
 *                                       useful half of the range is at the
 *                                       bottom and linear wastes it
 *   FXC_ENUM  round(p * (hi-1))         a switch; `opts` names the positions
 *
 * A knob with a NULL name is a hole at its own index, never a compaction -
 * Detune's Mix is p3 whether or not p2 exists, because the wire key is the
 * index and compacting would re-point it.
 */
enum { FXC_LIN = 0, FXC_EXP = 1, FXC_ENUM = 2 };

typedef struct fx_knob {
    const char *name;
    const char *unit;      /* "", "ms", "dB", "%", "Hz", ":1" */
    float lo, hi;
    unsigned char curve;
    const char *opts;      /* FXC_ENUM only, '|' separated */
    /* A KNOB WHOSE MEANING A SWITCH CHANGES. The delay's Time control is
     * milliseconds until Mode says Note, and then it is a division - same
     * encoder, different question. Two knobs would leave one of them dead
     * at all times and the screen has four cells; this is the pedal's own
     * answer, and the alternate lives in the SAME table so the label the
     * screen prints and the number the delay line reads cannot disagree. */
    const struct fx_knob *alt;
} fx_knob_t;

typedef struct {
    const char *name;
    fx_knob_t   knob[FX_PARAMS];
    /* Which knob is the switch that selects the alternates, or -1. A knob
     * uses its `alt` when THIS knob reads non-zero. */
    signed char alt_when;
} fx_pedal_t;

/* Eight divisions, dotted and triplet included, because a delay that can
 * only do straight notes cannot play with a shuffle. */
static const fx_knob_t K_NOTE = {
    "Note", "", 0, 8, FXC_ENUM, "1/2|1/4.|1/4|1/4T|1/8.|1/8|1/8T|1/16", NULL
};

#define KNOB_NONE { NULL, "", 0, 0, FXC_LIN, NULL }
#define K_LIN(n, u, a, b) { n, u, a, b, FXC_LIN, NULL }
#define K_EXP(n, u, a, b) { n, u, a, b, FXC_EXP, NULL }
#define K_ENUM(n, cnt, o) { n, "", 0, cnt, FXC_ENUM, o }
/* 0..100 with no unit - a knob whose number means nothing but itself,
 * which is what a Drive or a Depth control on a real pedal is. */
#define K_POS(n) { n, "", 0, 100, FXC_LIN, NULL }

static const fx_pedal_t FX_PEDALS[FX_COUNT] = {
/* --- Overdrive ------------------------------------------------------- */
{ "TS808",      { K_POS("Drive"), K_POS("Tone"), K_POS("Level"), KNOB_NONE, KNOB_NONE }, -1 },
{ "TS9",        { K_POS("Drive"), K_POS("Tone"), K_POS("Level"), KNOB_NONE, KNOB_NONE }, -1 },
{ "SD-1",       { K_POS("Drive"), K_POS("Tone"), K_POS("Level"), KNOB_NONE, KNOB_NONE }, -1 },
{ "Blues Brkr", { K_POS("Gain"),  K_POS("Tone"), K_POS("Volume"), KNOB_NONE, KNOB_NONE }, -1 },
{ "Centaur",    { K_POS("Gain"),  K_POS("Treble"), K_POS("Output"), KNOB_NONE, KNOB_NONE }, -1 },
/* --- Distortion ------------------------------------------------------ */
/* The RAT's is a FILTER, not a tone: clockwise rolls treble OFF. Printing
 * it as "Tone" would make every setting of it backwards. */
{ "RAT",        { K_POS("Dist"), K_POS("Filter"), K_POS("Volume"), KNOB_NONE, KNOB_NONE }, -1 },
{ "DS-1",       { K_POS("Dist"), K_POS("Tone"), K_POS("Level"), KNOB_NONE, KNOB_NONE }, -1 },
{ "Dist+",      { K_POS("Dist"), K_POS("Output"), KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
{ "Fullbore",   { K_POS("Gain"), K_LIN("Low", "dB", -12, 12), K_LIN("Mid", "dB", -12, 12),
                  K_LIN("High", "dB", -12, 12), K_POS("Level") }, -1 },
{ "SuperBad",   { K_POS("Dist"), K_LIN("Bass", "dB", -12, 12), K_LIN("Mid", "dB", -12, 12),
                  K_LIN("Treble", "dB", -12, 12), K_POS("Level") }, -1 },
/* --- Fuzz ------------------------------------------------------------ */
{ "Big Muff",   { K_POS("Sustain"), K_POS("Tone"), K_POS("Volume"), KNOB_NONE, KNOB_NONE }, -1 },
{ "Fuzz Face",  { K_POS("Fuzz"), K_POS("Volume"), KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
/* --- Boost ----------------------------------------------------------- */
/* Three knobs, as asked: how much, the tilt, and the top end on its own.
 * The level is in dB because that is the one number here you want to be
 * able to set rather than dial for. */
{ "Boost",      { K_LIN("dB", "dB", 0, 20), K_LIN("Tone", "dB", -9, 9),
                  K_LIN("Treble", "dB", -9, 9), KNOB_NONE, KNOB_NONE }, -1 },
/* --- Dynamics -------------------------------------------------------- */
/* The 1176's front panel: Input sets how hard it is hit (that IS the
 * threshold), Output makes it up, and the ratio is four buttons - plus
 * "All", which on the real unit is all four pressed at once and is a
 * different, dirtier machine rather than a fifth ratio. */
{ "1176",       { K_POS("Input"), K_EXP("Attack", "ms", 0.02f, 8.0f),
                  K_EXP("Release", "ms", 50, 1100), K_ENUM("Ratio", 5, "4:1|8:1|12:1|20:1|All"),
                  K_LIN("Output", "dB", 0, 24) }, -1 },
{ "Dyna Comp",  { K_POS("Sens"), K_LIN("Output", "dB", 0, 20), KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
{ "SP Comp",    { K_LIN("Volume", "dB", 0, 20), K_LIN("Blend", "%", 0, 100),
                  K_ENUM("Mode", 3, "Lo|Mid|Hi"), KNOB_NONE, KNOB_NONE }, -1 },
{ "Ross Comp",  { K_POS("Sustain"), K_LIN("Level", "dB", 0, 20), KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
/* ONE KNOB, as the pedal has. Everything else about a gate - how fast it
 * opens, how it lets go, how far below the opening point it closes - is a
 * decision the pedal makes for you, and making them here is the whole
 * reason it is a model rather than a Threshold/Release pair. */
{ "Star Gate",  { K_LIN("Threshold", "dB", -80, -20), KNOB_NONE, KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
{ "Limiter",    { K_LIN("Ceiling", "dB", -18, 0), K_EXP("Release", "ms", 20, 800),
                  KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
/* --- Filter ---------------------------------------------------------- */
{ "EQ",         { K_LIN("Bass", "dB", -12, 12), K_LIN("Mid", "dB", -12, 12),
                  K_LIN("Treble", "dB", -12, 12), KNOB_NONE, KNOB_NONE }, -1 },
{ "Auto Wah",   { K_POS("Sens"), K_POS("Range"), K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Wah",        { K_EXP("Pedal", "Hz", 320, 2200), K_POS("Q"),
                  K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Lo-Fi",      { K_LIN("Bits", "", 12, 2), K_LIN("Rate", "", 1, 32),
                  K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
/* --- Modulation ------------------------------------------------------ */
/*
 * RATE IS A PERIOD IN MILLISECONDS, not a 0-100 and not a frequency.
 *
 * Asked for, and right: "how long does one sweep take" is the question you
 * are actually answering when you set a chorus, and it is the number you
 * can match to a tempo by arithmetic. Hertz is the same fact upside down
 * and nobody sets a chorus to 0.7.
 *
 * AND THE DEPTHS ARE DELIBERATELY SHORT OF THE REAL PEDALS' MAXIMA.
 * Reported: "modulation 이 너무 심하면 너무 빙빙 도는 소리가 난다" - the
 * seasick end of a CE-2 or a BF-2 is a place these are turned to by
 * accident far more often than on purpose, and a Nuno-style rhythm tone is
 * nowhere near it. So Depth 100 here is about three quarters of the real
 * pedal's travel, which loses nothing anybody wanted and removes the
 * setting that makes a take unusable.
 */
{ "Chorus",     { K_EXP("Rate", "ms", 200, 10000), K_POS("Depth"),
                  K_LIN("Mix", "%", 0, 50), KNOB_NONE, KNOB_NONE }, -1 },
{ "Flanger",    { K_EXP("Rate", "ms", 400, 20000), K_POS("Depth"),
                  K_POS("Manual"), K_POS("Reso"), KNOB_NONE }, -1 },
{ "Phaser",     { K_EXP("Rate", "ms", 300, 12000), K_POS("Depth"), K_POS("Reso"),
                  K_ENUM("Stage", 4, "4|8|10|12"), KNOB_NONE }, -1 },
/* ONE KNOB. The Phase 90 has exactly one and that is the whole character
 * of it - the depth, the feedback and the stage count are not choices the
 * pedal offers, so offering them would be modelling something else and
 * calling it a Phase 90. */
{ "Phase 90",   { K_EXP("Speed", "ms", 300, 8000), KNOB_NONE, KNOB_NONE,
                  KNOB_NONE, KNOB_NONE }, -1 },
{ "Vibrato",    { K_EXP("Rate", "ms", 200, 5000), K_POS("Depth"),
                  K_EXP("Rise", "ms", 10, 2000), KNOB_NONE, KNOB_NONE }, -1 },
{ "Tremolo",    { K_EXP("Rate", "ms", 100, 3000), K_POS("Depth"),
                  K_ENUM("Wave", 3, "Tri|Sine|Sqr"), KNOB_NONE, KNOB_NONE }, -1 },
{ "Rotary",     { K_EXP("Speed", "ms", 150, 3000), K_POS("Depth"),
                  K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
/* --- Time ------------------------------------------------------------ */
/* Boss front-panel order is E.Level, F.Back, D.Time; the order here is the
 * one the encoders are in, and Time comes first because it is the one you
 * reach for. */
{ "Digital Dly",{ { "Time", "ms", 20, 1200, FXC_EXP, NULL, &K_NOTE },
                  K_POS("F.Back"), K_LIN("E.Level", "%", 0, 100),
                  K_ENUM("Mode", 2, "ms|Note"), KNOB_NONE }, 3 },
/* TWO LINES, ONE PEDAL. Not a delay with a second tap: the feedback is
 * shared and cross-fed, so A and B trade repeats and a 3:2 ratio between
 * them is a pattern rather than two echoes. */
{ "Dual Dly",   { { "Time A", "ms", 20, 1200, FXC_EXP, NULL, &K_NOTE },
                  { "Time B", "ms", 20, 1200, FXC_EXP, NULL, &K_NOTE },
                  K_POS("F.Back"), K_LIN("E.Level", "%", 0, 100),
                  K_ENUM("Mode", 2, "ms|Note") }, 4 },
{ "Analog Dly", { K_EXP("Time", "ms", 20, 330), K_POS("Intensity"),
                  K_LIN("E.Level", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Tape Echo",  { K_EXP("Time", "ms", 60, 700), K_POS("Intensity"),
                  K_LIN("E.Level", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Reverb",     { K_LIN("Time", "", 0, 100), K_LIN("Tone", "dB", -9, 9),
                  K_LIN("E.Level", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Spring",     { K_LIN("Decay", "", 0, 100), K_LIN("Tone", "dB", -9, 9),
                  K_LIN("E.Level", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
/* --- Pitch ----------------------------------------------------------- */
/* WIDTH IS THE WHOLE POINT, so this one is stereo - see fx_doubler. */
{ "Doubler",    { K_EXP("Delay", "ms", 12, 60), K_LIN("Detune", "cent", 0, 25),
                  K_LIN("Width", "%", 0, 100), K_LIN("Level", "%", 0, 100),
                  KNOB_NONE }, -1 },
{ "Detune",     { K_LIN("Amount", "cent", 2, 30), KNOB_NONE,
                  K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Octave",     { K_LIN("Sub", "%", 0, 100), K_LIN("Tone", "dB", -9, 9),
                  K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Ring Mod",   { K_EXP("Freq", "Hz", 20, 2000), KNOB_NONE,
                  K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
/* --- 0.7.0 ----------------------------------------------------------- */
{ "Blues Drv",  { K_POS("Gain"), K_POS("Tone"), K_POS("Level"), KNOB_NONE, KNOB_NONE }, -1 },
{ "Plexi",      { K_POS("Gain"), K_POS("Tone"), K_POS("Level"), KNOB_NONE, KNOB_NONE }, -1 },
/* THE SWEEPABLE MID IS THE PEDAL. An MT-2 with a fixed mid is just a
 * bright distortion; the whole reason it is loved and hated is that the
 * notch can be put anywhere from a honk to a scoop. */
{ "Metal Zone", { K_POS("Dist"), K_LIN("Low", "dB", -14, 14),
                  K_LIN("Mid", "dB", -14, 14), K_EXP("M.Freq", "Hz", 200, 5000),
                  K_POS("Level") }, -1 },
{ "Octavia",    { K_POS("Fuzz"), K_POS("Volume"), KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
/* NOT THE CLEAN BOOST. A Rangemaster is a germanium transistor with a
 * small coupling cap, so it passes almost NO bass at all and clips on its
 * way past - which is why a Marshall stops sounding woolly with one in
 * front and why a flat 20 dB lift does not do the same thing. */
{ "Rangemaster",{ K_LIN("Boost", "dB", 0, 26), K_EXP("Range", "Hz", 200, 2000),
                  KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
{ "Univibe",    { K_EXP("Speed", "ms", 200, 6000), K_POS("Intensity"),
                  K_LIN("Mix", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
/* A CHORUS WITH NO RATE KNOB, because the original has none. Two fixed,
 * slow, opposed sweeps written into a stereo field: it thickens without
 * ever announcing a cycle, which is exactly the thing asked for when the
 * complaint about modulation is that it goes round and round. */
{ "Dimension",  { K_ENUM("Mode", 4, "1|2|3|4"), K_LIN("Width", "%", 0, 100),
                  KNOB_NONE, KNOB_NONE, KNOB_NONE }, -1 },
{ "Graphic EQ", { K_LIN("100", "dB", -12, 12), K_LIN("400", "dB", -12, 12),
                  K_LIN("800", "dB", -12, 12), K_LIN("2k", "dB", -12, 12),
                  K_LIN("6k", "dB", -12, 12) }, -1 },
{ "Plate",      { K_LIN("Time", "", 0, 100), K_LIN("Tone", "dB", -9, 9),
                  K_LIN("E.Level", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
{ "Reverse",    { K_EXP("Time", "ms", 120, 1200), K_POS("F.Back"),
                  K_LIN("E.Level", "%", 0, 100), KNOB_NONE, KNOB_NONE }, -1 },
/* --- The amp's tone controls ----------------------------------------- */
/* A PASSIVE TONE STACK IS NOT A THREE BAND EQ, and we already have one of
 * those (FX_EQ), so this is only worth having if it behaves like the
 * circuit. The four things a generic EQ does not do:
 *
 *   - MID AT ZERO IS A NOTCH, not flat. That scoop is the sound of a
 *     Marshall; an EQ at 0 dB is the sound of nothing.
 *   - THE CONTROLS INTERACT. Raising Bass FILLS the mid scoop, because in
 *     the circuit the bass pot and the mid pot share the same leg.
 *   - IT ONLY EVER CUTS. The stack is passive and loses 10-20 dB; the
 *     amp makes it up afterwards, which is why every setting of it has a
 *     different level.
 *   - THE SCOOP SITS SOMEWHERE DIFFERENT PER AMP, and that frequency is
 *     most of what tells a Fender from a Marshall.
 *
 * Each of those four is a line in tests/test_fx.cpp. They were measured
 * with numpy before a line of this was written, which is why the numbers
 * below are the ones they are.
 *
 * WHAT IT CANNOT DO, and nothing after a capture can: on a real amp the
 * stack sits BEFORE the power section, so turning Bass up changes what
 * the valves clip on. Here the distortion is already baked into the
 * capture. ToneX has exactly the same limit. Put it between the amp and
 * the cab, which is where the circuit has it relative to the speaker.
 */
{ "Tone Stack", { K_ENUM("Amp", 4, "Fendr|Marsh|Vox|Mdrn"),
                  K_POS("Bass"), K_POS("Mid"), K_POS("Treble"),
                  K_POS("Presnc") }, -1 },
/* --- What a capture cannot remember ---------------------------------- */
/* A .nam IS A STATIC FUNCTION: input sample in, output sample out, with
 * no memory of what came before. Every nonlinearity a real amp has that
 * depends on TIME is therefore absent from it by construction - not
 * missed by the capture, impossible for it.
 *
 * The two that a player feels rather than hears:
 *
 *  SAG    the supply rail drops as the output stage draws current, so a
 *         hard chord ducks over a few tens of ms and swells back as the
 *         reservoir refills. That swell is the "bloom", and it is the
 *         single biggest reason a capture feels stiffer than the amp it
 *         was taken from.
 *  BIAS   on a big transient the grid conducts, the bias walks negative
 *         and the valve cuts off for an instant - the spit on a hard
 *         pick and the flub on a low note. Asymmetric, level dependent,
 *         and gone again in 50 ms.
 *
 * Both only ever ADD to the capture. Neither duplicates anything in it,
 * which is the test for whether a post-capture process is worth having.
 */
{ "Power Amp",  { K_POS("Sag"), K_LIN("Bloom", "ms", 50, 600),
                  K_POS("Bias"), KNOB_NONE, KNOB_NONE }, -1 },

/* The tunings are named from the LOW string, the way a guitarist names
 * them, and ordered by pitch so the encoder walks down to C and up to F#
 * instead of jumping about. E is index 0 rather than the middle, which is
 * why this pedal needs fx_block_apply_defaults. */
{ "Tuning",     { K_ENUM("Tune", 7, "E|Eb|D|C#|C|F|F#"),
                  K_LIN("Fine", "", -50, 50), K_POS("Mix"),
                  KNOB_NONE, KNOB_NONE }, -1 },
};

/* The table and the enum are one fact in two places, and a table one row
 * short is a read past its end on the highest pedal - a pedal that works
 * in the DSP and crashes the picker. */
static_assert(sizeof(FX_PEDALS) / sizeof(FX_PEDALS[0]) == FX_COUNT,
              "FX_PEDALS must describe every pedal in the enum");

/* ------------------------------------------------------- reading a knob */

static inline float fx_knob_value(const fx_knob_t *k, float p) {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    if (k->curve == FXC_EXP) return k->lo * powf(k->hi / k->lo, p);
    if (k->curve == FXC_ENUM) {
        int n = (int)k->hi;
        if (n < 1) n = 1;
        int i = (int)(p * (n - 1) + 0.5f);
        return (float)((i < 0) ? 0 : (i > n - 1) ? n - 1 : i);
    }
    return k->lo + (k->hi - k->lo) * p;
}

struct fx_block;
static inline const fx_knob_t *fx_knob_of(const struct fx_block *f, int i);
static inline float fx_v(const struct fx_block *f, int i);

/* ------------------------------------------------------------------ util */

static inline float fx_onepole(float *state, float x, float a) {
    *state = flush_denormal(*state + a * (x - *state));
    return *state;
}

/* A one-pole coefficient for a cutoff in Hz. */
static inline float fx_coeff(float hz) {
    float a = 1.0f - expf(-2.0f * (float)M_PI * hz / SAMPLE_RATE);
    return (a < 0.0f) ? 0.0f : (a > 1.0f ? 1.0f : a);
}

static inline float fx_db2lin(float db) { return powf(10.0f, db / 20.0f); }

/* The biquads this file used to own live in a2_common.h now: the cab's mic
 * emulation needs the same three functions and sits in that header, which
 * this one includes rather than the other way round. */

/* ------------------------------------------------------------------ state */

typedef struct fx_block {
    int   id;
    float p[FX_PARAMS];         /* the encoders, 0..1 */
    float bpm;                  /* host tempo, for the delays' Note mode */

    /* gain stages */
    float hp_x1, hp_y1, hp2_x1, hp2_y1, lp_y1, lp2_y1, dc_x1, dc_y1;
    fx_bqs_t bq1, bq2, bq3, bq4;   /* the tone stack needs four */
    /* dynamics */
    float env, env2, gain_s, gate_gain;
    int   hold;
    /* eq / wah */
    float lo_s, mid_s, hi_s, wah_lp2, wah_bp;
    /* modulation */
    float lfo_phase, lfo2_phase, rise;
    float ap[12];               /* phaser allpass states, up to 12 stages */
    /* delay line, shared by everything that needs one */
    float *line;
    int    line_len;
    int    w;
    float  rd, rd2;      /* two read heads - the doubler needs both */
    /* reverb / spring */
    float *rv;
    int    rv_len;
    int    comb_i[4], ap_i[4];
    float  comb_z[4];
    /* doubler / tape: a random walk, not a second LFO */
    float  walk, walk_to, walk2, walk2_to;
    int    walk_cnt;
    float  oct_sign, oct_lp;
    float  sh_val;
    int    sh_cnt;
    unsigned rng;
} fx_block_t;

static inline const fx_knob_t *fx_knob_of(const fx_block_t *f, int i) {
    const fx_pedal_t *pd = &FX_PEDALS[f->id];
    const fx_knob_t *k = &pd->knob[i];
    if (k->alt && pd->alt_when >= 0 &&
        fx_knob_value(&pd->knob[pd->alt_when], f->p[pd->alt_when]) >= 1.0f)
        return k->alt;
    return k;
}
static inline float fx_v(const fx_block_t *f, int i) {
    return fx_knob_value(fx_knob_of(f, i), f->p[i]);
}

static inline float fx_rand(fx_block_t *f) {
    f->rng = f->rng * 1664525u + 1013904223u;
    return (float)((f->rng >> 9) & 0x7FFFFF) / 8388607.0f;
}

/* Reverb buffers: Schroeder's shape, also carved up by the spring. */
#define RV_COMB0 1557
#define RV_COMB1 1617
#define RV_COMB2 1491
#define RV_COMB3 1422
#define RV_AP0    225
#define RV_AP1     77
#define RV_TOTAL (RV_COMB0 + RV_COMB1 + RV_COMB2 + RV_COMB3 + RV_AP0 + RV_AP1)

static int fx_block_alloc(fx_block_t *f) {
    if (!f->line) {
        f->line = (float *)calloc(FX_MAX_DELAY, sizeof(float));
        if (!f->line) return 0;
        f->line_len = FX_MAX_DELAY;
    }
    if (!f->rv) {
        f->rv = (float *)calloc(RV_TOTAL, sizeof(float));
        if (!f->rv) return 0;
        f->rv_len = RV_TOTAL;
    }
    return 1;
}

static void fx_block_free(fx_block_t *f) {
    free(f->line); f->line = NULL; f->line_len = 0;
    free(f->rv);   f->rv = NULL;   f->rv_len = 0;
}

/* Changing pedal must not leave the previous one's tail ringing through
 * the new one's filters. */
static void fx_block_reset(fx_block_t *f) {
    f->hp_x1 = f->hp_y1 = f->hp2_x1 = f->hp2_y1 = 0.0f;
    f->lp_y1 = f->lp2_y1 = f->dc_x1 = f->dc_y1 = 0.0f;
    memset(&f->bq1, 0, sizeof(f->bq1));
    memset(&f->bq2, 0, sizeof(f->bq2));
    memset(&f->bq3, 0, sizeof(f->bq3));
    f->env = f->env2 = 0.0f; f->gain_s = 1.0f; f->gate_gain = 1.0f; f->hold = 0;
    f->lo_s = f->mid_s = f->hi_s = f->wah_lp2 = f->wah_bp = 0.0f;
    f->lfo_phase = f->lfo2_phase = 0.0f; f->rise = 0.0f;
    for (int i = 0; i < 12; i++) f->ap[i] = 0.0f;
    f->w = 0; f->rd = f->rd2 = 0.0f;
    for (int i = 0; i < 4; i++) { f->comb_i[i] = 0; f->ap_i[i] = 0; f->comb_z[i] = 0.0f; }
    f->walk = f->walk_to = f->walk2 = f->walk2_to = 0.0f; f->walk_cnt = 0;
    f->oct_sign = 1.0f; f->oct_lp = 0.0f;
    f->sh_val = 0.0f; f->sh_cnt = 0;
    if (!f->rng) f->rng = 22695477u;
    if (f->line) memset(f->line, 0, (size_t)f->line_len * sizeof(float));
    if (f->rv)   memset(f->rv, 0, (size_t)f->rv_len * sizeof(float));
}

/* A PEDAL WHOSE NEUTRAL POSITION IS NOT THE MIDDLE OF ITS RANGE.
 *
 * Every knob in this table is created at 0.5 and that is right for all but
 * one of them, because the middle is what a Depth, a Mix or a Drive means
 * with nothing asked for. An enum that names TUNINGS is the exception:
 * index 3 of seven is C# standard, so a Tuning block would retune the
 * guitar by three semitones the moment it was dropped on the board.
 *
 * The default is the PEDAL'S, so it lives beside the pedal rather than at
 * the call site - the same reason ir_rv_defaults() exists. Called on an id
 * CHANGE only: the knobs of a pedal that did not change still mean what
 * they meant, and a state load writes its saved values after this runs.
 */
static void fx_block_apply_defaults(fx_block_t *f) {
    for (int k = 0; k < FX_PARAMS; k++) f->p[k] = 0.5f;
    if (f->id == FX_TUNING) f->p[0] = 0.0f;   /* E standard - no shift */
}

static inline float fx_tap(fx_block_t *f, float delay_samples) {
    float rp = (float)f->w - delay_samples;
    while (rp < 0.0f) rp += (float)f->line_len;
    int i0 = (int)rp;
    float fr = rp - i0;
    int i1 = (i0 + 1 >= f->line_len) ? 0 : i0 + 1;
    return f->line[i0] + (f->line[i1] - f->line[i0]) * fr;
}

/* ====================================================================== */
/* Gain stages - overdrive, distortion, fuzz, boost                        */
/* ====================================================================== */

/*
 * ONE ENGINE, A TABLE OF CIRCUITS.
 *
 * Every pedal in the first three groups is the same six blocks in the same
 * order - cut some bass, shape a bump, multiply, clip, roll off the fizz,
 * then a tone control - and what makes a TS808 a TS808 rather than a DS-1
 * is entirely the numbers. Writing ten process functions would be writing
 * the same loop ten times and letting nine of them drift.
 *
 * The four that carry the character, in the order they matter:
 *
 *   pre_hp   WHERE THE BASS IS CUT RELATIVE TO THE CLIPPER. This is the
 *            single biggest difference between a screamer and a fuzz. The
 *            TS cuts at 720 Hz BEFORE clipping, so the low end never
 *            reaches the diodes and the amp stays tight; the Big Muff
 *            cuts almost nothing, and keeping its bottom end is most of
 *            why it is a Muff.
 *   clean    HOW MUCH DRY IS ADDED BACK. Zero for a TS, which is why it
 *            has a voice; high for a Klon and a Bluesbreaker, which is
 *            what "transparent" means and why people pay for them.
 *   asym     ASYMMETRIC CLIPPING, i.e. even harmonics. An SD-1 is a TS
 *            with one extra diode and that is the whole audible
 *            difference.
 *   hard     how abruptly it turns over: soft for a tube-ish overdrive,
 *            abrupt for silicon clipped to ground.
 */
enum { TONE_TS = 0, TONE_RAT, TONE_DS1, TONE_MUFF,
       TONE_TILT, TONE_TREBLE, TONE_3BAND, TONE_PARAM, TONE_NONE };

typedef struct {
    float pre_hp;          /* Hz, before the clipper */
    float gain_max;        /* multiplier at Drive full */
    float asym;            /* 0..1 */
    float hard;            /* clip abruptness */
    float mid_hz, mid_q, mid_db;
    float post_lp;         /* Hz, fizz roll-off */
    float clean;           /* dry blended back, 0..1 */
    float out;             /* output trim */
    unsigned char tone;
    unsigned char stages;  /* cascaded clipping stages */
} od_model_t;

/* KEYED BY ID, NOT BY POSITION.
 *
 * This was a plain array indexed by the pedal's id, which worked exactly
 * as long as every gain pedal sat in one unbroken run at the front of the
 * enum - and 0.7.0 appended a Blues Driver and a Plexi to the END, where
 * the wire format requires new entries to go. A positional table would
 * then have read the Doubler's row for the Plexi, silently: both are
 * in-range small integers.
 *
 * A linear scan of fifteen rows, once per block, is beneath measurement. */
typedef struct { int id; od_model_t m; } od_row_t;

static const od_row_t OD_ROWS[] = {
/*                          pre_hp gain  asym hard  mid_hz   q  mid_db post_lp clean  out  tone        stages */
{ FX_TS808,      {   720,   30, 0.00f, 1.00f,  720, 0.80f,  5.0f,  6000, 0.00f, 0.30f, TONE_TS,     1 } },
{ FX_TS9,        {   720,   34, 0.00f, 1.10f,  760, 0.80f,  4.5f,  7500, 0.00f, 0.32f, TONE_TS,     1 } },
{ FX_SD1,        {   720,   32, 0.35f, 1.15f,  780, 0.90f,  3.0f,  7000, 0.00f, 0.32f, TONE_TS,     1 } },
{ FX_BLUESBRK,   {   170,   22, 0.00f, 0.75f,    0, 1.00f,  0.0f,  9000, 0.45f, 0.42f, TONE_TILT,   1 } },
{ FX_CENTAUR,    {   180,   45, 0.15f, 1.30f,    0, 1.00f,  0.0f,  8500, 0.55f, 0.36f, TONE_TREBLE, 1 } },
/* A BD-2 is not a screamer with a different badge: it keeps its bass (the
 * cut is at 120 Hz, not 720), clips on MOSFETs rather than diodes so the
 * knee is softer and later, and leaves a little clean underneath. That is
 * the "amp-like, not boxy" people buy it for. */
{ FX_BLUESDRV,   {   120,   60, 0.25f, 0.90f,  500, 0.70f,  2.0f,  7000, 0.15f, 0.30f, TONE_TS,     1 } },
/* The missing flavour: a British amp rather than a pedal. Its hump sits
 * up at 1.8 kHz where a Marshall's presence lives, not down at 720 where
 * a TS puts it, which is most of why one cuts and the other honks. */
{ FX_PLEXI,      {   260,   90, 0.20f, 1.40f, 1800, 0.60f,  4.0f,  6500, 0.10f, 0.26f, TONE_TILT,   1 } },
{ FX_RAT,        {   430,  500, 0.00f, 2.40f, 1000, 0.70f,  4.0f, 10000, 0.00f, 0.16f, TONE_RAT,    1 } },
{ FX_DS1,        {   500,  220, 0.00f, 2.00f,    0, 1.00f,  0.0f,  9000, 0.00f, 0.20f, TONE_DS1,    1 } },
{ FX_DISTPLUS,   {   340,  160, 0.45f, 1.80f,  900, 0.80f,  3.0f,  5200, 0.00f, 0.22f, TONE_NONE,   1 } },
{ FX_FULLBORE,   {   300,  900, 0.00f, 2.60f,    0, 1.00f,  0.0f,  8000, 0.00f, 0.15f, TONE_3BAND,  2 } },
{ FX_SUPERBAD,   {   240,  420, 0.00f, 2.00f,    0, 1.00f,  0.0f,  9000, 0.00f, 0.17f, TONE_3BAND,  1 } },
{ FX_METALZONE,  {   340, 1200, 0.00f, 2.80f,    0, 1.00f,  0.0f,  7000, 0.00f, 0.13f, TONE_PARAM,  2 } },
{ FX_BIGMUFF,    {    80,  260, 0.00f, 2.20f,    0, 1.00f,  0.0f,  7000, 0.00f, 0.22f, TONE_MUFF,   2 } },
{ FX_FUZZFACE,   {    60,  120, 0.60f, 1.60f,    0, 1.00f,  0.0f,  6000, 0.00f, 0.28f, TONE_NONE,   1 } },
};

static const od_model_t *od_model_for(int id) {
    for (unsigned i = 0; i < sizeof(OD_ROWS) / sizeof(OD_ROWS[0]); i++)
        if (OD_ROWS[i].id == id) return &OD_ROWS[i].m;
    return &OD_ROWS[0].m;
}

static inline float od_clip(float x, float hard, float asym) {
    /* The positive half turns over sooner than the negative one, which is
     * what one extra diode in one leg does and what puts even harmonics
     * in an SD-1 and a Dist+. */
    float h = (x > 0.0f) ? hard * (1.0f + asym) : hard;
    return tanhf(x * h);
}

static void fx_gain_stage(fx_block_t *f, float *a, int n) {
    const int id = f->id;
    const od_model_t *m = od_model_for(id);

    /* Which encoder is which differs per pedal, so read them by POSITION
     * in that pedal's own row rather than assuming p1 is always drive. */
    float drive, tone, level;
    float b_db = 0, m_db = 0, t_db = 0;
    float mid_hz = 650.0f;
    if (id == FX_FULLBORE || id == FX_SUPERBAD) {
        drive = f->p[0];
        b_db = fx_v(f, 1); m_db = fx_v(f, 2); t_db = fx_v(f, 3);
        tone = 0.5f; level = f->p[4];
    } else if (id == FX_METALZONE) {
        drive = f->p[0];
        b_db = fx_v(f, 1); m_db = fx_v(f, 2); mid_hz = fx_v(f, 3);
        tone = 0.5f; level = f->p[4];
    } else if (id == FX_DISTPLUS) {
        drive = f->p[0]; tone = 0.5f; level = f->p[1];
    } else if (id == FX_FUZZFACE) {
        drive = f->p[0]; tone = 0.5f; level = f->p[1];
    } else {
        drive = f->p[0]; tone = f->p[1]; level = f->p[2];
    }

    /* A DRIVE KNOB IS NOT LINEAR IN GAIN. Squaring it puts the useful part
     * of every one of these - the first third, where it is still an
     * overdrive - across half the travel instead of the first eighth. */
    const float gain = 1.0f + drive * drive * (m->gain_max - 1.0f);
    const float hp_a = fx_coeff(m->pre_hp);
    const float lp_a = fx_coeff(m->post_lp);
    const float clean = m->clean * (0.35f + 0.65f * drive);

    /* Coefficients once per block, never per sample. */
    fx_bq_t hump, band_lo, band_mid, band_hi;
    int use_hump = (m->mid_db != 0.0f);
    if (use_hump) fx_bq_peak(&hump, m->mid_hz, m->mid_q, m->mid_db);
    /* The MT-2's mid is a PARAMETRIC: one bell whose centre the fourth
     * knob sweeps two and a half octaves, over a fixed scooped voicing.
     * Put it at 200 Hz and it is a bark; at 5 kHz it is an ice pick; in
     * the middle with the gain down it is the scoop it is famous for. */
    fx_bq_t mz_low, mz_mid;
    if (m->tone == TONE_PARAM) {
        fx_bq_shelf(&mz_low, 120.0f, b_db, 0);
        fx_bq_peak(&mz_mid, mid_hz, 1.1f, m_db);
    }
    int three = (m->tone == TONE_3BAND);
    if (three) {
        /* Fullbore is voiced scooped before the knobs touch it; SuperBad
         * is flat, which is the difference between "a metal pedal" and "a
         * distortion with an EQ on it". */
        float scoop = (id == FX_FULLBORE) ? -5.0f : 0.0f;
        fx_bq_shelf(&band_lo, 160.0f, b_db, 0);
        fx_bq_peak(&band_mid, 650.0f, 0.7f, m_db + scoop);
        fx_bq_shelf(&band_hi, 2600.0f, t_db, 1);
    }

    /* Tone circuits that need a corner computed per block. */
    float tone_a = 0.0f;
    fx_bq_t shelf;
    switch (m->tone) {
        case TONE_TS:   tone_a = fx_coeff(700.0f * powf(6000.0f / 700.0f, tone)); break;
        /* CLOCKWISE IS DARKER. The RAT's control is a Filter, not a Tone,
         * and printing it the other way round would make every setting of
         * it backwards. */
        case TONE_RAT:  tone_a = fx_coeff(12000.0f * powf(600.0f / 12000.0f, tone)); break;
        case TONE_DS1:  tone_a = fx_coeff(900.0f); break;
        case TONE_MUFF: tone_a = fx_coeff(800.0f); break;
        case TONE_TILT: tone_a = fx_coeff(900.0f); break;
        case TONE_TREBLE: fx_bq_shelf(&shelf, 1500.0f, -9.0f + tone * 19.0f, 1); break;
        default: break;
    }
    fx_bq_t ds1_dip;
    if (m->tone == TONE_DS1) fx_bq_peak(&ds1_dip, 500.0f, 0.9f, -4.0f);

    const float outg = m->out * (0.05f + level * 1.35f);

    for (int i = 0; i < n; i++) {
        const float dry = a[i];
        float x = dry;

        /* pre-clip bass cut, as a highpass built from the one-pole we
         * already have: what the lowpass keeps is what the clipper must
         * not see. */
        x = x - fx_onepole(&f->lo_s, x, hp_a);

        if (use_hump) x = fx_bq_run(&hump, &f->bq1, x);

        x *= gain;
        x = od_clip(x, m->hard, m->asym);
        if (m->stages > 1) {
            /* A second stage does not sound like more of the first: it
             * clips what the first one already flattened, which is where a
             * Muff's wall and a Fullbore's compression come from. */
            x = od_clip(x * (1.0f + drive * 3.0f), m->hard, m->asym * 0.5f);
        }

        /* post-clip fizz roll-off */
        x = fx_onepole(&f->lp_y1, x, lp_a);

        switch (m->tone) {
            case TONE_TS: {
                float lp = fx_onepole(&f->hi_s, x, tone_a);
                x = lp + (x - lp) * (0.25f + tone * 0.75f);
                break;
            }
            case TONE_RAT:
                x = fx_onepole(&f->hi_s, x, tone_a);
                break;
            case TONE_DS1: {
                float lp = fx_onepole(&f->hi_s, x, tone_a);
                x = lp * (1.6f - 1.4f * tone) + (x - lp) * (0.3f + 1.6f * tone);
                x = fx_bq_run(&ds1_dip, &f->bq2, x);
                break;
            }
            case TONE_MUFF: {
                /* The scoop IS the tone control: the two branches are a
                 * lowpass and a highpass and the knob crossfades between
                 * them, so the middle position is the one with the notch. */
                float lp = fx_onepole(&f->hi_s, x, tone_a);
                float hp = x - lp;
                x = lp * (1.0f - tone) * 1.5f + hp * tone * 1.5f;
                break;
            }
            case TONE_TILT: {
                float lp = fx_onepole(&f->hi_s, x, tone_a);
                x = lp * (1.4f - 0.9f * tone) + (x - lp) * (0.4f + 1.1f * tone);
                break;
            }
            case TONE_TREBLE:
                x = fx_bq_run(&shelf, &f->bq2, x);
                break;
            case TONE_3BAND:
                x = fx_bq_run(&band_lo, &f->bq1, x);
                x = fx_bq_run(&band_mid, &f->bq2, x);
                x = fx_bq_run(&band_hi, &f->bq3, x);
                break;
            case TONE_PARAM:
                x = fx_bq_run(&mz_low, &f->bq1, x);
                x = fx_bq_run(&mz_mid, &f->bq2, x);
                break;
            default: break;
        }

        /* THE CLEAN BLEND IS WHY A KLON IS A KLON. It is added, not mixed:
         * the dry is never turned down, so the pick attack and the low end
         * arrive unclipped and the gain sits behind them. */
        x = x + dry * clean;

        a[i] = sanitize_sample(x * outg);
    }
}

/* Boost: three knobs, and none of them is a drive.
 *
 * A boost's whole job is to hit what comes after it harder, so the level
 * is in dB and the two shapers are gentle shelves rather than a tone
 * circuit. It still clips - at 20 dB into a 1.0 ceiling something has to -
 * but softly and late, so what you hear is the amp breaking up rather than
 * the pedal. */
static void fx_boost(fx_block_t *f, float *a, int n) {
    const float g = fx_db2lin(fx_v(f, 0));
    fx_bq_t tilt, treb;
    fx_bq_shelf(&tilt, 700.0f, -fx_v(f, 1), 0);      /* tone: bass side, inverted */
    fx_bq_shelf(&treb, 3000.0f, fx_v(f, 2), 1);
    for (int i = 0; i < n; i++) {
        float x = a[i];
        float lo = fx_onepole(&f->lo_s, x, fx_coeff(25.0f));
        x -= lo;                                      /* a boost has no sub */
        x = fx_bq_run(&tilt, &f->bq1, x);
        x = fx_bq_run(&treb, &f->bq2, x);
        x *= g;
        a[i] = sanitize_sample(tanhf(x * 0.7f) * 1.35f);
    }
}

/* ====================================================================== */
/* Dynamics                                                                */
/* ====================================================================== */

/*
 * FOUR COMPRESSORS, ONE DETECTOR, AND THE DIFFERENCES ARE REAL ONES.
 *
 * A 1176 and a Dyna Comp are not the same box with different knobs: the
 * FET unit's attack is measured in MICROseconds and its release follows
 * the programme, so it catches the pick and lets the note breathe; the
 * OTA pedals have one fixed, slowish attack and a long release, which is
 * why they square off a pick attack into that famous "pop" and why
 * country players own them.
 *
 * So the table carries attack, release, ratio, knee and whether the
 * release is programme-dependent, and the knobs differ because the real
 * front panels differ - the 1176 has no threshold control at all, it has
 * an INPUT knob, and how hard you hit it is the threshold.
 */
typedef struct {
    float thr_lo, thr_hi;    /* dB, what the pedal's own knob spans */
    float atk_ms, rel_ms;
    float ratio;
    float knee_db;
    unsigned char prog_rel;  /* release follows the programme */
} comp_model_t;

static const comp_model_t COMP_MODELS[] = {
/* 1176      */ { -34, -6,  0.05f,  300, 4.0f, 3.0f, 1 },   /* knobs override */
/* Dyna Comp */ { -30, -4,  6.0f,   450, 6.0f, 6.0f, 0 },
/* SP Comp   */ { -32, -6,  3.0f,   260, 5.0f, 8.0f, 0 },
/* Ross Comp */ { -34, -6,  8.0f,   520, 5.5f, 9.0f, 0 },
};

static void fx_comp(fx_block_t *f, float *a, int n) {
    const int which = f->id - FX_C1176;
    const comp_model_t *m = &COMP_MODELS[which];

    float thr_db, atk_ms, rel_ms, ratio, makeup_db, blend = 1.0f;
    switch (f->id) {
        case FX_C1176: {
            /* INPUT IS THE THRESHOLD. Turning it up on the real unit does
             * not raise a threshold, it drives the detector harder, and
             * the two are the same thing seen from either side. */
            thr_db = m->thr_lo + (m->thr_hi - m->thr_lo) * (1.0f - f->p[0]);
            atk_ms = fx_v(f, 1);
            rel_ms = fx_v(f, 2);
            const int r = (int)fx_v(f, 3);
            static const float RATIOS[5] = { 4, 8, 12, 20, 30 };
            ratio = RATIOS[(r < 0) ? 0 : (r > 4) ? 4 : r];
            /* "All buttons in" is not a fifth ratio - it is the unit
             * mis-biased, so the knee collapses and the thing distorts.
             * That is what people press it for. */
            makeup_db = fx_v(f, 4);
            if (r == 4) { thr_db -= 8.0f; }
            break;
        }
        case FX_SPCOMP: {
            const int mode = (int)fx_v(f, 2);
            static const float MODE_THR[3] = { -20.0f, -28.0f, -36.0f };
            thr_db = MODE_THR[(mode < 0) ? 0 : (mode > 2) ? 2 : mode];
            atk_ms = m->atk_ms; rel_ms = m->rel_ms; ratio = m->ratio;
            makeup_db = fx_v(f, 0);
            /* THE BLEND IS THE PEDAL. A Ross clone with a dry blend stops
             * being a squash and becomes the thing people leave on. */
            blend = fx_v(f, 1) * 0.01f;
            break;
        }
        default:
            thr_db = m->thr_lo + (m->thr_hi - m->thr_lo) * (1.0f - f->p[0]);
            atk_ms = m->atk_ms; rel_ms = m->rel_ms; ratio = m->ratio;
            makeup_db = fx_v(f, 1);
            break;
    }

    const float thr = fx_db2lin(thr_db);
    const float makeup = fx_db2lin(makeup_db);
    const float atk = fx_coeff(1000.0f / (atk_ms > 0.01f ? atk_ms : 0.01f));
    const float rel = fx_coeff(1000.0f / rel_ms);
    const float rel_slow = fx_coeff(1000.0f / (rel_ms * 4.0f));
    const float slope = (1.0f / ratio) - 1.0f;
    const float knee = m->knee_db;

    for (int i = 0; i < n; i++) {
        const float dry = a[i];
        float mag = fabsf(dry);
        f->env = flush_denormal(f->env + (mag > f->env ? atk : rel) * (mag - f->env));

        float g = 1.0f;
        if (f->env > 1e-9f) {
            float over_db = 20.0f * log10f(f->env / thr);
            if (over_db > -knee) {
                /* A SOFT KNEE, because the hard one is audible as a click
                 * on every note that crosses it. */
                float amt = (over_db < knee)
                          ? (over_db + knee) * (over_db + knee) / (4.0f * knee)
                          : over_db;
                g = fx_db2lin(amt * slope);
            }
        }
        /* PROGRAMME-DEPENDENT RELEASE: let go fast after a transient, slow
         * under sustain. One extra follower, and it is most of what makes
         * the 1176 sit still on a chord and still breathe on a riff. */
        if (m->prog_rel) {
            f->env2 = flush_denormal(f->env2 + rel_slow * (mag - f->env2));
            float w = (f->env2 > thr) ? 1.0f : 0.35f;
            f->gain_s = flush_denormal(f->gain_s +
                        ((g < f->gain_s) ? atk : rel * w) * (g - f->gain_s));
        } else {
            f->gain_s = flush_denormal(f->gain_s +
                        ((g < f->gain_s) ? atk : rel) * (g - f->gain_s));
        }

        float wet = dry * f->gain_s * makeup;
        a[i] = sanitize_sample(wet * blend + dry * (1.0f - blend));
    }
}

/*
 * STAR GATE - one knob, and everything else is a decision the pedal makes.
 *
 * A gate in front of a very high gain amp has one job and two ways to get
 * it wrong: chatter on the decay of every note, and chop the tail off a
 * sustained one. A Threshold/Release pair leaves both to the player. So
 * this is fixed at what works and exposes only the level:
 *
 *   - it EXPANDS rather than gates. Below the threshold the signal is
 *     pulled down at about 4:1 instead of muted, so a note dying under the
 *     line fades rather than stopping, which is the difference between a
 *     gate you can hear and one you cannot.
 *   - hysteresis of 9 dB, so what opens it is not what closes it and a
 *     note hovering at the line cannot flutter;
 *   - a 40 ms HOLD after it opens, which is what stops chatter during the
 *     pick attack's own envelope ripple;
 *   - attack under a millisecond (a gate that is late clips the pick),
 *     release ~120 ms (fast enough to kill hum between phrases, slow
 *     enough not to cut the tail).
 */
static void fx_stargate(fx_block_t *f, float *a, int n) {
    const float open_db = fx_v(f, 0);
    const float open_at = fx_db2lin(open_db);
    const float close_at = fx_db2lin(open_db - 9.0f);
    const float atk = fx_coeff(1400.0f);
    const float rel = fx_coeff(8.5f);
    const float det = fx_coeff(60.0f);
    const int   hold_samples = (int)(0.040f * SAMPLE_RATE);
    const float ratio = 0.25f;          /* 4:1 downward expansion */

    for (int i = 0; i < n; i++) {
        float mag = fabsf(a[i]);
        f->env = flush_denormal(f->env + (mag > f->env ? atk : det) * (mag - f->env));

        if (f->env > open_at) f->hold = hold_samples;
        else if (f->hold > 0) f->hold--;

        float want;
        if (f->env > open_at || f->hold > 0) {
            want = 1.0f;
        } else if (f->env < close_at) {
            want = 0.0f;
        } else {
            /* Between the two lines it is an expander, not a switch. */
            float below_db = 20.0f * log10f((f->env + 1e-12f) / open_at);
            want = fx_db2lin(below_db * (1.0f / ratio - 1.0f) * -1.0f);
            if (want > 1.0f) want = 1.0f;
            if (want < 0.0f) want = 0.0f;
        }
        f->gate_gain = flush_denormal(f->gate_gain +
                       ((want > f->gate_gain) ? atk : rel) * (want - f->gate_gain));
        a[i] = sanitize_sample(a[i] * f->gate_gain);
    }
}

static void fx_limiter(fx_block_t *f, float *a, int n) {
    /* Not the compressor with the ratio turned up: the detector is a PEAK
     * follower with an instant attack, so it catches the sample that would
     * clip rather than the average that would not. */
    const float ceil_lin = fx_db2lin(fx_v(f, 0));
    const float rel = fx_coeff(1000.0f / fx_v(f, 1));

    for (int i = 0; i < n; i++) {
        float x = a[i];
        float mag = fabsf(x);
        if (mag > f->env) f->env = mag;
        else f->env = flush_denormal(f->env + rel * (mag - f->env));
        float g = (f->env > ceil_lin && f->env > 1e-9f) ? (ceil_lin / f->env) : 1.0f;
        float y = x * g;
        if (y > ceil_lin) y = ceil_lin;
        else if (y < -ceil_lin) y = -ceil_lin;
        a[i] = sanitize_sample(y);
    }
}

/* ====================================================================== */
/* Filter                                                                  */
/* ====================================================================== */

static void fx_eq(fx_block_t *f, float *a, int n) {
    fx_bq_t lo, mid, hi;
    fx_bq_shelf(&lo, 180.0f, fx_v(f, 0), 0);
    fx_bq_peak(&mid, 800.0f, 0.7f, fx_v(f, 1));
    fx_bq_shelf(&hi, 2800.0f, fx_v(f, 2), 1);
    for (int i = 0; i < n; i++) {
        float x = fx_bq_run(&lo, &f->bq1, a[i]);
        x = fx_bq_run(&mid, &f->bq2, x);
        x = fx_bq_run(&hi, &f->bq3, x);
        a[i] = sanitize_sample(x);
    }
}

static void fx_autowah(fx_block_t *f, float *a, int n) {
    const float sens = 0.5f + f->p[0] * 8.0f;
    const float range = f->p[1];
    const float mix = fx_v(f, 2) * 0.01f;
    const float env_a = fx_coeff(30.0f);
    for (int i = 0; i < n; i++) {
        float x = a[i];
        f->env = flush_denormal(f->env + env_a * (fabsf(x) - f->env));
        float e = f->env * sens;
        if (e > 1.0f) e = 1.0f;
        float hz = 300.0f + e * (300.0f + 2200.0f * range);
        float fc = 2.0f * sinf((float)M_PI * hz / SAMPLE_RATE);
        if (fc > 0.7f) fc = 0.7f;
        float hpf = x - f->wah_lp2 - 0.25f * f->wah_bp;
        f->wah_bp = flush_denormal(f->wah_bp + fc * hpf);
        f->wah_lp2 = flush_denormal(f->wah_lp2 + fc * f->wah_bp);
        a[i] = sanitize_sample(x * (1.0f - mix) + f->wah_bp * 2.0f * mix);
    }
}

static void fx_wah(fx_block_t *f, float *a, int n) {
    /* The sweep is on a KNOB, which is the useful difference from the auto
     * wah on a device with no expression pedal: a knob can be driven by an
     * LFO or an automation lane and a pedal cannot. */
    const float hz = fx_v(f, 0);
    const float mix = fx_v(f, 2) * 0.01f;
    float fc = 2.0f * sinf((float)M_PI * hz / SAMPLE_RATE);
    if (fc > 0.7f) fc = 0.7f;
    const float q = 0.38f - f->p[1] * 0.30f;
    for (int i = 0; i < n; i++) {
        float x = a[i];
        float hpf = x - f->wah_lp2 - q * f->wah_bp;
        f->wah_bp = flush_denormal(f->wah_bp + fc * hpf);
        f->wah_lp2 = flush_denormal(f->wah_lp2 + fc * f->wah_bp);
        a[i] = sanitize_sample(x * (1.0f - mix) + f->wah_bp * 2.2f * mix);
    }
}

static void fx_lofi(fx_block_t *f, float *a, int n) {
    const float steps = powf(2.0f, fx_v(f, 0));
    const int   div = (int)(fx_v(f, 1) + 0.5f);
    const float mix = fx_v(f, 2) * 0.01f;
    for (int i = 0; i < n; i++) {
        if (--f->sh_cnt <= 0) {
            f->sh_cnt = (div < 1) ? 1 : div;
            float q = a[i] * 0.5f + 0.5f;
            q = (float)((int)(q * steps + 0.5f)) / steps;
            f->sh_val = q * 2.0f - 1.0f;
        }
        a[i] = sanitize_sample(a[i] * (1.0f - mix) + f->sh_val * mix);
    }
}

/* ====================================================================== */
/* Modulation                                                              */
/* ====================================================================== */

/* Rate arrives as a PERIOD IN MILLISECONDS, so the increment is its
 * reciprocal. One place, so no pedal can read it the other way round. */
static inline float fx_lfo_inc(float period_ms) {
    if (period_ms < 1.0f) period_ms = 1.0f;
    return 1000.0f / (period_ms * SAMPLE_RATE);
}

static void fx_chorus(fx_block_t *f, float *a, int n) {
    const float inc = fx_lfo_inc(fx_v(f, 0));
    const float depth = f->p[1];
    const float mix = fx_v(f, 2) * 0.01f;
    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float lfo = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * f->lfo_phase);
        f->line[f->w] = a[i];
        /* 6 to 14 ms. A CE-2 lives here; shorter is a flanger and longer
         * is a doubler. */
        float d = (0.006f + 0.008f * lfo * depth) * SAMPLE_RATE;
        float y = fx_tap(f, d);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(a[i] * (1.0f - mix) + y * mix * 2.0f);
    }
}

static void fx_flanger(fx_block_t *f, float *a, int n) {
    const float inc = fx_lfo_inc(fx_v(f, 0));
    const float depth = f->p[1];
    const float manual = f->p[2];
    const float fb = f->p[3] * 0.80f;
    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float lfo = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * f->lfo_phase);
        /* MANUAL SETS WHERE THE SWEEP STARTS, which is the knob that makes
         * a BF-2 a jet and not a wobble: parked, it is a fixed comb. */
        float base = 0.0004f + manual * 0.0035f;
        float d = (base + 0.0045f * lfo * depth) * SAMPLE_RATE;
        float y = fx_tap(f, d);
        f->line[f->w] = flush_denormal(a[i] + y * fb);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample((a[i] + y) * 0.7f);
    }
}

static void fx_phaser(fx_block_t *f, float *a, int n) {
    int stages;
    float inc, depth, reso;
    if (f->id == FX_PHASE90) {
        /* The Phase 90 has ONE knob. Its four stages, its depth and its
         * feedback are the pedal, not settings of it - which is exactly
         * why it is the one people can set in the dark. */
        inc = fx_lfo_inc(fx_v(f, 0));
        stages = 4; depth = 0.85f; reso = 0.0f;
    } else {
        inc = fx_lfo_inc(fx_v(f, 0));
        depth = f->p[1];
        reso = f->p[2] * 0.7f;
        static const int ST[4] = { 4, 8, 10, 12 };
        int s = (int)fx_v(f, 3);
        stages = ST[(s < 0) ? 0 : (s > 3) ? 3 : s];
    }
    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float lfo = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * f->lfo_phase);
        float d = 0.30f + 0.50f * lfo * depth;
        float v = a[i] + f->lp2_y1 * reso;
        for (int k = 0; k < stages; k++) {
            float out = -d * v + f->ap[k];
            f->ap[k] = flush_denormal(v + d * out);
            v = out;
        }
        f->lp2_y1 = flush_denormal(v);
        a[i] = sanitize_sample((a[i] + v) * 0.62f);
    }
}

static void fx_vibrato(fx_block_t *f, float *a, int n) {
    /* All wet. A vibrato that keeps its dry IS a chorus - the dry is what
     * makes the comb - so there is no mix knob, as there is none on a
     * VB-2 in Vibrato mode. */
    const float inc = fx_lfo_inc(fx_v(f, 0));
    const float depth = f->p[1];
    /* RISE TIME: the VB-2's third knob, how long the modulation takes to
     * arrive after the note. */
    const float rise_a = fx_coeff(1000.0f / fx_v(f, 2));
    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        f->rise = flush_denormal(f->rise + rise_a * (1.0f - f->rise));
        float lfo = sinf(2.0f * (float)M_PI * f->lfo_phase);
        f->line[f->w] = a[i];
        float d = (0.005f + 0.0035f * lfo * depth * f->rise) * SAMPLE_RATE;
        float y = fx_tap(f, d);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(y);
    }
}

static void fx_tremolo(fx_block_t *f, float *a, int n) {
    const float inc = fx_lfo_inc(fx_v(f, 0));
    const float depth = f->p[1];
    const int wave = (int)fx_v(f, 2);
    /* The square is SLEWED, not stepped - a hard edge is a click on every
     * cycle and no amp tremolo has ever made one. */
    const float slew = fx_coeff(900.0f);
    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float ph = f->lfo_phase, lfo;
        if (wave == 1)      lfo = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * ph);
        else if (wave == 2) lfo = (ph < 0.5f) ? 1.0f : 0.0f;
        else                lfo = (ph < 0.5f) ? (ph * 2.0f) : (2.0f - ph * 2.0f);
        if (wave == 2) lfo = fx_onepole(&f->lp_y1, lfo, slew);
        a[i] = sanitize_sample(a[i] * (1.0f - depth * (1.0f - lfo)));
    }
}

static void fx_rotary(fx_block_t *f, float *a, int n) {
    /* Horn and drum at different rates, each contributing a doppler shift
     * and an amplitude sweep. Mono, so the stereo half of a Leslie is not
     * here; what IS here is two speeds interacting, which is what makes it
     * a rotary rather than a tremolo. */
    const float inc_h = fx_lfo_inc(fx_v(f, 0));
    const float inc_d = inc_h * 0.78f;
    const float depth = f->p[1];
    const float mix = fx_v(f, 2) * 0.01f;
    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc_h;  if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        f->lfo2_phase += inc_d; if (f->lfo2_phase >= 1.0f) f->lfo2_phase -= 1.0f;
        float h = sinf(2.0f * (float)M_PI * f->lfo_phase);
        float d = sinf(2.0f * (float)M_PI * f->lfo2_phase);
        f->line[f->w] = a[i];
        float dly = (0.0025f + 0.0015f * h * depth) * SAMPLE_RATE;
        float y = fx_tap(f, dly);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        float am = 1.0f - depth * 0.35f * (1.0f - h) - depth * 0.2f * (1.0f - d);
        a[i] = sanitize_sample(a[i] * (1.0f - mix) + y * am * mix);
    }
}

/* ====================================================================== */
/* Time                                                                    */
/* ====================================================================== */

/*
 * MS OR NOTE, AND THE SAME ENCODER ANSWERS BOTH.
 *
 * In Note mode the Time knob stops being a number of milliseconds and
 * becomes a division; the screen reads the alternate spelling out of the
 * same table, so the label, the option list and the number the delay line
 * uses are one fact.
 *
 * Tempo comes from the host - MIDI clock, else a set tempo, else the
 * project - and a host that will not answer falls back to 120 rather than
 * to silence: a delay that stops working because nothing is playing is
 * worse than one that is briefly at the wrong tempo.
 */
static float fx_time_ms(fx_block_t *f, int i) {
    const fx_pedal_t *pd = &FX_PEDALS[f->id];
    int note_mode = (pd->alt_when >= 0 &&
                     fx_knob_value(&pd->knob[pd->alt_when], f->p[pd->alt_when]) >= 1.0f);
    if (!note_mode) return fx_v(f, i);

    /* Beats per division, quarter = 1: half, dotted 4th, 4th, 4th triplet,
     * dotted 8th, 8th, 8th triplet, 16th. */
    static const float MULT[8] = {
        2.0f, 1.5f, 1.0f, 2.0f / 3.0f, 0.75f, 0.5f, 1.0f / 3.0f, 0.25f
    };
    int d = (int)fx_v(f, i);
    if (d < 0) d = 0;
    if (d > 7) d = 7;
    float bpm = (f->bpm > 20.0f && f->bpm < 400.0f) ? f->bpm : 120.0f;
    float ms = (60000.0f / bpm) * MULT[d];
    /* A half note at 40 BPM is three seconds and the line is 1250 ms, so
     * this is a real clamp rather than a formality. Reading past the write
     * head would return the PREVIOUS lap - a delay at a time nothing asked
     * for, which is much harder to recognise than one that is simply
     * shorter than the note. */
    const float max_ms = (float)(FX_MAX_DELAY - 256) * 1000.0f / SAMPLE_RATE;
    while (ms > max_ms) ms *= 0.5f;
    return ms;
}

static void fx_delay(fx_block_t *f, float *a, int n) {
    const int analog = (f->id == FX_ANALOGDLY);
    const float ms = analog ? fx_v(f, 0) : fx_time_ms(f, 0);
    const float fb = f->p[1] * (analog ? 0.80f : 0.88f);
    const float mix = fx_v(f, 2) * 0.01f;
    const float dly = ms * 0.001f * SAMPLE_RATE;
    /* A BBD loses treble on EVERY pass and compresses; a digital delay
     * repeats what it was given. That is the whole difference and it is
     * two numbers. */
    const float damp = fx_coeff(analog ? 1500.0f : 4500.0f);
    for (int i = 0; i < n; i++) {
        float x = a[i];
        float y = fx_tap(f, dly);
        float back = fx_onepole(&f->lp_y1, y, damp) * fb;
        if (analog) back = tanhf(back * 1.4f) * 0.75f;
        f->line[f->w] = flush_denormal(x + back);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x + y * mix);
    }
}

/*
 * TWO LINES SHARING ONE FEEDBACK PATH.
 *
 * Not a delay with a second tap: A's output feeds B's input and B's feeds
 * A's, so the repeats alternate and a 3:2 between the times is a pattern
 * rather than two echoes running past each other. One buffer, two read
 * heads - which is only possible because both heads read BEHIND the single
 * write head.
 */
static void fx_dual_delay(fx_block_t *f, float *a, int n) {
    const float msA = fx_time_ms(f, 0);
    const float msB = fx_time_ms(f, 1);
    const float fb = f->p[2] * 0.82f;
    const float mix = fx_v(f, 3) * 0.01f;
    const float dA = msA * 0.001f * SAMPLE_RATE;
    const float dB = msB * 0.001f * SAMPLE_RATE;
    const float damp = fx_coeff(4000.0f);
    for (int i = 0; i < n; i++) {
        float x = a[i];
        float yA = fx_tap(f, dA);
        float yB = fx_tap(f, dB);
        /* Halved, because two taps at full feedback is a loop gain of two
         * and the thing runs away on the second repeat. */
        float back = fx_onepole(&f->lp_y1, (yA + yB) * 0.5f, damp) * fb;
        f->line[f->w] = flush_denormal(x + back);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x + (yA + yB) * 0.5f * mix * 1.4f);
    }
}

static void fx_tape(fx_block_t *f, float *a, int n) {
    const float base = fx_v(f, 0) * 0.001f * SAMPLE_RATE;
    const float fb = f->p[1] * 0.88f;
    const float mix = fx_v(f, 2) * 0.01f;
    const float damp = fx_coeff(1900.0f);
    const float wow_inc = 0.6f / SAMPLE_RATE;
    const float slew = fx_coeff(3.0f);
    const int   reaim = (int)(SAMPLE_RATE / 9.0f);
    for (int i = 0; i < n; i++) {
        float x = a[i];
        f->lfo_phase += wow_inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float wow = sinf(2.0f * (float)M_PI * f->lfo_phase) * base * 0.004f;
        if (--f->walk_cnt <= 0) {
            f->walk_cnt = reaim;
            f->walk_to = (fx_rand(f) * 2.0f - 1.0f) * base * 0.0015f;
        }
        f->walk = flush_denormal(f->walk + slew * (f->walk_to - f->walk));
        float d = base + wow + f->walk;
        if (d < 8.0f) d = 8.0f;
        float y = fx_tap(f, d);
        float back = tanhf(fx_onepole(&f->lp_y1, y, damp) * fb * 1.3f) * 0.8f;
        f->line[f->w] = flush_denormal(x + back);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x + y * mix);
    }
}

static void fx_reverb(fx_block_t *f, float *a, int n) {
    const int len[4] = { RV_COMB0, RV_COMB1, RV_COMB2, RV_COMB3 };
    float *comb[4];
    comb[0] = f->rv;
    comb[1] = comb[0] + RV_COMB0;
    comb[2] = comb[1] + RV_COMB1;
    comb[3] = comb[2] + RV_COMB2;
    float *ap0 = comb[3] + RV_COMB3;
    float *ap1 = ap0 + RV_AP0;

    /* A PLATE IS NOT A BRIGHTER ROOM. A steel sheet has no walls, so there
     * are no discrete early reflections - the density is there from the
     * first millisecond and the decay is smooth and long. Here that is a
     * longer tail, much less damping, and no dry-side early character:
     * the same combs, different numbers, and the difference is audible. */
    const int plate = (f->id == FX_PLATE);
    const float room = plate ? (0.84f + f->p[0] * 0.15f)
                             : (0.70f + f->p[0] * 0.28f);
    const float mix = fx_v(f, 2) * 0.01f;
    fx_bq_t tone;
    fx_bq_shelf(&tone, plate ? 3200.0f : 2200.0f, fx_v(f, 1), 1);
    const float damp = plate ? (0.18f - f->p[1] * 0.10f)
                             : (0.45f - f->p[1] * 0.25f);

    for (int i = 0; i < n; i++) {
        float x = a[i] * 0.25f;
        float acc = 0.0f;
        for (int c = 0; c < 4; c++) {
            float y = comb[c][f->comb_i[c]];
            f->comb_z[c] = flush_denormal(y * (1.0f - damp) + f->comb_z[c] * damp);
            comb[c][f->comb_i[c]] = flush_denormal(x + f->comb_z[c] * room);
            if (++f->comb_i[c] >= len[c]) f->comb_i[c] = 0;
            acc += y;
        }
        for (int s = 0; s < 2; s++) {
            float *buf = s ? ap1 : ap0;
            int L = s ? RV_AP1 : RV_AP0;
            float y = buf[f->ap_i[s]];
            buf[f->ap_i[s]] = flush_denormal(acc + y * 0.5f);
            acc = y - acc;
            if (++f->ap_i[s] >= L) f->ap_i[s] = 0;
        }
        acc = fx_bq_run(&tone, &f->bq1, acc);
        a[i] = sanitize_sample(a[i] + acc * mix);
    }
}

static void fx_spring(fx_block_t *f, float *a, int n) {
    /* A spring is DISPERSIVE: highs travel the wire faster than lows, so a
     * transient arrives as a descending chirp - the "boing". A Schroeder
     * room has no dispersion and cannot make that sound however its combs
     * are tuned, which is why this is a separate pedal. */
    float *comb = f->rv;
    float *ap[4];
    ap[0] = comb + RV_COMB0;
    ap[1] = ap[0] + 225;
    ap[2] = ap[1] + 311;
    ap[3] = ap[2] + 409;
    const int apl[4] = { 225, 311, 409, 521 };

    const float decay = 0.55f + f->p[0] * 0.33f;
    /* THE TANK'S GAIN IS 1/(1-decay), so a fixed input scale would make
     * the DECAY knob a volume knob. Scaling the input by (1-decay) holds
     * the steady state where it was put, which is what the knob is
     * supposed to mean: how long, not how loud. */
    const float drive = 1.6f * (1.0f - decay);
    const float mix = fx_v(f, 2) * 0.01f;
    fx_bq_t tone;
    fx_bq_shelf(&tone, 2000.0f, fx_v(f, 1), 1);
    const float damp = 0.45f;

    for (int i = 0; i < n; i++) {
        float x = a[i] - fx_onepole(&f->lo_s, a[i], fx_coeff(300.0f));
        float v = x * drive + f->comb_z[0] * decay;
        for (int k = 0; k < 4; k++) {
            int idx = (k == 0) ? f->ap_i[0] : f->comb_i[k - 1];
            float yv = ap[k][idx];
            /* THE UNITY-GAIN ALLPASS SUBTRACTS THE DELAY'S INPUT, NOT ITS
             * OWN. Written as `yv - g*in` this has gain (1+g*g-g)/(1-g) at
             * DC - 2.0 at g=0.62 - so four inside a feedback loop is a
             * loop gain of 16 and the tank runs away. It did: the harness
             * measured a 4.8-million peak. */
            float t = v + yv * 0.62f;
            ap[k][idx] = flush_denormal(t);
            v = yv - 0.62f * t;
            if (++idx >= apl[k]) idx = 0;
            if (k == 0) f->ap_i[0] = idx; else f->comb_i[k - 1] = idx;
        }
        float y = comb[f->ap_i[1]];
        comb[f->ap_i[1]] = flush_denormal(v);
        if (++f->ap_i[1] >= RV_COMB0) f->ap_i[1] = 0;
        f->comb_z[0] = flush_denormal(y * (1.0f - damp) + f->comb_z[0] * damp);
        v = fx_bq_run(&tone, &f->bq1, v);
        a[i] = sanitize_sample(a[i] + v * mix * 1.2f);
    }
}

/* ====================================================================== */
/* Pitch                                                                   */
/* ====================================================================== */

/*
 * THE DOUBLER IS STEREO, AND THAT IS THE WHOLE FIX.
 *
 * Two previous attempts at this were mono, and both were reported as
 * sounding wrong - first as "it is just a chorus", then, after the
 * modulation was taken out of it, as still unnatural. The second report
 * was right and the diagnosis was not a setting.
 *
 * A double-track is two takes arriving from two PLACES. What your ear
 * uses to hear them as two players is almost entirely the difference
 * between your two ears - the delay and the detune are what make the
 * copies survivable, but the WIDTH is what makes them a second guitarist
 * instead of a smeared one. Summed to mono, two copies 30 ms apart are by
 * definition a comb filter on one signal, and no amount of tuning makes a
 * comb filter sound like a room. That is why the mono version kept
 * reading as artificial however its numbers were set, and why this is the
 * only pedal here that writes a side channel.
 *
 *   A   delayed by Delay, pitched UP by Detune, drifting on its own walk
 *   B   delayed by 1.32x that, pitched DOWN, drifting on another
 *   mid  dry + (A+B)/2          <- always, whatever the width
 *   side (A-B)/2 * Width        <- what the two ears differ by
 *
 * At Width 0 it collapses to the mono doubler, deliberately: mono
 * compatibility is a real requirement and it should be a knob position
 * rather than a different pedal.
 *
 * Detune at 0 is pure ADT - a fixed offset with only the random walk
 * moving it. Turning it up is where the Neural-style shimmer comes from,
 * and unlike an LFO the two copies move in OPPOSITE directions, so there
 * is no common rate for the ear to lock onto and call a chorus.
 */
static void fx_doubler(fx_block_t *f, float *a, float *side, int n) {
    const float ms = fx_v(f, 0);
    const float cents = fx_v(f, 1);
    const float width = fx_v(f, 2) * 0.01f;
    const float level = fx_v(f, 3) * 0.01f;

    const float baseA = ms * 0.001f * SAMPLE_RATE;
    const float baseB = ms * 1.32f * 0.001f * SAMPLE_RATE;
    const float win = 0.045f * SAMPLE_RATE;
    const float upA = powf(2.0f, cents / 1200.0f) - 1.0f;
    const float dnB = powf(2.0f, -cents / 1200.0f) - 1.0f;
    /* ~6 Hz re-aim on a walk, not a rate: what makes two takes differ is
     * that neither of them is periodic. */
    const int   reaim = (int)(SAMPLE_RATE / 6.0f);
    const float slew = fx_coeff(0.8f);
    const float span = 0.0012f * SAMPLE_RATE;
    const float dark = fx_coeff(3600.0f);
    const float dark2 = fx_coeff(2600.0f);

    for (int i = 0; i < n; i++) {
        const float dry = a[i];
        f->line[f->w] = dry;

        if (--f->walk_cnt <= 0) {
            f->walk_cnt = reaim;
            f->walk_to  = (fx_rand(f) * 2.0f - 1.0f) * span;
            f->walk2_to = (fx_rand(f) * 2.0f - 1.0f) * span;
        }
        f->walk  = flush_denormal(f->walk  + slew * (f->walk_to  - f->walk));
        f->walk2 = flush_denormal(f->walk2 + slew * (f->walk2_to - f->walk2));

        f->rd += upA;
        while (f->rd >= win) f->rd -= win;
        while (f->rd < 0.0f)  f->rd += win;
        f->rd2 += dnB;
        while (f->rd2 >= win) f->rd2 -= win;
        while (f->rd2 < 0.0f)  f->rd2 += win;

        /* Equal-power crossfade between two taps half a window apart, so
         * the wrap is inaudible. */
        float tA = f->rd / win;
        float aA = baseA + f->walk + f->rd;
        float aB = aA + win * 0.5f;
        if (aB >= baseA + f->walk + win) aB -= win;
        float A = fx_tap(f, aA) * sinf((float)M_PI * (1.0f - tA))
                + fx_tap(f, aB) * sinf((float)M_PI * tA);

        float tB = f->rd2 / win;
        float bA = baseB + f->walk2 + f->rd2;
        float bB = bA + win * 0.5f;
        if (bB >= baseB + f->walk2 + win) bB -= win;
        float B = fx_tap(f, bA) * sinf((float)M_PI * (1.0f - tB))
                + fx_tap(f, bB) * sinf((float)M_PI * tB);

        /* The copies are DARKER, and not by the same amount - two takes
         * are never the same distance from the cab. */
        A = fx_onepole(&f->lp_y1, A, dark);
        B = fx_onepole(&f->lp2_y1, B, dark2);

        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;

        a[i]    = sanitize_sample(dry + level * (A + B) * 0.5f);
        side[i] = sanitize_sample(side[i] + level * (A - B) * 0.5f * width);
    }
}

static void fx_detune(fx_block_t *f, float *a, int n) {
    const float cents = fx_v(f, 0);
    const float mix = fx_v(f, 2) * 0.01f;
    const float ratio = powf(2.0f, cents / 1200.0f);
    const float win = 0.050f * SAMPLE_RATE;
    const float base = 0.012f * SAMPLE_RATE;
    for (int i = 0; i < n; i++) {
        float x = a[i];
        f->line[f->w] = x;
        f->rd += (ratio - 1.0f);
        while (f->rd >= win) f->rd -= win;
        while (f->rd < 0.0f)  f->rd += win;
        float d0 = base + f->rd;
        float d1 = d0 + win * 0.5f;
        if (d1 >= base + win) d1 -= win;
        float t = f->rd / win;
        float y = fx_tap(f, d0) * sinf((float)M_PI * (1.0f - t))
                + fx_tap(f, d1) * sinf((float)M_PI * t);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x + y * mix);
    }
}

/* THE TUNING PEDAL, and why it is not Detune with a bigger number.
 *
 * Detune ADDS a shifted copy to the dry (`x + y*mix`) because the beating
 * between the two IS the effect. A retune must REPLACE: Eb standard means
 * the Eb, and Eb sounding together with the E it came from is a minor
 * second, which reads as a mistake rather than as a tuning. So the only
 * structural difference below is the last line - a crossfade where Detune
 * has a sum.
 *
 * The engine is the same two-tap crossfaded delay line, and its LATENCY IS
 * INHERENT rather than a shortcoming. A read pointer moving at a different
 * rate than the write pointer must start behind it, and the window it can
 * travel before it has to jump back is what sets both the base delay and
 * the warble period. 12 ms is about standing four metres from a cabinet.
 *
 * Put this FIRST in the chain, ahead of the drive and the capture, or the
 * amp is responding to the pitch you played instead of the one you hear.
 */
static const signed char TUNE_SEMI[7] = { 0, -1, -2, -3, -4, +1, +2 };
/*                                        E  Eb   D  C#   C  F   F#   */

static void fx_tuning(fx_block_t *f, float *a, int n) {
    int sel = (int)(fx_v(f, 0) + 0.5f);
    if (sel < 0) sel = 0;
    if (sel > 6) sel = 6;
    const float mix = fx_v(f, 2) * 0.01f;
    const float semis = (float)TUNE_SEMI[sel] + fx_v(f, 1) * 0.01f;

    /* E standard with no fine offset is a BYPASS - but the line has to keep
     * running through it. Returning outright leaves whatever was in the
     * line when the knob last passed E, and switching back plays those
     * 60 ms of the past as a blip. (fx_block_reset zeroes the line, but it
     * only runs when the PEDAL changes, not when this knob moves.) */
    if (semis > -0.0005f && semis < 0.0005f) {
        for (int i = 0; i < n; i++) {
            f->line[f->w] = flush_denormal(a[i]);
            f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        }
        f->rd = 0.0f;
        return;
    }

    const float ratio = powf(2.0f, semis / 12.0f);
    const float win = 0.050f * SAMPLE_RATE;
    const float base = 0.012f * SAMPLE_RATE;
    for (int i = 0; i < n; i++) {
        const float x = a[i];
        f->line[f->w] = flush_denormal(x);
        /* THE DELAY GROWS TO SHIFT DOWN. The read position advances at
         * `ratio` while the write advances at 1, so the gap between them
         * opens at (1 - ratio) a sample - positive for a downshift. The
         * other sign is a shift UP, and fx_detune has it: a detune sounds
         * the same either way, which is how that survived being read. */
        f->rd += (1.0f - ratio);
        while (f->rd >= win) f->rd -= win;
        while (f->rd < 0.0f)  f->rd += win;
        float d0 = base + f->rd;
        float d1 = d0 + win * 0.5f;
        if (d1 >= base + win) d1 -= win;
        /* A CROSSFADE NEEDS TWO DIFFERENT WEIGHTS, and the obvious pair
         * is not one: sin(PI*(1-t)) IS sin(PI*t) - the same function, so
         * writing the second that way applies ONE weight to both taps and
         * the result is a fixed sum of two reads half a window apart, i.e.
         * a comb. On A3 (220 Hz) win/2 is 5.5 periods, which puts the taps
         * exactly out of phase and cancels the note to silence.
         *
         * sin / |cos| is the pair. Each is zero at its OWN tap's jump
         * (tap0 wraps at t=0, tap1 at t=0.5, which is where d1 crosses
         * base+win), the squares sum to 1 at every t, and neither is ever
         * negative. fx_detune carries the wrong pair and is left alone on
         * purpose: it ADDS a few cents to the dry, so there the comb is
         * mild amplitude modulation that people have already dialled in
         * around, not a null. */
        const float t = f->rd / win;
        const float ang = (float)M_PI * t;
        const float y = fx_tap(f, d0) * sinf(ang)
                      + fx_tap(f, d1) * fabsf(cosf(ang));
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x * (1.0f - mix) + y * mix);
    }
}

static void fx_octave(fx_block_t *f, float *a, int n) {
    /* A DIVIDER, not a pitch shifter. Lowpass hard, watch for a rising
     * zero crossing, flip a sign on each one: the flip-flop runs at half
     * the input's frequency, and multiplying it by the input's own
     * envelope gives the octave a body instead of a square wave. This is
     * how the analog octave pedals do it, and it is why they track one
     * note at a time - that limitation IS the sound. */
    const float level = fx_v(f, 0) * 0.01f;
    const float mix = fx_v(f, 2) * 0.01f;
    const float track = fx_coeff(420.0f);
    const float env_a = fx_coeff(22.0f);
    fx_bq_t tone;
    fx_bq_shelf(&tone, 900.0f, fx_v(f, 1), 1);
    for (int i = 0; i < n; i++) {
        float x = a[i];
        float prev = f->oct_lp;
        float lp = fx_onepole(&f->oct_lp, x, track);
        if (prev <= 0.0f && lp > 0.0f && fabsf(lp) > 1e-4f)
            f->oct_sign = -f->oct_sign;
        f->env = flush_denormal(f->env + env_a * (fabsf(x) - f->env));
        float sub = f->oct_sign * f->env * 2.0f;
        sub = fx_onepole(&f->lp_y1, sub, fx_coeff(1200.0f));
        sub = fx_bq_run(&tone, &f->bq1, sub);
        a[i] = sanitize_sample(x + sub * level * mix);
    }
}

static void fx_ringmod(fx_block_t *f, float *a, int n) {
    const float inc = fx_v(f, 0) / SAMPLE_RATE;
    const float mix = fx_v(f, 2) * 0.01f;
    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float c = sinf(2.0f * (float)M_PI * f->lfo_phase);
        a[i] = sanitize_sample(a[i] * (1.0f - mix) + a[i] * c * mix);
    }
}

/* ====================================================================== */
/* 0.7.0                                                                   */
/* ====================================================================== */

/*
 * OCTAVIA - a fuzz with the octave BUILT INTO the distortion, not after it.
 *
 * The trick is full-wave rectification: |x| has twice as many peaks per
 * cycle as x, so it rings an octave up, and rectifying BEFORE the clipping
 * is what makes the octave and the fuzz one sound instead of two stacked.
 * It only tracks cleanly on single notes high on the neck - that is the
 * pedal, not a shortcoming, and it is why everyone plays the same lick on
 * it.
 *
 * The band-pass afterwards is doing real work: raw rectification is all
 * intermodulation down low, and narrowing it to where the octave actually
 * sits is the difference between a note and a splat.
 */
static void fx_octavia(fx_block_t *f, float *a, int n) {
    const float fuzz = f->p[0];
    const float vol = f->p[1];
    const float gain = 1.0f + fuzz * fuzz * 180.0f;
    const float hp_a = fx_coeff(180.0f);
    fx_bq_t band;
    /* +4 dB, not +7. A broad bell at Q 0.55 gains most of the band, so
     * the boost lands on a signal tanh has already pushed to 1.0 and the
     * output pinned - the harness caught it at full scale. The band's job
     * is to say WHERE the octave sits, not to add level. */
    fx_bq_peak(&band, 1400.0f, 0.55f, 4.0f);
    const float lp_a = fx_coeff(5200.0f);

    for (int i = 0; i < n; i++) {
        float x = a[i] - fx_onepole(&f->lo_s, a[i], hp_a);
        x *= gain;
        /* RECTIFY, THEN CLIP. The other order is a fuzz with an octave
         * bolted on and sounds like two pedals. */
        float r = fabsf(x) * 2.0f - 0.35f;
        r = tanhf(r * 1.5f);
        r = fx_bq_run(&band, &f->bq1, r);
        r = fx_onepole(&f->lp_y1, r, lp_a);
        /* Rectification leaves a DC offset that rides the envelope; left
         * in, it moves the whole waveform off centre and eats headroom. */
        float y = r - f->hp_y1;
        f->hp_y1 = flush_denormal(f->hp_y1 + fx_coeff(18.0f) * (r - f->hp_y1));
        a[i] = sanitize_sample(y * vol * 0.40f);
    }
}

/*
 * RANGEMASTER - a treble booster, which is a different machine from a
 * clean boost and a different job.
 *
 * Its coupling cap is tiny, so it passes almost no bass at all: what
 * reaches the amp is the top two octaves, hard. That is why a Marshall
 * stops being woolly with one in front, and why turning a flat 20 dB
 * boost up does NOT do the same thing - a flat boost pushes the mud into
 * the amp along with everything else.
 *
 * The germanium transistor clips asymmetrically on its way past, so the
 * boost is never quite clean, which is the other half of it.
 */
static void fx_rangemaster(fx_block_t *f, float *a, int n) {
    const float g = fx_db2lin(fx_v(f, 0));
    const float corner = fx_v(f, 1);        /* where the bass stops */
    const float hp_a = fx_coeff(corner);
    for (int i = 0; i < n; i++) {
        float x = a[i] - fx_onepole(&f->lo_s, a[i], hp_a);
        x *= g;
        /* Germanium: the positive half turns over first. */
        float h = (x > 0.0f) ? 0.85f : 0.62f;
        a[i] = sanitize_sample(tanhf(x * h) * 1.15f);
    }
}

/*
 * UNIVIBE - not a phaser, and the difference is the whole reason it is
 * here.
 *
 * A phaser's allpass stages are matched and sweep together, which makes
 * evenly spaced notches and that even whoosh. A Univibe's four stages are
 * deliberately MISMATCHED - each sits at a different frequency and sweeps
 * by a different amount - so the notches move at different rates relative
 * to each other and the result throbs rather than sweeps. That, plus the
 * lamp-driven LFO being distinctly non-sinusoidal (a bulb heats faster
 * than it cools), is the sound.
 */
static void fx_univibe(fx_block_t *f, float *a, int n) {
    const float inc = fx_lfo_inc(fx_v(f, 0));
    const float intensity = f->p[1];
    const float mix = fx_v(f, 2) * 0.01f;
    /* The four stages' resting points and travel, mismatched on purpose. */
    static const float BASE[4] = { 0.22f, 0.36f, 0.52f, 0.70f };
    static const float SPAN[4] = { 0.30f, 0.26f, 0.20f, 0.14f };

    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        /* A LAMP, NOT AN OSCILLATOR: it brightens faster than it fades, so
         * the sweep is asymmetric and that asymmetry is audible as the
         * throb. A sine here gives you a phaser. */
        float ph = f->lfo_phase;
        float lfo = (ph < 0.35f) ? (ph / 0.35f)
                                 : (1.0f - (ph - 0.35f) / 0.65f);
        lfo = lfo * lfo * (3.0f - 2.0f * lfo);       /* smoothstep the corner */

        float v = a[i];
        for (int k = 0; k < 4; k++) {
            float d = BASE[k] + SPAN[k] * lfo * intensity;
            float out = -d * v + f->ap[k];
            f->ap[k] = flush_denormal(v + d * out);
            v = out;
        }
        /* The photocell also swings the level a little. */
        /* A Univibe THROBS and a phaser sweeps, and the throb is mostly
         * this: the photocells swing the level as well as the notches.
         * At 0.12 the two were indistinguishable on a held note (measured
         * 3.3% against the phaser's 2.9%); the amplitude term is what
         * separates them and it has to be audible to do that. */
        float am = 1.0f - 0.30f * intensity * lfo;
        a[i] = sanitize_sample(a[i] * (1.0f - mix) + (a[i] + v) * 0.5f * am * mix * 2.0f);
    }
}

/*
 * DIMENSION - a chorus with NO RATE KNOB, because the original has none.
 *
 * Four buttons, and that is the entire control surface. Inside, two BBD
 * lines run slow opposed sweeps and land on OPPOSITE SIDES of the stereo
 * field, so what you hear is width rather than a cycle. It is the answer
 * to "modulation that does not go round and round": there is nothing to
 * hear going round, and the sweep is far too slow and too shallow to
 * announce itself.
 *
 * Stereo, so it writes the side channel the way the doubler does - and for
 * the same reason, it belongs late in the chain.
 */
static void fx_dimension(fx_block_t *f, float *a, float *side, int n) {
    const int mode = (int)fx_v(f, 0);
    const float width = fx_v(f, 1) * 0.01f;
    /* The four buttons are not a depth knob - they step both the amount
     * and the rate together, which is why the pedal has no separate one. */
    static const float DEPTH[4] = { 0.25f, 0.45f, 0.70f, 1.00f };
    static const float MS[4]    = { 9000, 7000, 5200, 3800 };
    const int mi = (mode < 0) ? 0 : (mode > 3) ? 3 : mode;
    const float inc = fx_lfo_inc(MS[mi]);
    const float depth = DEPTH[mi];

    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float s1 = sinf(2.0f * (float)M_PI * f->lfo_phase);
        float s2 = -s1;                        /* opposed, exactly */
        f->line[f->w] = a[i];
        float dA = (0.0085f + 0.0022f * s1 * depth) * SAMPLE_RATE;
        float dB = (0.0110f + 0.0022f * s2 * depth) * SAMPLE_RATE;
        float A = fx_tap(f, dA), B = fx_tap(f, dB);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        /* THE MID GETS LITTLE AND THE SIDE GETS THE REST, because the
         * effect IS the width. Summed to mono, two copies 8.5 and 11 ms
         * behind the dry are a deep comb that the opposed sweep then
         * MOVES - measured at 21% level swing, worse than the chorus this
         * exists to be calmer than. Keeping the sum small leaves the mono
         * fold-down as a gentle thickening and puts the character where
         * it belongs; the real unit behaves the same way, which is why a
         * Dimension largely disappears in mono. */
        a[i]    = sanitize_sample(a[i] + (A + B) * 0.5f * 0.30f);
        side[i] = sanitize_sample(side[i] + (A - B) * 0.5f * (0.35f + width * 0.65f));
    }
}

/* Five bands, fixed, as on the front of a rack EQ - the thing you reach
 * for to fix a room rather than to voice a tone. Five is what fits the
 * encoders; the centres are the ones that matter on a guitar. */
static void fx_grapheq(fx_block_t *f, float *a, int n) {
    static const float HZ[5] = { 100.0f, 400.0f, 800.0f, 2000.0f, 6000.0f };
    fx_bq_t b[5];
    fx_bqs_t *st[5] = { &f->bq1, &f->bq2, &f->bq3, NULL, NULL };
    /* Two more states than the three the struct carries, borrowed from the
     * allpass array - which no EQ uses, so there is nothing to collide
     * with and nothing to allocate. */
    fx_bqs_t extra0, extra1;
    extra0.x1 = f->ap[0]; extra0.x2 = f->ap[1];
    extra0.y1 = f->ap[2]; extra0.y2 = f->ap[3];
    extra1.x1 = f->ap[4]; extra1.x2 = f->ap[5];
    extra1.y1 = f->ap[6]; extra1.y2 = f->ap[7];
    st[3] = &extra0; st[4] = &extra1;

    for (int k = 0; k < 5; k++) fx_bq_peak(&b[k], HZ[k], 1.4f, fx_v(f, k));
    for (int i = 0; i < n; i++) {
        float x = a[i];
        for (int k = 0; k < 5; k++) x = fx_bq_run(&b[k], st[k], x);
        a[i] = sanitize_sample(x);
    }
    f->ap[0] = extra0.x1; f->ap[1] = extra0.x2;
    f->ap[2] = extra0.y1; f->ap[3] = extra0.y2;
    f->ap[4] = extra1.x1; f->ap[5] = extra1.x2;
    f->ap[6] = extra1.y1; f->ap[7] = extra1.y2;
}

/*
 * REVERSE DELAY - the line read backwards, in overlapping windows.
 *
 * The window is the delay time. Two read heads run BACKWARDS through it,
 * half a window apart, crossfaded equal-power so the seam where each one
 * restarts is inaudible: one head is always in the middle of its pass
 * while the other is at an edge. One head alone clicks once per window,
 * every window, forever.
 */
static void fx_reverse(fx_block_t *f, float *a, int n) {
    const float ms = fx_v(f, 0);
    const float fb = f->p[1] * 0.55f;
    const float mix = fx_v(f, 2) * 0.01f;
    float win = ms * 0.001f * SAMPLE_RATE;
    if (win > (float)(f->line_len - 512)) win = (float)(f->line_len - 512);
    if (win < 256.0f) win = 256.0f;

    for (int i = 0; i < n; i++) {
        /* rd counts FORWARD through the window; the tap distance counts
         * backward, which is what plays the buffer in reverse. */
        f->rd += 1.0f;
        while (f->rd >= win) f->rd -= win;
        float t = f->rd / win;
        float d0 = win - f->rd;
        float d1 = d0 + win * 0.5f;
        if (d1 >= win * 2.0f) d1 -= win;
        float y = fx_tap(f, d0 + 64.0f) * sinf((float)M_PI * t)
                + fx_tap(f, d1 + 64.0f) * sinf((float)M_PI * (1.0f - t));
        f->line[f->w] = flush_denormal(a[i] + y * fb);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(a[i] + y * mix);
    }
}

/* ------------------------------------------------------------- dispatch */

/* `side` is the stereo difference channel, zeroed by the caller each block
 * and carried through untouched by everything except the doubler. A pedal
 * placed AFTER the doubler therefore processes the centre only - which is
 * why the doubler belongs last, and is said so in its help. */
/* THE PASSIVE TONE STACK. See the table entry for why it is not FX_EQ.
 *
 * Coefficients recomputed once per BLOCK, like every other pedal here -
 * four biquad designs is ~2 us and the knobs are not audio.
 *
 * Every number below was measured with numpy before this was written, and
 * tests/test_fx.cpp re-measures the four that matter on the real build. */
typedef struct { float fb, fm, qm, ft, mid_min; } ts_amp_t;
static const ts_amp_t TS_AMPS[4] = {
    {  90.0f, 400.0f, 0.55f, 1200.0f, -20.0f },   /* Fender: low, broad   */
    { 110.0f, 500.0f, 0.65f, 1500.0f, -17.0f },   /* Marshall             */
    { 130.0f, 800.0f, 0.90f, 2000.0f,  -9.0f },   /* Vox: barely a notch  */
    { 110.0f, 650.0f, 0.50f, 1800.0f, -22.0f },   /* Modern: deep scoop   */
};
/* The stack's own loss at noon, so (5,5,5) is unity and every other
 * setting is quieter or louder than it the way the circuit is. */
static const float TS_MAKEUP[4] = { 4.7f, 4.1f, 2.8f, 5.2f };

#define TS_SPAN      21.0f    /* a control's full travel, in dB     */
#define TS_MID_MAX    4.0f
#define TS_BASS_FILL  4.0f    /* how much Bass fills the mid scoop  */
#define TS_PRES_DB    9.0f

static void fx_tonestack(fx_block_t *f, float *a, int n) {
    int amp = (int)(f->p[0] * 3.0f + 0.5f);
    if (amp < 0) amp = 0;
    if (amp > 3) amp = 3;
    const ts_amp_t *T = &TS_AMPS[amp];
    const float b = f->p[1], m = f->p[2], t = f->p[3], pr = f->p[4];

    fx_bq_t lo, mid, hi, pres;
    fx_bq_shelf(&lo, T->fb, -TS_SPAN * 0.72f + b * TS_SPAN, 0);
    /* BASS FILLS THE SCOOP. In the circuit the bass and mid pots share a
     * leg, so they are not two independent bands however much the panel
     * suggests it - and that coupling is most of why a stack does not
     * sound like an EQ set to the same curve. */
    fx_bq_peak(&mid, T->fm, T->qm,
               T->mid_min * (1.0f - m) + TS_MID_MAX * m + TS_BASS_FILL * b);
    fx_bq_shelf(&hi, T->ft, -TS_SPAN * 0.72f + t * TS_SPAN, 1);
    const int use_pres = (pr > 0.001f);
    /* PRESENCE IS NOT PART OF THE STACK. It lives in the power amp's
     * feedback loop, which is why it is one-sided - a real one boosts and
     * does not cut. */
    if (use_pres) fx_bq_shelf(&pres, 3500.0f, pr * TS_PRES_DB, 1);

    const float mk = powf(10.0f, TS_MAKEUP[amp] / 20.0f);
    for (int i = 0; i < n; i++) {
        float x = fx_bq_run(&lo,  &f->bq1, a[i]);
        x = fx_bq_run(&mid, &f->bq2, x);
        x = fx_bq_run(&hi,  &f->bq3, x);
        if (use_pres) x = fx_bq_run(&pres, &f->bq4, x);
        a[i] = x * mk;
    }
}

/* THE POWER STAGE'S MEMORY. See the table entry for why a capture has
 * none of it.
 *
 * Two envelopes, deliberately at different speeds, because the two
 * effects are different physics at different time scales:
 *
 *  the RAIL follows current over tens of ms and refills over hundreds
 *  the BIAS walks in a couple of ms and comes back in fifty
 *
 * One envelope driving both would make the sag spit and the bias breathe.
 */
static void fx_poweramp(fx_block_t *f, float *a, int n) {
    const float sag   = f->p[0];
    const float bias  = f->p[2];
    if (sag < 0.001f && bias < 0.001f) return;

    /* 50..600 ms of reservoir. The ATTACK is fixed at ~45 ms: how fast a
     * rail falls is the supply's, not a knob of the player's; what they
     * are choosing is how long it takes to come back.
     *
     * 45 and not the 12 it was first written with. A reservoir cap takes
     * tens of milliseconds to discharge, and at 12 ms the droop was over
     * before the first window of a note had finished - which measures as
     * almost no sag at all AND is what a compressor does, not a supply. */
    const float bloom_ms = 50.0f + f->p[1] * 550.0f;
    const float a_up   = 1.0f - expf(-1.0f / (0.045f * SAMPLE_RATE));
    const float a_dn   = 1.0f - expf(-1.0f / (bloom_ms * 0.001f * SAMPLE_RATE));
    const float b_up   = 1.0f - expf(-1.0f / (0.002f * SAMPLE_RATE));
    const float b_dn   = 1.0f - expf(-1.0f / (0.060f * SAMPLE_RATE));

    const float k = sag * 3.0f;
    for (int i = 0; i < n; i++) {
        float x = a[i];
        const float mag = x < 0.0f ? -x : x;

        /* LEVEL DEPENDENT, which is the whole point: a quiet passage must
         * not duck at all, or this is a compressor with an amp's name. */
        f->env = (mag > f->env) ? f->env + (mag - f->env) * a_up
                                : f->env + (mag - f->env) * a_dn;
        if (k > 0.0f) x *= 1.0f / (1.0f + k * f->env);

        if (bias > 0.001f) {
            f->env2 = (mag > f->env2) ? f->env2 + (mag - f->env2) * b_up
                                      : f->env2 + (mag - f->env2) * b_dn;
            /* ASYMMETRIC, because the bias walks one way. A symmetric
             * squash is just more distortion; the odd/even split is what
             * makes it read as a valve letting go rather than as gain. */
            const float exc = bias * f->env2 * 8.0f;
            if (x < 0.0f) x = x / (1.0f + exc * (-x));
        }
        a[i] = flush_denormal(x);
    }
}

static void fx_block_process(fx_block_t *f, float *a, float *side, int n) {
    switch (f->id) {
        case FX_TS808: case FX_TS9: case FX_SD1:
        case FX_BLUESBRK: case FX_CENTAUR:
        case FX_RAT: case FX_DS1: case FX_DISTPLUS:
        case FX_FULLBORE: case FX_SUPERBAD:
        case FX_BIGMUFF: case FX_FUZZFACE:      fx_gain_stage(f, a, n); break;
        case FX_BOOST:                          fx_boost(f, a, n); break;
        case FX_C1176: case FX_DYNACOMP:
        case FX_SPCOMP: case FX_ROSSCOMP:       fx_comp(f, a, n); break;
        case FX_STARGATE:                       fx_stargate(f, a, n); break;
        case FX_LIMITER:                        fx_limiter(f, a, n); break;
        case FX_EQ:                             fx_eq(f, a, n); break;
        case FX_AUTOWAH:                        fx_autowah(f, a, n); break;
        case FX_WAH:                            fx_wah(f, a, n); break;
        case FX_LOFI:                           fx_lofi(f, a, n); break;
        case FX_CHORUS:                         fx_chorus(f, a, n); break;
        case FX_FLANGER:                        fx_flanger(f, a, n); break;
        case FX_PHASER: case FX_PHASE90:        fx_phaser(f, a, n); break;
        case FX_VIBRATO:                        fx_vibrato(f, a, n); break;
        case FX_TREMOLO:                        fx_tremolo(f, a, n); break;
        case FX_ROTARY:                         fx_rotary(f, a, n); break;
        case FX_DELAY: case FX_ANALOGDLY:       fx_delay(f, a, n); break;
        case FX_DUALDELAY:                      fx_dual_delay(f, a, n); break;
        case FX_TAPE:                           fx_tape(f, a, n); break;
        case FX_REVERB: case FX_PLATE:          fx_reverb(f, a, n); break;
        case FX_BLUESDRV: case FX_PLEXI:
        case FX_METALZONE:                      fx_gain_stage(f, a, n); break;
        case FX_OCTAVIA:                        fx_octavia(f, a, n); break;
        case FX_RANGEMASTER:                    fx_rangemaster(f, a, n); break;
        case FX_UNIVIBE:                        fx_univibe(f, a, n); break;
        case FX_DIMENSION:                      fx_dimension(f, a, side, n); break;
        case FX_GRAPHEQ:                        fx_grapheq(f, a, n); break;
        case FX_REVERSE:                        fx_reverse(f, a, n); break;
        case FX_SPRING:                         fx_spring(f, a, n); break;
        case FX_DOUBLER:                        fx_doubler(f, a, side, n); break;
        case FX_DETUNE:                         fx_detune(f, a, n); break;
        case FX_OCTAVE:                         fx_octave(f, a, n); break;
        case FX_RINGMOD:                        fx_ringmod(f, a, n); break;
        case FX_TONESTACK:                      fx_tonestack(f, a, n); break;
        case FX_TUNING:                         fx_tuning(f, a, n); break;
        case FX_POWERAMP:                       fx_poweramp(f, a, n); break;
        default: break;
    }
}

#endif /* A2_FX_H */

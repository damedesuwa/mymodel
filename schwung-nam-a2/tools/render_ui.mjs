/*
 * DRAW ui_chain.js INTO A FRAMEBUFFER AND PRINT IT.
 *
 * Every layout decision in this module has so far been made by reasoning
 * about coordinates and then shipping it to a device to look at, which is
 * three minutes a round and has now been wrong four times running. The
 * screen is 128x64 of one-bit pixels and the drawing calls are four
 * primitives; there is no reason not to just look at it here.
 *
 *   node tools/render_ui.mjs [scenario]
 */
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const src = fs.readFileSync(path.join(root, 'src/c/ui_chain.js'), 'utf8')
    .replace(/^import .*$/m, '');
const specsRaw = fs.readFileSync(path.join(root, 'tests/fixtures/fx_specs.json'), 'utf8').trim();
const FX = JSON.parse(specsRaw).map(p => p.n);

/* A 5x7 font is close enough to Move's for layout: what matters here is
 * whether things collide and whether the picture reads, not the glyphs. */
const CW = 5, CH = 7;
const W = 128, H = 64;
const fb = [];
const clearFb = () => { for (let y = 0; y < H; y++) fb[y] = new Array(W).fill(0); };
const px = (x, y, c) => {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    fb[y][x] = c;
};

const scen = process.argv[2] || 'split';
const params = { sel_block: '2', cpu: '41', build: 'r', peak: '55',
    model_list: JSON.stringify(['OCD', 'Recto']),
    cab_list: JSON.stringify(['TF MESA']),
    fx_list: JSON.stringify(FX), fx_specs: specsRaw,
    in_level: '0.5', out_level: '0.85', split: '0', pan_a: '-1.0', pan_b: '1.0' };
for (let b = 1; b <= 8; b++) {
    params[`b${b}_type`] = '0'; params[`b${b}_on`] = '0'; params[`b${b}_fx`] = '0';
    params[`b${b}_cpu`] = '0'; params[`b${b}_model`] = '0'; params[`b${b}_cab`] = '0';
    params[`b${b}_quality`] = '0'; params[`b${b}_lane`] = String(b & 1);
    for (let k = 1; k <= 5; k++) params[`b${b}_p${k}`] = '0.5';
}
const set = (b, type, fx, on) => {
    params[`b${b}_type`] = String(type);
    if (fx !== undefined) params[`b${b}_fx`] = String(fx);
    params[`b${b}_on`] = on === false ? '1' : '0';
};
if (scen === 'empty') { /* nothing loaded */ }
if (scen === 'series') { set(1, 3, 0); set(2, 1); set(3, 2); set(4, 3, 30); }
if (scen === 'bypass') { set(1, 3, 0); set(2, 1); set(3, 2); set(4, 3, 30, false); set(5, 1, undefined, false); }
if (scen === 'split') {
    set(1, 3, 0); set(2, 1); set(3, 2); set(4, 2); set(5, 3, 34);
    params.split = '3';
    params.b3_lane = '0'; params.b4_lane = '1'; params.b5_lane = '1';
}

const host = {
    host_module_get_param: k => (k in params ? params[k] : null),
    host_module_set_param: (k, v) => { params[k] = String(v); },
    host_pad_block: () => {},
    clear_screen: () => clearFb(),
    print: (x, y, t, ink) => {
        let cx = x;
        for (const ch of String(t)) {
            /* A filled 4x7 cell per glyph with a 1px gutter: the point is
             * the box it occupies, not which letter it is. */
            for (let dy = 0; dy < CH; dy++)
                for (let dx = 0; dx < CW - 1; dx++) px(cx + dx, y + dy, ink === 0 ? 0 : 1);
            cx += CW;
        }
    },
    draw_rect: (x, y, w, h, c) => {
        for (let i = 0; i < w; i++) { px(x + i, y, c); px(x + i, y + h - 1, c); }
        for (let j = 0; j < h; j++) { px(x, y + j, c); px(x + w - 1, y + j, c); }
    },
    fill_rect: (x, y, w, h, c) => {
        for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) px(x + i, y + j, c);
    },
    text_width: t => String(t).length * CW,
    console: { log: () => {} },
};
const stubs = `const setLED=()=>{},setButtonLED=()=>{},
  decodeDelta=v=>(v===0?0:(v<=63?v:-(128-v))),invalidateLedCache=()=>{};`;
const names = Object.keys(host);
const ui = new Function('globalThis', ...names,
    stubs + src + '\nreturn globalThis.chain_ui;')({}, ...names.map(n => host[n]));

clearFb();
ui.init();
let skew = 0; const real = Date.now; Date.now = () => real() + skew;
for (let i = 0; i < 6; i++) { skew += 50; ui.tick(); }

/* Two rows of pixels per character line, so 64 rows fit a terminal. */
const BLOCKS = [' ', '▀', '▄', '█'];
console.log('    +' + '-'.repeat(W) + '+   ' + scen);
for (let y = 0; y < H; y += 2) {
    let line = '';
    for (let x = 0; x < W; x++) line += BLOCKS[(fb[y][x] ? 1 : 0) | (fb[y + 1][x] ? 2 : 0)];
    console.log(String(y).padStart(3) + ' |' + line + '|');
}
console.log('    +' + '-'.repeat(W) + '+');

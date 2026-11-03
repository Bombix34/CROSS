// Node mirror of cross/cross_dsp.c, used to listen to CROSS on a PC before
// the Blackfin build exists. It follows the C line by line with the same
// integer formats and tables, so the sound matches the device engine.
// Usage: node tools/gen_tables.mjs && node tools/preview.mjs
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const T = JSON.parse(readFileSync(join(root, 'tools', 'tables.json'), 'utf8'));
// FS: the voices' rate on the OP-1 (128 samples a 1.45 ms block); the demos are halved to OUT_FS, as the OS does.
const FS = 88200, OUT_FS = 44100, BLOCK = 128, STEP = 16;
const ENV_ONE = 1 << 23, ENV_TARGET = 10905190, OCT_MAX = (160 << 12) - 1;
const F_SUB = 1, F_DECAY = 2, F_STILL = 4;

const clamp = (x, lo, hi) => (x < lo ? lo : x > hi ? hi : x);
const knobLut = (tab, p) => tab[p >> 8] + (((tab[(p >> 8) + 1] - tab[p >> 8]) * (p & 255)) >> 8);

function sat(x) {
  x = clamp(x, -32768, 32767);
  const i = x + 32768, idx = i >> 8, fr = i & 255, a = T.tanh[idx];
  return a + (((T.tanh[idx + 1] - a) * fr) >> 8);
}
function log2q16(x) {
  const n = 31 - Math.clz32(x);
  const m = (x << (31 - n)) >>> 0;
  const i = (m >>> 25) & 63, fr = (m >>> 17) & 255;
  return (n << 16) + T.log2[i] + (((T.log2[i + 1] - T.log2[i]) * fr) >> 8);
}
function cutoffCoef(oct) {
  oct = clamp(oct, 0, OCT_MAX);
  const idx = oct >> 12, fr = (oct >> 4) & 255, a = T.gcoef[idx];
  return a + (((T.gcoef[idx + 1] - a) * fr) >> 8);
}
const INC_MIN = 4096, INC_MAX = 0x7f000000;
// inc * (1 + up / 65536), the C's 32-bit split
const raise = (inc, up) => (inc + (inc >>> 16) * up + (((inc & 0xffff) * (up >>> 1)) >>> 15)) >>> 0;
function blepSetup(inc) {
  const sh = Math.max(0, 31 - Math.clz32(inc) - 14), incs = inc >>> sh;
  return { sh, incs, rdt: Math.floor(2 ** 30 / incs) };
}
function blepX(ph, bw) {
  const q = ph >>> bw.sh;
  return q >= bw.incs ? 32768 : (q * bw.rdt) >>> 15;
}
// the step correction for a phase within one increment of a wrap (nearWrap holds)
function blep(ph, inc, bw) {
  if (ph < inc) {
    const x = blepX(ph, bw);
    return 2 * x - ((x * x) >> 15) - 32768;
  }
  const x = blepX((0x100000000 - ph) >>> 0, bw);
  return ((x * x) >> 15) - 2 * x + 32768;
}
const nearWrap = (ph, inc, inc2) => ((ph + inc) >>> 0) < inc2;
const saw = (ph, inc, inc2, bw) => (ph >>> 16) - 32768 - (nearWrap(ph, inc, inc2) ? blep(ph, inc, bw) : 0);

const newVoice = () => ({ ph1: 0, ph2: 0x40000000, lp: [0, 0, 0, 0], env: 0, flags: 0, rng: 0, wand: [0, 0, 0, 0], nudge: [0, 0] });
// analogue drift, as the C: LCG noise, each VCO's slow wander (two poles) and a small offset drawn per note (Q24 ratio)
const noise = v => (v.rng = (Math.imul(v.rng, 1664525) + 1013904223) >>> 0) | 0;
const drift = (inc, d) => (inc + ((((inc >>> 15) * d) | 0) >> 9)) >>> 0;
function noteOn(v) {
  v.flags &= F_SUB | F_STILL;
  v.nudge = [noise(v) >> 19, noise(v) >> 19];
  if (v.env > ENV_ONE) v.env = 0;
  v.lp = v.lp.map(x => clamp(x, -32767, 32767));
}

// incs: VCO1 increment per sample, as OP1_PITCH_INC gives it (read once per 16 samples). Three passes, as the C:
// contour and levels per 16 samples, the oscillators into the buffer, the ladder in place. The C's branch-free loop
// for steps clear of any wrap gives the same samples as this one.
function render(v, out, off, n, incs, w, gain) {
  let { ph1, ph2, env, flags } = v;
  let [l0, l1, l2, l3] = v.lp;
  const inc0 = clamp(incs[0] >>> 0, INC_MIN, INC_MAX);
  const up = T.detune[(w[6] >>> 8) & 127] - 65536;
  const bw1 = blepSetup(inc0), bw2 = blepSetup(raise(inc0, up));
  let d1 = 0, d2 = 0;
  if (!(v.flags & F_STILL)) {
    const a = v.wand;
    a[0] += ((noise(v) >> 14) * 3 - a[0]) >> 8;
    a[1] += (a[0] - a[1]) >> 8;
    a[2] += ((noise(v) >> 14) * 3 - a[2]) >> 8;
    a[3] += (a[2] - a[3]) >> 8;
    d1 = clamp(a[1] + v.nudge[0], -32767, 32767);
    d2 = clamp(a[3] + v.nudge[1], -32767, 32767);
  }

  let p = clamp(w[5], 0, 32767), m, sub, pw = 0x80000000;
  if (p < 10923) { m = 0; sub = p * 3; }
  else if (p < 21845) { m = (p - 10923) * 3; sub = 32767; }
  else { const t = (p - 21845) * 3; m = 32767; pw = (0x80000000 - t * 49807) >>> 0; sub = 32767 - (t >> 1); }
  m = clamp(m, 0, 32767);
  const m9 = (m * 29491) >> 15;
  sub = (clamp(sub, 0, 32767) * 29491) >> 15;

  gain = clamp(gain, 0, 32767);
  const base = clamp(w[0], 0, 32767) * 19;
  // velocity opens the filter too: 30 % of ENV for the softest note, all of it at full velocity
  const amt = ((((clamp(w[2], 0, 32767) * 12) >> 4) * (9830 + ((gain * 22937) >> 15))) >> 15) << 4;
  const d = clamp(log2q16(inc0) - T.LOG2_REF, -(8 << 16), 8 << 16);
  const key = ((d >> 4) * 43254) >> 12;
  p = clamp(w[1], 0, 32767);
  const pq = (p * (p + 32768)) >>> 16, k = (pq * 16000) >> 15, comp = 4096 + ((3 * k) >> 2);
  p = clamp(w[3], 0, 32767);
  const pre = knobLut(T.drivePre, p), post = knobLut(T.drivePost, p), outg = knobLut(T.driveOut, p);
  const punch = (outg * knobLut(T.drivePunch, p)) >> 15;
  const dc = knobLut(T.decay, clamp(w[7], 0, 32767));

  // every 16 samples: contour, cutoff, output level (velocity folded in)
  const steps = n / STEP, gs = [], ogs = [];
  for (let st = 0; st < steps; st++) {
    if (!(flags & F_DECAY)) {
      env += (((ENV_TARGET - env) >>> 8) * T.ATTACK) >>> 8;
      if (env >= ENV_ONE) { env = ENV_ONE; flags |= F_DECAY; }
    } else {
      env -= ((env >>> 8) * dc) >>> 8;
    }
    const e15 = env >>> 8;
    gs.push(cutoffCoef(base + key + (((e15 >> 3) * (amt >> 4)) >> 8)));
    ogs.push(((outg + ((punch * e15) >> 15)) * gain) >> 15);
  }

  // oscillators and mixer overload
  let sq = flags & F_SUB ? -32768 : 32767, sv = (sq * sub) >> 15;
  for (let st = 0; st < steps; st++) {
    const i0 = clamp(incs[st * STEP] >>> 0, INC_MIN, INC_MAX), i1 = drift(i0, d1), i2 = drift(raise(i0, up), d2);
    const w1 = 2 * i1, w2 = i2 < 0x80000000 ? 2 * i2 : 0xffffffff;
    for (let i = 0; i < STEP; i++) {
      let s1 = (ph1 >>> 16) - 32768, sb = sv;
      if (nearWrap(ph1, i1, w1)) {
        const b1 = blep(ph1, i1, bw1);
        s1 -= b1;
        sb = ((sq + ((ph1 < i1) === (sq < 0) ? -b1 : b1)) * sub) >> 15;
      }
      let mix = s1 + ((saw(ph2, i2, w2, bw2) * 29491) >> 15) + sb, x;
      if (m) {
        mix -= (m * saw((ph1 + pw) >>> 0, i1, w1, bw1) + m9 * saw((ph2 + pw) >>> 0, i2, w2, bw2)) >> 15;
        x = sat(((mix >> 3) * pre) >> 13);
      } else {
        x = sat(((mix >> 2) * pre) >> 14);
      }
      out[off + st * STEP + i] = x;
      const next = (ph1 + i1) >>> 0;
      if (next < ph1) { sq = ~sq; sv = (sq * sub) >> 15; }
      ph1 = next;
      ph2 = (ph2 + i2) >>> 0;
    }
  }
  flags = (flags & ~F_SUB) | (sq < 0 ? F_SUB : 0);

  // ladder, post drive and level, in place
  for (let st = 0; st < steps; st++) {
    const g = gs[st], og = ogs[st];
    for (let i = 0; i < STEP; i++) {
      const j = off + st * STEP + i;
      const u = sat((((out[j] * comp) >> 12) - ((k * l3) >> 12)) >> 2);
      l0 += (g * u - g * l0 + 16384) >> 15;
      l1 += (g * l0 - g * l1 + 16384) >> 15;
      l2 += (g * l1 - g * l2 + 16384) >> 15;
      l3 += (g * l2 - g * l3 + 16384) >> 15;
      const z = post ? sat((l3 * post) >> 10) : l3;
      out[j] = (z * og) >> 15;
    }
  }
  Object.assign(v, { ph1, ph2, env, flags, lp: [l0, l1, l2, l3] });
}

// ---- demo sequencer: stands in for the OP-1 keyboard and its T2 envelope ----
const BPM = 122, STEP_S = 60 / BPM / 4;
const _ = null;
const BAR_A = [33, _, 45, 33, _, 33, 45, _, 31, _, 43, 31, _, 31, 43, _];
const BAR_B = [29, _, 41, 29, _, 29, 41, _, 31, _, 43, 31, 36, _, 34, 31];
const PATTERN = [...BAR_A, ...BAR_B, ...BAR_A, ...BAR_B];
const midiInc = n => Math.round(440 * 2 ** ((n - 69) / 12) / FS * 2 ** 32);

function play(name, words, opts = {}) {
  const loops = opts.loops ?? 2;
  const steps = [];
  for (let l = 0; l < loops; l++) steps.push(...PATTERN);
  const total = Math.ceil((steps.length * STEP_S + 0.5) * FS / BLOCK) * BLOCK;
  const dry = new Int16Array(total), out = new Int16Array(total);
  const v = newVoice();
  const events = [];
  steps.forEach((note, i) => {
    if (note === null) return;
    let len = 1;
    while (i + len < steps.length && steps[i + len] === null) len++;
    events.push({ on: Math.round(i * STEP_S * FS), off: Math.round((i + len * 0.8) * STEP_S * FS), inc: midiInc(note) });
  });

  // Stock-style amplitude envelope: 2 ms attack, full sustain, 40 ms release.
  let amp = 0, gate = false, inc = events[0].inc, ev = 0;
  const atk = 1 / (0.002 * FS), rel = Math.exp(-1 / (0.04 * FS));
  for (let b = 0; b < total; b += BLOCK) {
    while (ev < events.length && events[ev].on < b + BLOCK) {
      inc = events[ev].inc; gate = true; noteOn(v); ev++;
    }
    const w = typeof words === 'function' ? words(b / total) : words;
    render(v, dry, b, BLOCK, new Int32Array(BLOCK).fill(inc), w, 32767);
    for (let i = b; i < b + BLOCK; i++) {
      const cur = events[ev - 1];
      if (cur && i >= cur.off && (ev >= events.length || i < events[ev].on)) gate = false;
      amp = gate ? Math.min(1, amp + atk) : amp * rel;
      out[i] = Math.round(dry[i] * amp);
    }
  }
  const half = decimate(out);
  writeWav(join(root, 'demos', `${name}.wav`), half);
  report(name, half);
}

// 88.2 kHz -> 44.1 kHz. The OS's own decimator is not documented: a 63-tap half-band (Blackman-windowed sinc) stands
// in for it.
const HB = (() => {
  const h = Array.from({ length: 63 }, (_, i) => {
    const t = i - 31, w = 0.42 - 0.5 * Math.cos(Math.PI * i / 31) + 0.08 * Math.cos(2 * Math.PI * i / 31);
    return (t ? Math.sin(Math.PI * t / 2) / (Math.PI * t) : 0.5) * w;
  });
  const sum = h.reduce((a, c) => a + c, 0);
  return h.map(c => c / sum);
})();
function decimate(x) {
  const y = new Int16Array(x.length >> 1);
  for (let j = 0; j < y.length; j++) {
    let s = 0;
    for (let k = 0; k < HB.length; k++) {
      const i = 2 * j + k - 31;
      if (i >= 0 && i < x.length) s += HB[k] * x[i];
    }
    y[j] = clamp(Math.round(s), -32768, 32767);
  }
  return y;
}

function report(name, x) {
  let peak = 0, sum = 0, dc = 0, clip = 0;
  for (const s of x) { peak = Math.max(peak, Math.abs(s)); sum += s * s; dc += s; if (Math.abs(s) >= 32767) clip++; }
  const db = v => (20 * Math.log10(v / 32767)).toFixed(1);
  console.log(`${name.padEnd(18)} peak ${db(peak)} dBFS  rms ${db(Math.sqrt(sum / x.length))} dBFS  dc ${(dc / x.length).toFixed(1)}  clipped ${clip}`);
}

function writeWav(path, x) {
  const buf = Buffer.alloc(44 + x.length * 2);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + x.length * 2, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(OUT_FS, 24); buf.writeUInt32LE(OUT_FS * 2, 28); buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(x.length * 2, 40);
  x.forEach((s, i) => buf.writeInt16LE(s, 44 + i * 2));
  writeFileSync(path, buf);
}

export { newVoice, noteOn, render, midiInc, FS, F_STILL };

// ---- presets: [CUTOFF, RESONANCE, ENV, DRIVE, engine, WAVE, DETUNE, DECAY] ----
const DEFAULTS = [11450, 13000, 19115, 11500, 0, 10923, 13900, 22400];
// Demos only when run directly, not when imported by tools/check.mjs.
if (process.argv[1] === fileURLToPath(import.meta.url)) {
mkdirSync(join(root, 'demos'), { recursive: true });
play('1-default', DEFAULTS);
play('2-pluck', [6000, 20000, 26000, 9000, 0, 10923, 9000, 15000]);
play('3-drive', [9000, 18000, 22000, 29500, 0, 21000, 6000, 20000]);
play('4-pulse-funk', [8000, 16000, 24000, 14000, 0, 30000, 4000, 17000]);
play('5-sweep', t => [Math.round(3000 + 20000 * (0.5 - 0.5 * Math.cos(2 * Math.PI * t))), 20000, 9000, 13000, 0, 10923, 13900, 22400], { loops: 4 });
play('6-resonance-max', [12000, 32767, 20000, 11500, 0, 10923, 13900, 22400]);
play('7-drive-max', [9000, 18000, 22000, 32767, 0, 21000, 6000, 20000]);
}

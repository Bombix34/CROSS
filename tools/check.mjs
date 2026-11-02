// Measurement checks for the CROSS engine, run on the Node mirror.
// Usage: node tools/check.mjs
import { newVoice, noteOn, render, midiInc, FS, F_STILL } from './preview.mjs';

const BLOCK = 128;
const CLEAN = [32767, 0, 0, 0, 0, 0, 0, 22400]; // filter open, no resonance, envelope or drive

function steady(note, words, seconds = 1.5) {
  const n = Math.ceil(seconds * FS / BLOCK) * BLOCK, out = new Int16Array(n), v = newVoice();
  v.flags |= F_STILL; // exact pitches for the measurements below; the fuzz keeps the drift
  noteOn(v);
  for (let b = 0; b < n; b += BLOCK) render(v, out, b, BLOCK, new Int32Array(BLOCK).fill(midiInc(note)), words, 32767);
  return out.subarray(n - FS); // last second, past the attack
}
function power(x, f) { // Goertzel
  const c = 2 * Math.cos(2 * Math.PI * f / FS);
  let s1 = 0, s2 = 0;
  for (const s of x) { const s0 = s + c * s1 - s2; s2 = s1; s1 = s0; }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / (x.length * x.length / 4);
}
const rms = x => Math.sqrt(x.reduce((a, s) => a + s * s, 0) / x.length);
const db = r => (10 * Math.log10(r)).toFixed(1);
let failed = 0;
const check = (name, ok, detail) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${detail}`); if (!ok) failed++; };

// 1. The sub sits one octave below VCO1.
{
  const f = 55;
  const saw = steady(33, CLEAN), withSub = steady(33, [...CLEAN.slice(0, 5), 10923, 0, 22400]);
  const r = power(withSub, f / 2) / power(saw, f / 2);
  check('sub at -1 octave', r > 1000, `power at 27.5 Hz rises ${db(r)} dB with SAW+SUB`);
}

// 2. Aliasing of a high saw stays low (polyBLEP).
{
  const f = 1760, x = steady(93, CLEAN);
  let harm = 0;
  for (let h = 1; h * f < FS / 2; h++) harm += power(x, h * f);
  const total = 2 * rms(x) ** 2;
  check('aliasing at 1760 Hz', 1 - harm / total < 0.01, `inharmonic energy ${db(Math.max(1e-9, 1 - harm / total))} dB of total`);
}

// 3. CUTOFF darkens monotonically as it closes.
{
  const levels = [32767, 20000, 12000, 6000, 1000].map(c => rms(steady(33, [c, 0, 0, 0, 0, 0, 0, 22400])));
  const mono = levels.every((l, i) => i === 0 || l <= levels[i - 1] * 1.02);
  check('cutoff sweep', mono, levels.map(l => l.toFixed(0)).join(' > '));
}

// 4. Bass survives full resonance (compensation) and the loop stays bounded.
{
  const low = rms(steady(33, [8000, 0, 0, 0, 0, 0, 0, 22400]));
  const res = steady(33, [8000, 32767, 0, 0, 0, 0, 0, 22400]);
  const ratio = rms(res) / low;
  const peak = res.reduce((a, s) => Math.max(a, Math.abs(s)), 0);
  check('resonance keeps the bass', ratio > 0.5 && peak < 32767, `level x${ratio.toFixed(2)} vs no resonance, peak ${peak}`);
}

// 5. Fuzz: random knobs, notes and retriggers never clip or go silent forever.
{
  let seed = 1;
  const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 2 ** 32;
  const v = newVoice(), out = new Int16Array(BLOCK);
  let clipped = 0, bad = 0;
  for (let b = 0; b < 20000; b++) {
    if (b % 40 === 0) noteOn(v);
    const w = Array.from({ length: 8 }, () => Math.floor(rnd() * 32768));
    // pitch per sample: steady, or gliding / jumping inside the block like a legato glide
    const a = midiInc(24 + Math.floor(rnd() * 60)), z = rnd() < 0.3 ? midiInc(24 + Math.floor(rnd() * 60)) : a;
    const incs = Int32Array.from({ length: BLOCK }, (_, i) => Math.round(a + (z - a) * i / BLOCK));
    render(v, out, 0, BLOCK, incs, w, 32767);
    for (const s of out) { if (Math.abs(s) >= 32767) clipped++; if (!Number.isInteger(s)) bad++; }
  }
  check('fuzz 20000 blocks', clipped === 0 && bad === 0, `${clipped} clipped, ${bad} invalid samples`);
}

// 6. Velocity opens the filter too: a soft note is darker, not only quieter (first 100 ms, the envelope's sweep).
{
  const bright = gain => {
    const n = Math.ceil(0.1 * FS / BLOCK) * BLOCK, out = new Int16Array(n), v = newVoice();
    noteOn(v);
    for (let b = 0; b < n; b += BLOCK) render(v, out, b, BLOCK, new Int32Array(BLOCK).fill(midiInc(33)), [4000, 0, 32767, 0, 0, 0, 0, 20000], gain);
    let hi = 0;
    for (let h = 8; h <= 20; h++) hi += power(out, 55 * h);
    return hi / power(out, 55);   // upper harmonics against the fundamental: level-free
  };
  const full = bright(32767), soft = bright(64 * 258);
  check('velocity darkens', soft < full / 4, `harmonics 8-20 vs fundamental: ${db(full)} dB at velocity 127, ${db(soft)} dB at 64`);
}

// 7. The analogue drift stays subtle: each VCO's pitch offset over 5 minutes of notes, from the voice's state.
{
  const v = newVoice(), out = new Int16Array(BLOCK), inc = new Int32Array(BLOCK).fill(midiInc(33));
  const cents = (a, n) => (a + n) / 2 ** 24 * 1731.23;
  let s2 = 0, peak = 0, gap = 0, n = 0;
  for (let b = 0; b < 344 * 300; b++) {
    if (b % 86 === 0) noteOn(v);
    render(v, out, 0, BLOCK, inc, [11450, 13000, 19115, 11500, 0, 10923, 13900, 22400], 32767);
    const c1 = cents(v.wand[1], v.nudge[0]), c2 = cents(v.wand[3], v.nudge[1]);
    s2 += c1 * c1; peak = Math.max(peak, Math.abs(c1)); gap = Math.max(gap, Math.abs(c1 - c2)); n++;
  }
  const rms = Math.sqrt(s2 / n);
  check('drift subtle', rms > 0.3 && rms < 1.2 && peak < 3.5, `VCO1 ${rms.toFixed(2)} cents RMS, ${peak.toFixed(2)} at most; VCOs up to ${gap.toFixed(2)} cents apart`);
}

process.exitCode = failed ? 1 : 0;

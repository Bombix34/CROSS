// Writes cross/cross.svg: empty, as blot's in the SDK; everything on the screen is drawn live by cross.c.
// Native subset only: svg, line, circle, path (M/L/Z), explicit paint, <= 4 decimals, no transforms.
// Usage: node tools/gen_svg.mjs
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const parts = ['<!-- cross: everything is drawn by cross.c -->'];
const svg = `<svg version="1.1" xmlns="http://www.w3.org/2000/svg" width="320px" height="160px" viewBox="0 0 320 160">\n${parts.join('\n')}\n</svg>\n`;
writeFileSync(join(root, 'cross', 'cross.svg'), svg);

// subset check
const tags = [...svg.matchAll(/<([a-z]+)/g)].map(m => m[1]);
const bad = tags.filter(t => !['svg', 'line', 'circle', 'path', 'g', 'polyline'].includes(t));
const decimals = [...svg.matchAll(/\d+\.(\d+)/g)].filter(m => m[1].length > 4);
if (bad.length || decimals.length || /transform|style=|opacity/.test(svg)) {
  console.error('cross.svg breaks the native subset', bad, decimals.map(m => m[0]));
  process.exit(1);
}
console.log(`cross.svg written, ${svg.length} bytes, ${tags.length - 1} elements`);

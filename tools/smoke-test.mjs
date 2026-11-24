// End-to-end check of the bridge in --test mode: connects like the TV app,
// streams for a few seconds and verifies the fMP4 output decodes with ffmpeg.
// Usage: node tools/smoke-test.mjs <bridge-binary> <test.h264>
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const [bridgeBin, testFile] = process.argv.slice(2);
const port = 18090;
const bridge = spawn(bridgeBin, ['--test', testFile, '--port', String(port)], { stdio: 'inherit' });
const fail = (msg) => { console.error('FAIL: ' + msg); bridge.kill(); process.exit(1); };
await new Promise((r) => setTimeout(r, 500));

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
ws.binaryType = 'arraybuffer';
const chunks = [];
const texts = [];
let inits = 0, frags = 0, audioBytes = 0;
ws.onerror = () => fail('cannot connect to bridge');
ws.onopen = () => {
  ws.send(JSON.stringify({ t: 'hello' }));
  ws.send(JSON.stringify({ t: 'apps' }));
  setTimeout(() => ws.send(JSON.stringify({ t: 'start', app: 1, w: 1280, h: 720, fps: 60 })), 300);
};
ws.onmessage = (e) => {
  if (typeof e.data === 'string') { texts.push(JSON.parse(e.data)); return; }
  const b = new Uint8Array(e.data);
  if (b[0] === 1) { inits++; chunks.push(b.subarray(1)); }
  else if (b[0] === 2) { frags++; chunks.push(b.subarray(1)); }
  else if (b[0] === 3) audioBytes += b.length - 1;
};

await new Promise((r) => setTimeout(r, 3500));
ws.send(JSON.stringify({ t: 'stop' }));
await new Promise((r) => setTimeout(r, 300));
ws.close();
bridge.kill();

const types = texts.map((m) => m.t);
for (const t of ['host', 'apps', 'video', 'audio', 'started', 'stopped']) {
  if (!types.includes(t)) fail(`missing "${t}" message (got ${types.join(',')})`);
}
if (types.includes('error')) fail('bridge reported an error: ' + JSON.stringify(texts.find((m) => m.t === 'error')));
if (inits !== 1) fail(`expected 1 init segment, got ${inits}`);
if (frags < 150) fail(`too few video fragments: ${frags}`);
if (audioBytes < 48000 * 4 * 2) fail(`too little audio: ${audioBytes} bytes`);

const out = path.join(os.tmpdir(), 'ml-smoke.mp4');
fs.writeFileSync(out, Buffer.concat(chunks.map((c) => Buffer.from(c))));
const dec = spawnSync('ffmpeg', ['-v', 'error', '-i', out, '-f', 'null', '-'], { encoding: 'utf8' });
if (dec.status !== 0 || dec.stderr.trim()) fail('ffmpeg could not decode the stream: ' + dec.stderr);

console.log(`OK: ${frags} video fragments, ${audioBytes} audio bytes, codec ${texts.find((m) => m.t === 'video').codec}`);
process.exit(0);

// worker.js — runs one Cooja-NG simulation in a Web Worker.
//
// csim-web.js/.wasm is the native simulator (kernel, CPU emulators, radio
// models, the runner and its UI service) compiled with Emscripten.  This file
// only moves data: it fetches the scenario's firmware and config into the
// module's in-memory filesystem, starts the runner with `--ui` exactly as on
// the command line, and relays the UI stream (JSON full states, CBOR deltas)
// and the page's commands.  It simulates nothing itself.
//
// Messages from the page:
//   {type: 'start', scenario: '<key in scenarios.json>', extra: [args...]}
//   {type: 'ws', data: '<JSON command>'}       // what the page would send on a WebSocket
// Messages to the page:
//   {ready: true}  {text: '...'}  {binary: ArrayBuffer}  {log: '...'}  {error: '...'}

importScripts('csim-web.js');

let mod = null;
const early = [];          // page commands that arrive before the module is up

function toModule(s) {
  const bytes = new TextEncoder().encode(s);
  const p = mod._malloc(bytes.length + 1);
  mod.HEAPU8.set(bytes, p);
  mod._csim_wasm_ui_recv(p, bytes.length);
  mod._free(p);
}

async function fetchBytes(path) {
  const r = await fetch('files/' + path);
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

async function start(key, extra) {
  const scenarios = await (await fetch('scenarios.json')).json();
  const sc = scenarios[key];
  if (!sc) throw new Error(`unknown scenario '${key}'`);
  const files = await Promise.all(sc.files.map(async f => [f, await fetchBytes(f)]));

  // The runner's stdout/stderr (packet traces, progress) only reach the page
  // with ?arg=--log; otherwise the last lines are kept for an error report.
  const verbose = (extra || []).includes('--log');
  const tail = [];
  const keep = s => { tail.push(s); if (tail.length > 40) tail.shift(); };
  mod = await createCsim({
    print: s => { if (verbose) postMessage({ log: s }); },
    printErr: s => { if (verbose) postMessage({ log: s }); else keep(s); },
    onAbort: what => postMessage({ error: `simulator aborted: ${what}\n` + tail.join('\n') }),
  });
  mod.FS.mkdir('/sim');
  for (const [f, bytes] of files) {
    const dir = '/sim/' + f.split('/').slice(0, -1).join('/');
    mod.FS.mkdirTree(dir);
    mod.FS.writeFile('/sim/' + f, bytes);
  }
  mod.FS.chdir('/sim');

  postMessage({ ready: true });
  for (const c of early.splice(0)) toModule(c);
  // Returns at the first yield; the run continues from the event loop.
  mod.callMain(sc.args.concat(['--ui'], (extra || []).filter(a => a !== '--log')));
}

onmessage = (e) => {
  const m = e.data;
  if (m.type === 'start') {
    start(m.scenario, m.extra).catch(err => postMessage({ error: String(err && err.message || err) }));
  } else if (m.type === 'ws') {
    if (mod) toModule(m.data); else early.push(m.data);
  }
};

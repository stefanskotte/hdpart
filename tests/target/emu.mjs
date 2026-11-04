// Drives one WinUAE instance through mcp-winuae-emu (GDB server + input tools).
// Window, gadget and program state are read from guest memory using the ELF
// symbols, so nothing here depends on pixel positions that vary by Kickstart.
import { spawn, execFileSync } from 'node:child_process';
import { appendFileSync, closeSync, openSync, statSync, existsSync, writeSync } from 'node:fs';
import { dirname } from 'node:path';
import { pathToFileURL } from 'node:url';
import net from 'node:net';

const WD_USERDATA = 120, WINDOW_MARK = 0x48445054;   // intuition.h Window.UserData; 'HDPT'
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const HOME = process.env.HOME;
export const WINUAE = process.env.WINUAE_PATH || `${HOME}/Devel/winuae-logical/build/winuae`;
const MCP_DIR = process.env.WINUAE_MCP_DIR || `${HOME}/Devel/mcp-winuae-emu`;
const PORT = Number(process.env.WINUAE_GDB_PORT || 2345);
// A normal run logs about 2 MB. An orphaned emulator once wrote 255 GiB and filled the disk.
export const LOG_CAP = Number(process.env.WINUAE_LOG_CAP || 20 * 1024 * 1024);

// Starts a process with its output going to logPath, up to cap bytes in that file. Past the cap the process
// is killed, so a runaway stops instead of going unnoticed. Output goes through pipes rather than to the file,
// so a process that outlives this one loses its pipe and gets SIGPIPE on its next write.
export function spawnCapped(cmd, args, logPath, cap = LOG_CAP) {
  let size = existsSync(logPath) ? statSync(logPath).size : 0;
  const fd = openSync(logPath, 'a');
  const child = spawn(cmd, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  child.capped = false;
  const take = (chunk) => {
    if (child.capped) return;
    const room = Math.max(0, cap - size), n = Math.min(room, chunk.length);
    if (n) { writeSync(fd, chunk, 0, n); size += n; }
    if (chunk.length > room) {
      child.capped = true;
      console.error(`emulator stopped: log ${logPath} reached its ${cap} byte limit`);
      child.kill('SIGKILL');
    }
  };
  child.stdout.on('data', take);
  child.stderr.on('data', take);
  child.once('close', () => closeSync(fd));
  return child;
}

function portOpen() {
  return new Promise((res) => {
    const s = net.connect(PORT, '127.0.0.1');
    s.on('connect', () => { s.destroy(); res(true); });
    s.on('error', () => res(false));
  });
}

// elf2hunk emits one hunk per ELF section, in this order.
const SECTIONS = ['.text', '.rodata', '.data', '.bss'];

export function loadSymbols(elf) {
  const objdump = process.env.AMIGA_OBJDUMP || 'm68k-amiga-elf-objdump';
  const sections = {};
  for (const l of execFileSync(objdump, ['-h', elf], { encoding: 'utf8' }).split('\n')) {
    const m = l.match(/^\s*\d+\s+(\.\S+)\s+[0-9a-f]+\s+([0-9a-f]+)/);
    if (m) sections[m[1]] = parseInt(m[2], 16);
  }
  const syms = {};
  for (const l of execFileSync(objdump, ['-t', elf], { encoding: 'utf8', maxBuffer: 1 << 26 }).split('\n')) {
    const m = l.match(/^([0-9a-f]{8})\s.*\s(\.\w+)\s+[0-9a-f]{8}\s+(\S+)$/);
    if (m && SECTIONS.includes(m[2])) syms[m[3]] = { vma: parseInt(m[1], 16), section: m[2] };
  }
  return { sections, syms };
}

// Has the program finished reacting, i.e. is it blocked in Wait() on this signal mask and nothing else?
// By state, not by list: exec's idle loop leaves the last task as ThisTask, so with nothing else scheduled
// a blocked program still shows as current. A running or ready task is not 4 (TS_WAIT), and a crashed one
// or one in a dialog waits on other signals, so all of those keep the wait going until it times out.
export const blockedOn = (task, mask) => Boolean(task) && task.state === 4 && task.signals_wait === mask;

export class Emu {
  constructor(cfgPath, elf, logPath) {
    this.cfgPath = cfgPath; this.logPath = logPath;
    this.sym = loadSymbols(elf);
    this.segs = null;
  }

  async start() {
    if (await portOpen()) throw new Error(`port ${PORT} is already in use: another emulator is running`);
    this.child = spawnCapped(WINUAE, ['-f', this.cfgPath, '-s', 'use_gui=no', '-s', 'debugging_features=gdbserver',
      '-s', `gdb_port=${PORT}`, '-s', 'cpu_threaded=false'], this.logPath);
    for (let i = 0; i < 60 && !(await portOpen()); i++) await sleep(500);
    const base = pathToFileURL(`${MCP_DIR}/node_modules/@modelcontextprotocol/sdk/dist/esm/client/`).href;
    const { Client } = await import(`${base}index.js`);
    const { StdioClientTransport } = await import(`${base}stdio.js`);
    const transport = new StdioClientTransport({
      command: 'node', args: [`${MCP_DIR}/dist/index.js`], stderr: 'ignore',
      env: { ...process.env, WINUAE_PATH: WINUAE, WINUAE_CONFIG: this.cfgPath, WINUAE_GDB_PORT: String(PORT) },
    });
    this.client = new Client({ name: 'hdpart-target-test', version: '1' });
    await this.client.connect(transport);
    await this.tool('winuae_attach');
    this.segs = null;
    await this.go();
  }

  // Stops only the emulator this object started.
  async stop() {
    try { await this.tool('winuae_detach'); } catch { /* already gone */ }
    try { await this.client?.close(); } catch { /* already gone */ }
    if (this.child && this.child.exitCode === null) {
      const gone = new Promise((r) => this.child.once('exit', r));
      this.child.kill();
      await Promise.race([gone, sleep(5000)]);
      if (this.child.exitCode === null) this.child.kill('SIGKILL');
    }
    for (let i = 0; i < 20 && (await portOpen()); i++) await sleep(250);
  }

  async tool(name, args = {}) {
    const r = await this.client.callTool({ name, arguments: args }, undefined, { timeout: 120000 });
    const text = r.content.map((c) => c.text ?? '').join('\n');
    if (r.isError || /^Error:/.test(text)) throw new Error(`${name}: ${text}`);
    return text;
  }

  // Reads pause the guest; call this before anything that needs it running.
  async go() { try { await this.tool('winuae_continue'); } catch { /* already running */ } }
  async shot(file) { await this.tool('winuae_screenshot', { filename: file }); await this.go(); }

  /* ---- guest memory ---- */
  async mem(addr, len) {
    const t = await this.tool('winuae_memory_read', { address: addr, length: len });
    return Buffer.from((t.split('\n').slice(1).join(' ').match(/\b[0-9A-Fa-f]{2}\b/g) || []).join(''), 'hex');
  }
  async u8(a) { return (await this.mem(a, 1))[0]; }
  async u16(a) { return (await this.mem(a, 2)).readUInt16BE(0); }
  async i16(a) { return (await this.mem(a, 2)).readInt16BE(0); }
  async u32(a) { return (await this.mem(a, 4)).readUInt32BE(0); }
  async cstr(a, max = 80) {
    if (!a) return '';
    const b = await this.mem(a, max); const n = b.indexOf(0);
    return b.subarray(0, n < 0 ? max : n).toString('latin1');
  }

  /* ---- program location ---- */
  async locate(timeout = 90000, pollMs = 1000) {
    const end = Date.now() + timeout;
    while (Date.now() < end) {
      let tasks = [];
      try { tasks = JSON.parse(await this.tool('winuae_tasks')); } catch { /* exec not up yet */ }
      for (const t of tasks) {
        if (t.type !== 13) continue;
        let s;
        try { s = JSON.parse(await this.tool('winuae_loaded_segments', { address: t.address })); } catch { continue; }
        if (s.command === 'HDPart' && s.segments?.length === SECTIONS.length) {
          this.segs = s.segments; this.process = t.address;
          await this.go();
          return;
        }
      }
      await this.go(); await sleep(pollMs);
    }
    throw new Error('HDPart process not found: did the machine boot?');
  }
  addr(name) {
    let s = this.sym.syms[name];
    if (!s) { const k = Object.keys(this.sym.syms).find((n) => n.startsWith(`${name}.`)); s = this.sym.syms[k]; }
    if (!s) throw new Error(`no symbol ${name}`);
    return this.segs[SECTIONS.indexOf(s.section)].address + (s.vma - this.sym.sections[s.section]);
  }
  async g32(name) { return this.u32(this.addr(name)); }
  async gstr(name, off = 0, max = 80) { return this.cstr(this.addr(name) + off, max); }

  /* ---- Intuition ----  offsets from intuition/intuition.h, screens.h */
  async windows() {
    const out = []; const scr = await this.g32('g_scr');
    if (!scr) return out;
    for (let w = await this.u32(scr + 4), i = 0; w && i < 32; w = await this.u32(w), i++)
      out.push({ addr: w, title: await this.cstr(await this.u32(w + 32), 60) });
    return out;
  }
  // Newest window first, so a message box wins over a parent with the same title.
  async win(title) { return (await this.windows()).find((w) => w.title.startsWith(title))?.addr || 0; }
  async gadgets(w) {
    const out = [];
    for (let g = await this.u32(w + 62), i = 0; g && i < 64; g = await this.u32(g), i++) {
      const b = await this.mem(g, 42);
      const gad = { addr: g, left: b.readInt16BE(4), top: b.readInt16BE(6), width: b.readInt16BE(8), height: b.readInt16BE(10),
        flags: b.readUInt16BE(12), type: b.readUInt16BE(16), special: b.readUInt32BE(34), id: b.readUInt16BE(38) };
      if (!(gad.type & 0x8000) && gad.width > 0 && gad.height > 0) out.push(gad);
    }
    return out;
  }
  async gadget(w, id) {
    // Listview rows are GadTools-internal gadgets numbered from 0, so an ID alone is
    // ambiguous. They are GFLG_GADGHNONE, which no clickable gadget here uses.
    const g = (await this.gadgets(w)).find((x) => x.id === id && (x.flags & 3) !== 3);
    if (!g) throw new Error(`no gadget ${id}`);
    return g;
  }
  async waitFor(what, fn, timeout = 20000) {
    const end = Date.now() + timeout;
    for (;;) {
      const v = await fn();
      await this.go();
      if (v) return v;
      if (Date.now() > end) throw new Error(`timed out waiting for ${what}`);
      await sleep(150);
    }
  }
  // Wait until the program blocks on window w's port.
  async waitIdle(w, timeout) {
    const sig = await this.u8((await this.u32(w + 86)) + 15), mask = (1 << sig) >>> 0;
    await this.waitFor('program idle', async () =>
      blockedOn(JSON.parse(await this.tool('winuae_tasks')).find((x) => x.address === this.process), mask), timeout);
  }
  // One look: is the program blocked on window w's port right now?
  async isIdle(w) {
    const sig = await this.u8((await this.u32(w + 86)) + 15), mask = (1 << sig) >>> 0;
    return blockedOn(JSON.parse(await this.tool('winuae_tasks')).find((x) => x.address === this.process), mask);
  }
  waitWin(title, timeout) { return this.waitFor(`window "${title}"`, () => this.win(title), timeout); }

  /* ---- input ---- */
  // WinUAE refuses a new sequence while one is running.
  async inputIdle() {
    await this.go();
    for (let i = 0; i < 300; i++) {
      if (!/"state":"running"/.test(await this.tool('winuae_input', { action: 'status' }))) return;
      await sleep(30);
    }
    throw new Error('previous input did not finish');
  }
  async seq(steps) {
    await this.inputIdle();
    await this.tool('winuae_input_sequence', { steps });
    for (let i = 0; i < 300; i++) {
      await sleep(40);
      const t = await this.tool('winuae_input', { action: 'status' });
      if (!/"state":"running"/.test(t)) return;
      if (/"execution":"stopped"/.test(await this.tool('winuae_status'))) return;  // hit a breakpoint
    }
    throw new Error('input sequence did not finish');
  }
  // Closed loop on Screen->MouseX/Y: the count-to-pixel ratio differs per screen mode. Intuition updates these as it
  // handles the movement, so one read can be a stop on the way: arrived means two reads with the guest running between them.
  // Returns the settled position.
  async moveTo(x, y) {
    const scr = await this.g32('g_scr');
    let sx = 1, sy = 2, seen = null;
    for (let i = 0; i < 24; i++) {
      const my = await this.i16(scr + 16), mx = await this.i16(scr + 18);
      const dx = x - mx, dy = y - my;
      if (Math.abs(dx) <= 2 && Math.abs(dy) <= 2) {
        await this.go();
        if (seen && seen.mx === mx && seen.my === my) return { mx, my, reads: i + 1 };
        seen = { mx, my };
        await sleep(100);       // a few frames
        continue;
      }
      seen = null;
      const steps = [];
      for (const [axis, count] of [['x', Math.round(dx * sx)], ['y', Math.round(dy * sy)]]) {
        for (let n = count; n !== 0;) {
          const d = Math.max(-100, Math.min(100, n));
          steps.push({ after_frames: 1, action: 'mouse', axis, delta: d, port: 0 }); n -= d;
        }
      }
      steps.push({ after_frames: 3, action: 'wait' });
      await this.seq(steps);
    }
    throw new Error(`pointer did not reach ${x},${y}`);
  }
  // One line per press, beside the emulator log: where it was meant to go and where the pointer was. After a failure this
  // says whether the press went astray or landed and was ignored.
  pressLog(line) { appendFileSync(`${dirname(this.logPath)}/press.log`, `${new Date().toISOString()} ${line}\n`); }
  async clickAt(x, y) {
    const at = await this.moveTo(x, y);
    this.pressLog(`press target ${x},${y} mouse ${at.mx},${at.my} after ${at.reads} reads`);
    await this.seq([{ after_frames: 2, action: 'button', button: 0, pressed: true, port: 0 },
      { after_frames: 6, action: 'button', button: 0, pressed: false, port: 0 }]);
  }
  async click(w, id) {
    // A dialog can close a moment before the GUI re-enables the buttons behind it.
    const g = await this.waitFor(`gadget ${id} enabled`, async () => {
      const x = await this.gadget(w, id);
      return (x.flags & 0x100) ? null : x;
    }, 5000);
    const wl = await this.i16(w + 4), wt = await this.i16(w + 6);
    await this.clickAt(wl + g.left + (g.width >> 1), wt + g.top + (g.height >> 1));
  }
  // Click a dialog button and wait for that dialog to close or be replaced. Its address doesn't say: the next window of the same
  // size lands on the same memory, with its first gadget at the same address too, and on a fast machine it is up before the first
  // look. So mark this window in its UserData, which HDPart doesn't use and which a new window starts without.
  async clickClose(w, id, timeout = 20000) {
    const first = await this.u32(w + 62);
    await this.tool('winuae_memory_write', { address: w + WD_USERDATA, data: WINDOW_MARK.toString(16) });
    await this.click(w, id);
    await this.waitFor('dialog to close', async () => {
      const open = (await this.windows()).some((x) => x.addr === w);
      return !open || (await this.u32(w + 62)) !== first || (await this.u32(w + WD_USERDATA)) !== WINDOW_MARK;
    }, timeout);
    await sleep(400);      // let the code after the dialog finish before anyone looks
  }
  async key(...codes) {
    await this.inputIdle();
    await this.tool('winuae_key_tap', { codes });
    await this.inputIdle();
  }
  async type(text) {
    await this.inputIdle();
    await this.tool('winuae_type_text', { text, layout: 'us' });
    await this.inputIdle();
  }
  async typeInto(w, id, text) {
    const g = await this.gadget(w, id);
    await this.click(w, id);
    await this.key(0x67, 0x32);                 // right-Amiga X clears a string gadget
    await this.type(text);
    const buf = await this.u32(g.special);
    await this.waitFor(`"${text}" typed`, async () => (await this.cstr(buf, 64)) === text);
  }
  async pickMenu(menu, item) {
    const scr = await this.g32('g_scr');
    let m = await this.g32('g_menu');
    for (let i = 0; i < menu; i++) m = await this.u32(m);
    const mLeft = await this.i16(m + 4), mWidth = await this.i16(m + 8);
    let it = await this.u32(m + 18);
    for (let i = 0; i < item; i++) it = await this.u32(it);
    const iLeft = await this.i16(it + 4), iTop = await this.i16(it + 6), iW = await this.i16(it + 8), iH = await this.i16(it + 10);
    const bar = await this.u8(scr + 30);
    await this.moveTo(mLeft + (mWidth >> 1), bar >> 1);
    // A sequence releases held inputs when it ends, so the drag uses direct input.
    await this.inputIdle();
    await this.tool('winuae_input', { action: 'button', button: 1, port: 0, pressed: true });
    await this.go(); await sleep(300);
    const tx = mLeft + iLeft + (iW >> 1), ty = bar + 1 + iTop + (iH >> 1);
    for (let i = 0; i < 30; i++) {
      const my = await this.i16(scr + 16), mx = await this.i16(scr + 18);
      const dx = tx - mx, dy = ty - my;
      if (Math.abs(dx) <= 2 && Math.abs(dy) <= 1) break;
      if (dx) await this.tool('winuae_input', { action: 'mouse', axis: 'x', delta: Math.max(-100, Math.min(100, dx)), port: 0 });
      if (dy) await this.tool('winuae_input', { action: 'mouse', axis: 'y', delta: Math.max(-100, Math.min(100, dy * 2)), port: 0 });
      await this.go(); await sleep(120);
    }
    await this.go(); await sleep(200);
    await this.tool('winuae_input', { action: 'button', button: 1, port: 0, pressed: false });
    await this.go();
  }

  /* ---- fault injection ---- */
  // Run `act`, stop at the next call to function `sym`, make it return `value` without running.
  // `before(sp)` runs at that stop, with the call's return address at sp and its arguments above it.
  async failNextCall(sym, act, value = 0, before = null) {
    const bp = this.addr(sym);
    await this.tool('winuae_breakpoint_set', { address: bp });
    await act();
    await this.tool('winuae_wait_stop', { timeout_ms: 30000 });
    const regs = await this.tool('winuae_registers_get');
    const sp = parseInt(regs.match(/A7=\$([0-9A-F]+)/)[1], 16), pc = parseInt(regs.match(/PC=\$([0-9A-F]+)/)[1], 16);
    if (pc !== bp) throw new Error(`stopped at ${pc.toString(16)}, not ${sym}`);
    const ret = await this.u32(sp);
    if (before) await before(sp);
    await this.tool('winuae_registers_set', { D0: value, PC: ret, A7: sp + 4 });
    await this.tool('winuae_breakpoint_clear', { address: bp });
    await this.go();
  }
}

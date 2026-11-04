#!/usr/bin/env node
// The engine on emulated IDE: a card on the internal IDE port, driven through scsi.device.
// The questions Quick setup rests on are decided here. See docs/reference/quick-setup-tests.md.
// Usage: node tests/target/quick.mjs [ide|fresh|used|restart|limit|offset|card|run|resume] [keys...]     See tests/target/README.md.
import { chmodSync, closeSync, copyFileSync, existsSync, ftruncateSync, mkdirSync, openSync, readFileSync, readSync, renameSync, rmSync, statSync, writeFileSync, writeSync } from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { Emu, sleep } from './emu.mjs';
import { readRdb } from './rdbcheck.mjs';
import { recordFailure } from './faillog.mjs';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const ELF = `${ROOT}/out/HDPart.elf`;
const MiB = 1 << 20;
const GiB4 = 2 ** 32;

const A600 = ['cpu_model=68000', 'chipset=ecs', 'chipset_compatible=A600', 'chipmem_size=4', 'fastmem_size=8'];
const A1200 = ['cpu_model=68020', 'chipset=aga', 'chipset_compatible=A1200', 'chipmem_size=4', 'fastmem_size=8'];
const A4000 = ['cpu_model=68040', 'fpu_model=68040', 'cpu_24bit_addressing=false', 'cpu_compatible=false', 'chipset=aga',
  'chipset_compatible=A4000', 'chipmem_size=4', 'fastmem_size=8'];
const A3000 = ['cpu_model=68030', 'mmu_model=68030', 'cpu_24bit_addressing=false', 'cpu_compatible=false', 'chipset=ecs', 'chipset_compatible=A3000',
  'chipmem_size=4', 'a3000mem_size=8'];
const A4000T = [...A4000.filter((o) => !o.startsWith('chipset_compatible')), 'chipset_compatible=A4000T'];
const A500P = ['cpu_model=68000', 'chipset=ecs', 'chipset_compatible=A500+', 'chipmem_size=2', 'fastmem_size=8'];
const ENGINE = { ks205a: A600, ks205b: A600, ks30: A1200, ks31a600: A600, ks31a1200: A1200, ks31a4000: A4000, ks314: A1200, ks32: A1200 };
// Used only by the IDE check. Keys with a second word need files as well as a ROM: see IDE_CHECK.
export const MACHINES = {
  ...ENGINE,
  ks314a4000: A4000, ks32a4000: A4000,
  a3000ks204: A3000, a3000ks31: A3000, a3000ks32: A3000, a500pks204: A500P, a4000tks31: A4000T, a4000tks32: A4000T,
  a3000roma4000: A3000, a3000roma1200: A3000,
  ks314setpatch: A1200, ks32setpatch: A1200, ks31softload: A1200, a3000softload: A3000,
};

// QUICK_ROM_KS31A1200=/path/to.rom. The lower-case spelling of the key works too.
export const romFor = (key) => process.env[`QUICK_ROM_${key.toUpperCase()}`] || process.env[`QUICK_ROM_${key}`] || '';

const GID = { DEVICE: 1, SCAN: 2, NEW: 5, DELETE: 6, SAVE: 9, SPLIT: 12, RESIZE: 14, FORMAT: 15, QUICK: 17 };   // src/gui.c
const PROCEED = 1, OK = 1, CANCEL = 0;                     // gui_request
const ED_OK = 10;                                          // gui_edit_dialog
const VC = { WRITE: 1, VERIFY: 2, INFO: 4 };               // tests/target/guest/volcheck.s
const PHASES = ['ok', 'Lock/Info', 'open to write', 'write', 'open to read', 'read', 'compare'];
// A 68000 at cpu_speed=max writes about 0.4 MB/s, a 68020 about 1.5. Filling on the faster CPU makes the
// A600 runs possible. Same ROM, same chipset, same FFS and scsi.device code; only the filler is written this way.
const FILL_68020 = ['cpu_model=68020', 'chipset=ecs', 'chipset_compatible=A600', 'chipmem_size=4', 'fastmem_size=8'];

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${JSON.stringify(b)}, got ${JSON.stringify(a)}`); }
function ok(c, what) { if (!c) throw new Error(what); }
const hex = (s) => Buffer.from(`${s}\0`, 'latin1').toString('hex');
const long = (v) => { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0); return b.toString('hex'); };
export function pathTag(vol, file) {
  let h = 0;
  for (const c of Buffer.from(`${vol}:${file}`, 'latin1')) h = (Math.imul(h, 31) + c) >>> 0;
  return h;
}

// Every volcheck sector in the hardfile, by tag. Checks each sector's pattern independently of the guest.
export function scanStamps(file) {
  const fd = openSync(file, 'r'), size = statSync(file).size, buf = Buffer.alloc(16 * MiB), out = new Map();
  for (let pos = 0; pos < size; pos += buf.length) {
    const n = readSync(fd, buf, 0, buf.length, pos);
    for (let o = 0; o + 512 <= n; o += 512) {
      if (buf.readUInt32BE(o) !== 0x5643484B) continue;
      const tag = buf.readUInt32BE(o + 4), idx = buf.readUInt32BE(o + 8), lba = (pos + o) / 512;
      let v = (idx ^ tag ^ 0x5AA55AA5) >>> 0, good = true;
      for (let i = 3; i < 128; i++) { if (buf.readUInt32BE(o + i * 4) !== v) { good = false; break; } v = (v + 0x01010101) >>> 0; }
      let s = out.get(tag);
      if (!s) out.set(tag, s = { count: 0, bad: 0, min: lba, max: lba, lastIdx: -1, lastIdxLba: 0 });
      s.count++; if (!good) s.bad++;
      if (lba < s.min) s.min = lba;
      if (lba > s.max) s.max = lba;
      if (idx > s.lastIdx) { s.lastIdx = idx; s.lastIdxLba = lba; }
    }
  }
  closeSync(fd);
  return out;
}

export function peek(file, lba, sectors = 1) {
  const fd = openSync(file, 'r'), b = Buffer.alloc(sectors * 512);
  readSync(fd, b, 0, b.length, lba * 512);
  closeSync(fd);
  return b;
}

export class QuickSuite {
  // spec: {key, rom, machine}. card: hardfile path, created blank and sparse if missing.
  constructor(spec, card, { name = 'run', cardBytes = 64 * MiB, cpuMax = false } = {}) {
    this.key = spec.key; this.spec = spec; this.card = card; this.cardBytes = cardBytes; this.cpuMax = cpuMax;
    this.dir = `${ROOT}/out/target-quick/${spec.key}/${name}`;
    this.results = []; this.requesters = []; this.seq = 0; this.log = [];
  }

  prepare() {
    rmSync(this.dir, { recursive: true, force: true });
    for (const d of ['boot/s', 'shots']) mkdirSync(`${this.dir}/${d}`, { recursive: true });
    copyFileSync(`${ROOT}/out/HDPart.exe`, `${this.dir}/boot/HDPart`);
    copyFileSync(`${ROOT}/out/volcheck`, `${this.dir}/boot/volcheck`);
    for (const d of this.spec.dirs || []) mkdirSync(`${this.dir}/${d}`, { recursive: true });
    for (const [name, from] of Object.entries(this.spec.files || {})) {
      copyFileSync(from, `${this.dir}/boot/${name}`);
      chmodSync(`${this.dir}/boot/${name}`, 0o755);      // the host's x bit becomes the Amiga's e flag
    }
    mkdirSync(`${this.dir}/env`, { recursive: true });
    writeFileSync(`${this.dir}/boot/s/startup-sequence`, `${(this.spec.startup || []).join('\n')}\nRun >NIL: volcheck serve\nHDPart\n`);
    if (!this.spec.noCard && !existsSync(this.card)) {
      mkdirSync(path.dirname(this.card), { recursive: true });
      const fd = openSync(this.card, 'w'); ftruncateSync(fd, this.cardBytes); closeSync(fd);
    }
    this.cfg = `${this.dir}/test.uae`;
    this.setMachine(this.spec.machine);
    this.e = new Emu(this.cfg, ELF, `${this.dir}/emulator.log`);
  }

  // Takes effect at the next start.
  setMachine(machine) {
    writeFileSync(this.cfg, [
      'use_gui=no', `kickstart_rom_file=${this.spec.rom}`, ...machine, 'nr_floppies=1', 'sound_output=none',
      ...(this.cpuMax ? ['cpu_speed=max'] : []),
      `filesystem2=ro,TESTBOOT:TESTBOOT:${this.dir}/boot,${this.spec.bootPri ?? 0}`,
      ...(this.spec.cfg ? this.spec.cfg(this.dir) : []),
      // SetPatch asks for ENV:, which the bare boot does not assign. A volume of that name does the job.
      ...(this.spec.startup ? [`filesystem2=rw,ENV:ENV:${this.dir}/env,-128`] : []),
      ...(this.spec.noCard ? [] : [`hardfile2=rw,:${this.card},0,0,0,512,-128,,${this.spec.controller || 'ide0'}`]), '',
    ].join('\n'));
  }

  note(line) { this.log.push(line); console.log(`       ${line}`); }

  async step(name, fn) {
    const t = Date.now();
    try {
      const v = await fn();
      this.results.push({ name, pass: true });
      console.log(`  ok   ${name} (${((Date.now() - t) / 1000).toFixed(1)}s)`);
      return v;
    } catch (err) {
      this.results.push({ name, pass: false, err: err.message });
      console.log(`  FAIL ${name}: ${err.message}`);
      const shot = `${this.dir}/shots/FAIL-${this.results.length}.png`;
      const lines = [];
      try { await this.e.shot(shot); } catch { /* emulator gone */ }
      const ev = async (f) => { try { lines.push(await f()); } catch (e) { lines.push(`(unavailable: ${e.message})`); } };
      await ev(async () => `task: ${JSON.stringify(JSON.parse(await this.e.tool('winuae_tasks')).find((x) => x.address === this.e.process))}`);
      await ev(async () => `windows: ${JSON.stringify((await this.e.windows()).map((w) => w.title))}`);
      await ev(async () => `status="${await this.status()}"`);
      for (const l of lines) console.log(`       ${l}`);
      recordFailure(ROOT, { ks: this.key, name, error: err.message, lines, shot });
      throw err;
    }
  }

  /* ---- GUI state and actions, as in run.mjs ---- */
  main() { return this.e.win('HDPart '); }
  async nparts() { return this.e.u32(this.e.addr('g_model') + 36); }
  async sel() { return (await this.e.g32('g_sel_part')) | 0; }
  async status() { return this.e.gstr('g_statusbuf'); }
  async row(i) { return this.e.gstr('g_partrows', i * 80); }
  async clickMain(id) { await this.e.click(await this.main(), id); }
  async isChoice(w) { return (await this.e.gadgets(w)).some((g) => g.id === CANCEL); }

  async idle(timeout = 120000) { await this.e.waitIdle(await this.main(), timeout); }

  async boot() {
    const e = this.e;
    await this.step('Boot: HDPart and volcheck are running', async () => {
      await e.start();
      await this.waitUp();
    });
  }
  async waitUp() {
    const e = this.e;
    await e.locate();
    await e.waitFor('picker ready', async () => (await this.status()).includes('Select a driver'), 90000);
    await e.waitFor('volcheck listening', async () => (await e.u32(0x17C)) === 0x53524459, 30000);
    this.seq = 0;
    // The status is set before the startup check runs. A breakpoint armed during that check is hit by the check, not by what the
    // test goes on to do, so wait until it has ended: in the main loop, or on the popup.
    const w = await this.main(), t = Date.now();
    let looks = 0;
    await e.waitFor('the startup check', async () => {
      looks++;
      return (await e.g32('g_qs_resume_checks')) >= 1 && ((await e.g32('g_qs_screen')) === 7 || await e.isIdle(w));
    }, 120000);
    this.note(`startup check over ${Date.now() - t} ms after the status, ${looks} looks`);
  }
  // Leave a little time for the filesystems to flush, as a user waits for the drive light.
  async restart() {
    const e = this.e;
    await sleep(5000);
    await e.stop();
    await e.start();
    await this.waitUp();
    await this.collectRequesters('after restart');
  }

  async pickScsi(wantParts) {
    const e = this.e;
    for (let i = 0; i < 40; i++) {
      const idx = (await e.g32('g_drv_idx')) | 0;
      const label = await e.cstr(await e.u32(e.addr('g_drvlabels') + Math.max(idx, 0) * 4), 40);
      if (idx >= 0 && label.includes('scsi.device')) break;
      if (i === 39) throw new Error('no scsi.device in the driver list');
      await this.clickMain(GID.DEVICE);
      await sleep(500);
    }
    await this.idle();
    await this.clickMain(GID.SCAN);
    await e.waitFor('units after Scan', async () => (await e.g32('g_nunits')) >= 1, 180000);
    await this.idle();
    if (((await e.g32('g_cur_unitidx')) | 0) !== 0) {
      const w = await this.main(), b = await e.mem(e.addr('g_ulist'), 16);
      await e.clickAt((await e.i16(w + 4)) + b.readInt16BE(0) + (b.readInt16BE(4) >> 1),
        (await e.i16(w + 6)) + b.readInt16BE(2) + (b.readInt16BE(8) >> 1));
      await e.waitFor('unit shown', async () => ((await e.g32('g_cur_unitidx')) | 0) === 0);
      await this.idle();
    }
    if (wantParts !== undefined) eq(await this.nparts(), wantParts, 'partitions after Scan');
  }

  async selectPart(i) {
    const e = this.e, w = await this.main();
    const cyl = await e.u32(e.addr('g_model'));
    const nums = (await this.row(i)).trim().split(/\s+/).filter((x) => /^\d+$/.test(x)).map(Number);
    const [low, high] = nums;   // columns: Start End Size
    const bar = await e.mem(e.addr('g_bar'), 8);
    const x = bar.readInt16BE(0) + Math.floor(((low + high + 1) / 2 * bar.readInt16BE(4)) / cyl);
    await e.clickAt((await e.i16(w + 4)) + x, (await e.i16(w + 6)) + bar.readInt16BE(2) + (bar.readInt16BE(6) >> 1));
    await e.waitFor(`partition ${i} selected`, async () => (await this.sel()) === i);
  }

  async split(n) {
    const e = this.e, before = await this.nparts();
    await this.clickMain(GID.SPLIT);
    const w = await e.waitWin('Split remaining');
    for (let c = 4; c > n; c--) { await e.click(w, 2); await sleep(200); }   // the dialog opens at 4
    for (let c = 4; c < n; c++) { await e.click(w, 3); await sleep(200); }
    await e.clickClose(w, 10);
    eq(await this.nparts(), before + n, 'partitions after Split');
  }

  async deleteAll() {
    const e = this.e;
    for (let n = await this.nparts(); n > 0; n = await this.nparts()) {
      await this.selectPart(0);
      await this.clickMain(GID.DELETE);
      const w = await e.waitWin('Delete Partition');
      await e.clickClose(w, PROCEED);
      eq(await this.nparts(), n - 1, 'partitions after Delete');
    }
  }

  // One partition of `mb` megabytes at the start of the free space.
  async newSized(mb) {
    const e = this.e;
    await this.clickMain(GID.NEW);
    await e.clickClose(await e.waitWin('Edit Partition'), ED_OK);
    await this.idle();
    await this.clickMain(GID.RESIZE);
    const w = await e.waitWin('Resize partition');
    await sleep(800);
    await e.typeInto(w, 2, String(mb));
    await sleep(400);
    await e.click(w, 10);
    await e.waitFor('confirmation', async () => (await e.windows()).filter((x) => x.title === 'Resize partition').length === 2);
    await e.clickClose(await e.win('Resize partition'), PROCEED);
    await e.waitFor('resize dialog closed', async () => !(await e.win('Resize partition')));
    await this.idle();
  }

  // Answers every confirmation. Returns the words of the mounted-volume warning, if it came.
  async save() {
    const e = this.e;
    await this.clickMain(GID.SAVE);
    const said = [];
    for (let i = 0; i < 4; i++) {
      const w = await e.waitWin('Save', 60000);
      if (!(await this.isChoice(w))) {
        said.push('result box');
        await e.clickClose(w, OK);
        eq(await e.g32('g_dirty'), 0, 'table is saved');
        await this.idle();
        return said;
      }
      said.push('confirm');
      await e.clickClose(w, PROCEED, 40000);
    }
    throw new Error('too many Save dialogs');
  }

  // Foreign windows are the system's: disk requesters and Software Failure boxes.
  async foreignWindows() {
    return (await this.e.windows()).filter((w) => !/^(HDPart |Format|Volume name|Save|Delete Partition|Edit Partition|Resize partition|Split remaining|Quick setup)/.test(w.title) && w.title !== '');
  }
  // Records and answers Cancel to each system requester on screen.
  async collectRequesters(context) {
    const e = this.e;
    for (let tries = 0; tries < 8; tries++) {
      const w = (await this.foreignWindows())[0];
      if (!w) { await e.go(); return; }
      const shot = `${this.dir}/shots/req-${this.requesters.length}.png`;
      await e.shot(shot);
      this.requesters.push({ context, title: w.title, shot: path.basename(shot) });
      this.note(`REQUESTER during ${context}: "${w.title}" (${path.basename(shot)})`);
      const gs = (await e.gadgets(w.addr)).sort((a, b) => b.left - a.left);
      ok(gs.length, 'requester has no buttons');
      const wl = await e.i16(w.addr + 4), wt = await e.i16(w.addr + 6);
      await e.clickAt(wl + gs[0].left + (gs[0].width >> 1), wt + gs[0].top + (gs[0].height >> 1));
      await sleep(800);
    }
    throw new Error(`requester keeps coming back during ${context}`);
  }

  // Format partition i through the GUI. Reports what happened instead of throwing on a refusal.
  async format(i, label, { timeout = 180000 } = {}) {
    const e = this.e, r = { part: i, label, ok: false };
    await this.selectPart(i);
    await e.tool('winuae_memory_write', { address: e.addr('g_statusbuf'), data: '00' });
    await e.go();
    await this.clickMain(GID.FORMAT);
    const w = await e.waitWin('Format', 30000);
    r.confirm = await e.gstr('msg', 0, 200);
    r.reformat = /mounted as/.test(r.confirm);
    if (!(await this.isChoice(w))) { r.error = 'refused before the confirmation'; await e.clickClose(w, OK); return r; }
    await e.clickClose(w, PROCEED);
    const v = await e.waitWin('Volume name');
    await e.typeInto(v, 20, label);
    await e.clickClose(v, 21);
    const end = Date.now() + timeout;
    for (;;) {
      if ((await this.status()).includes('Formatted')) { r.ok = true; break; }
      const box = await e.win('Format');
      if (box) { r.error = 'Format reported an error'; await e.shot(`${this.dir}/shots/format-${i}-error.png`); await e.clickClose(box, OK); break; }
      await this.collectRequesters(`format of partition ${i}`);
      await e.go();
      if (Date.now() > end) { r.error = 'timed out'; await e.shot(`${this.dir}/shots/format-${i}-timeout.png`); break; }
      await sleep(500);
    }
    await e.go();
    this.note(`format ${i} "${label}": ${r.ok ? 'ok' : r.error}${r.reformat ? ' (through the live mount)' : ''}: ${r.confirm.replace(/\n/g, ' / ')}`);
    return r;
  }

  /* ---- volcheck ---- */
  // Returns the result block. `hung` is set when nothing moved for stallMs.
  async volcheck(vol, mb, { file = 'vc.dat', flags = 0, stallMs = 180000, quiet = false } = {}) {
    const e = this.e, seq = ++this.seq;
    await e.tool('winuae_memory_write', { address: 0x184, data: long(mb) + long(flags) });
    await e.tool('winuae_memory_write', { address: 0x190, data: hex(vol) });
    await e.tool('winuae_memory_write', { address: 0x1A0, data: hex(file) });
    await e.tool('winuae_memory_write', { address: 0x180, data: long(seq) });
    await e.go();
    let last = -1, lastMove = Date.now(), polls = 0, b;
    const t0 = Date.now();
    for (;;) {
      await sleep(flags & VC.INFO ? 300 : 1000);
      b = await e.mem(0x100, 64);
      await e.go();
      if (b.readUInt32BE(0) === 0x5643444E && b.readUInt32BE(0x34) === seq) break;
      const moved = b.readUInt32BE(0xC) + b.readUInt32BE(0x10);
      if (moved !== last) { last = moved; lastMove = Date.now(); }
      if (++polls % 3 === 0) await this.collectRequesters(`volcheck ${vol}:${file}`);
      if (Date.now() - lastMove > stallMs) {
        const shot = `${this.dir}/shots/hung-${seq}.png`;
        await e.shot(shot);
        const r = { vol, file, hung: true, mb, flags, wrote: b.readUInt32BE(0xC), read: b.readUInt32BE(0x10), shot: path.basename(shot) };
        this.note(`volcheck ${vol}:${file} HUNG after ${r.wrote} written, ${r.read} read (${r.shot})`);
        return r;
      }
    }
    await this.collectRequesters(`volcheck ${vol}:${file}`);
    const L = (o) => b.readUInt32BE(o);
    const r = { vol, file, mb, flags, phase: L(8) | 0, err: L(4), wrote: L(0xC), read: L(0x10), equal: L(0x14), bad: L(0x18), firstBad: L(0x1C) | 0,
      blocks: L(0x20), used: L(0x24), bsize: L(0x28), dtype: L(0x2C).toString(16), state: L(0x30), usedAfter: L(0x38), tag: L(0x3C), secs: (Date.now() - t0) / 1000 };
    if (!quiet) this.note(`volcheck ${vol}:${file} ${mb} MB flags ${flags}: ${PHASES[r.phase] ?? r.phase} err ${r.err} wrote ${r.wrote} read ${r.read} equal ${r.equal} bad ${r.bad} blocks ${r.blocks} used ${r.usedAfter} state ${r.state} (${r.secs.toFixed(0)}s)`);
    return r;
  }

  bounds(partIndex) {
    const r = readRdb(this.card), p = r.parts[partIndex];
    return { first: p.low * r.cylBlocks, last: (p.high + 1) * r.cylBlocks - 1, name: p.name, blocks: (p.high - p.low + 1) * r.cylBlocks };
  }
}

const passed = (r) => r && !r.hung && r.phase === 0;

/* ---- Step 1: a fresh partition on a blank card ---- */
async function fresh(spec) {
  const card = `${ROOT}/out/target-quick/${spec.key}/fresh.hdf`;
  rmSync(card, { force: true });
  const s = new QuickSuite(spec, card, { name: 'fresh', cardBytes: 64 * MiB });
  s.prepare();
  const res = { test: 'fresh', key: spec.key };
  try {
    await s.boot();
    await s.step('Blank card: Scan finds scsi.device unit 0 with no table', async () => { await s.pickScsi(0); });
    await s.step('Split into 2, Save', async () => { await s.split(2); await s.save(); });
    await s.step('RDB on the card is valid', async () => { eq(readRdb(card).parts.length, 2, 'parts'); });
    for (const i of [0, 1]) {
      await s.step(`Format DH${i} and volcheck 5 MB`, async () => {
        const f = await s.format(i, `FRESH${i}`);
        ok(f.ok, `format: ${f.error}`);
        const r = await s.volcheck(`DH${i}`, 5);
        ok(passed(r), `volcheck failed: ${JSON.stringify(r)}`);
        eq(r.wrote, 5 * MiB, 'bytes written'); eq(r.read, 5 * MiB, 'bytes read'); eq(r.equal, 5 * MiB, 'bytes equal');
        const st = scanStamps(card).get(r.tag), b = s.bounds(i);
        ok(st, 'no stamps in the hardfile');
        eq(st.count, 5 * 2048, 'stamps on the card'); eq(st.bad, 0, 'damaged stamps');
        ok(st.min >= b.first && st.max <= b.last, `stamps ${st.min}-${st.max} outside ${b.first}-${b.last}`);
      });
    }
    res.pass = s.requesters.length === 0;
    if (!res.pass) s.note('requesters appeared');
  } catch (err) { res.pass = false; res.err = err.message; } finally { await s.e.stop(); }
  res.requesters = s.requesters;
  writeFileSync(`${s.dir}/result.json`, JSON.stringify(res, null, 1));
  return res;
}

/* ---- Step 2: rewrite the table of a used card while its old volumes are mounted ---- */
async function used(spec) {
  const card = `${ROOT}/out/target-quick/${spec.key}/used.hdf`;
  rmSync(card, { force: true });
  const s = new QuickSuite(spec, card, { name: 'used', cardBytes: 256 * MiB });
  s.prepare();
  const res = { test: 'used', key: spec.key, old: [], rewritten: [], restarted: [], format: [], problems: [] };
  const problem = (t) => { res.problems.push(t); s.note(`PROBLEM: ${t}`); };
  // A volume is right if the Info size is the partition's and the file is whole and inside it.
  const inspect = (phase, i, r, part) => {
    if (r.hung) { problem(`${phase}: DH${i} volcheck hung`); return; }
    if (r.phase !== 0) problem(`${phase}: DH${i} failed at ${PHASES[r.phase] ?? r.phase}, IoErr ${r.err}`);
    const want = part.blocks - 2;
    if (r.blocks !== want) problem(`${phase}: DH${i} Info says ${r.blocks} blocks, partition has ${want}`);
    if (r.phase === 0) {
      const st = scanStamps(card).get(r.tag);
      if (!st || st.count < r.mb * 2048) problem(`${phase}: DH${i} ${st?.count ?? 0} stamps on the card, wanted ${r.mb * 2048}`);
      else if (st.bad) problem(`${phase}: DH${i} ${st.bad} damaged stamps`);
      else if (st.min < part.first || st.max > part.last) problem(`${phase}: DH${i} data at blocks ${st.min}-${st.max}, partition is ${part.first}-${part.last}`);
    }
  };
  try {
    await s.boot();
    await s.step('Old card: 2 partitions, formatted, a file on each', async () => {
      await s.pickScsi(0);
      await s.split(2);
      await s.save();
      for (const i of [0, 1]) {
        const f = await s.format(i, `OLD${i}`);
        ok(f.ok, `format: ${f.error}`);
        const r = await s.volcheck(`DH${i}`, 5, { file: 'old.dat' });
        ok(passed(r), `volcheck: ${JSON.stringify(r)}`);
        res.old.push(r);
      }
      res.oldTable = readRdb(card).parts.map((p) => ({ name: p.name, low: p.low, high: p.high }));
      s.note(`old table: ${JSON.stringify(res.oldTable)}`);
    });
    await s.step('Restart: both auto-mount', async () => {
      await s.restart();
      const tasks = JSON.parse(await s.e.tool('winuae_tasks')).map((t) => t.name);
      await s.e.go();
      ok(tasks.includes('DH0') && tasks.includes('DH1'), `handlers running: ${tasks.join()}`);
      for (const i of [0, 1]) {
        const r = await s.volcheck(`DH${i}`, 5, { file: 'old.dat', flags: VC.VERIFY });
        ok(passed(r), `old file on DH${i}: ${JSON.stringify(r)}`);
      }
    });
    await s.step('Replace the table without restarting: 3 partitions at other cylinders, Save', async () => {
      await s.pickScsi(2);
      await s.deleteAll();
      await s.split(3);
      res.saveDialogs = await s.save();
      res.newTable = readRdb(card).parts.map((p) => ({ name: p.name, low: p.low, high: p.high }));
      s.note(`new table: ${JSON.stringify(res.newTable)}`);
      eq(res.newTable.length, 3, 'partitions written');
    });
    await s.e.shot(`${s.dir}/shots/table-saved.png`);
    const parts = [0, 1, 2].map((i) => s.bounds(i));
    await s.step('Format each and run volcheck: results recorded, judged below', async () => {
      for (const i of [0, 1, 2]) {
        const f = await s.format(i, `NEW${i}`);
        res.format.push(f);
        if (!f.ok) problem(`format of DH${i}: ${f.error}`);
        if (f.reformat) problem(`format of DH${i} went through a live mount: ${f.confirm.replace(/\n/g, ' / ')}`);
        const h = peek(card, parts[i].first);
        const sig = h.toString('latin1', 0, 3);
        if (sig !== 'DOS') problem(`after format, DH${i} has no DOS signature at its first block (found ${JSON.stringify(sig)})`);
      }
      await s.e.shot(`${s.dir}/shots/formatted.png`);
      for (const i of [0, 1, 2]) {
        const r = await s.volcheck(`DH${i}`, 5, { file: 'new.dat' });
        res.rewritten.push(r);
        inspect('after rewrite', i, r, parts[i]);
      }
    });
    await s.step('Restart and check again: results recorded, judged below', async () => {
      await s.restart();
      await s.e.shot(`${s.dir}/shots/restarted.png`);
      const tasks = JSON.parse(await s.e.tool('winuae_tasks')).map((t) => t.name);
      await s.e.go();
      s.note(`handlers after restart: ${tasks.filter((t) => /^DH/.test(t)).join() || 'none'}`);
      for (const i of [0, 1, 2]) {
        if (!(res.rewritten[i] && passed(res.rewritten[i]))) continue;
        const v = await s.volcheck(`DH${i}`, 5, { file: 'new.dat', flags: VC.VERIFY });
        res.restarted.push(v);
        if (!passed(v)) problem(`after restart: file written on DH${i} before the restart did not read back: ${v.hung ? 'hung' : `${PHASES[v.phase] ?? v.phase}, IoErr ${v.err}, bad ${v.bad}`}`);
      }
      for (const i of [0, 1, 2]) {
        const r = await s.volcheck(`DH${i}`, 5, { file: 'second.dat' });
        res.restarted.push(r);
        inspect('after restart', i, r, parts[i]);
      }
    });
    for (const q of s.requesters) problem(`requester "${q.title}" during ${q.context} (${q.shot})`);
    await s.step('Read the table back from the card', async () => { eq(readRdb(card).parts.length, 3, 'partitions on card'); });
    res.safe = res.problems.length === 0;
    res.done = true;
  } catch (err) { res.done = false; res.err = err.message; res.safe = false; } finally { await s.e.stop(); }
  res.requesters = s.requesters;
  res.log = s.log;
  writeFileSync(`${s.dir}/result.json`, JSON.stringify(res, null, 1));
  console.log(`  ${spec.key} used card: ${res.done ? '' : 'INCOMPLETE '}single-run flow safe: ${res.safe ? 'yes' : 'NO'} (${res.problems.length} problems)`);
  return res;
}

/* ---- The restart path: same used card, but restart after Save and format only then ---- */
async function restartPath(spec) {
  const card = `${ROOT}/out/target-quick/${spec.key}/restart.hdf`;
  rmSync(card, { force: true });
  const s = new QuickSuite(spec, card, { name: 'restart', cardBytes: 256 * MiB });
  s.prepare();
  const res = { test: 'restart', key: spec.key, problems: [], format: [] };
  const problem = (t) => { res.problems.push(t); s.note(`PROBLEM: ${t}`); };
  const check = (when, i, r, part) => {
    if (!passed(r)) { problem(`${when}: DH${i} ${r.hung ? 'hung' : `${PHASES[r.phase] ?? r.phase}, IoErr ${r.err}, bad ${r.bad}`}`); return; }
    if (r.blocks !== part.blocks - 2) problem(`${when}: DH${i} Info says ${r.blocks} blocks, partition has ${part.blocks - 2}`);
    if (r.flags & VC.VERIFY) return;
    const st = scanStamps(card).get(r.tag);
    if (!st || st.count !== r.mb * 2048 || st.bad) problem(`${when}: DH${i} stamps ${JSON.stringify(st)}`);
    else if (st.min < part.first || st.max > part.last) problem(`${when}: DH${i} data at ${st.min}-${st.max}, partition is ${part.first}-${part.last}`);
  };
  try {
    await s.boot();
    await s.step('Old card: 2 partitions, formatted, a file on each, restart', async () => {
      await s.pickScsi(0);
      await s.split(2);
      await s.save();
      for (const i of [0, 1]) {
        ok((await s.format(i, `OLD${i}`)).ok, 'format');
        ok(passed(await s.volcheck(`DH${i}`, 5, { file: 'old.dat' })), 'volcheck');
      }
      await s.restart();
    });
    await s.step('Replace the table with 3 partitions, Save, restart, no format before it', async () => {
      await s.pickScsi(2);
      await s.deleteAll();
      await s.split(3);
      await s.save();
      await s.restart();
    });
    const parts = [0, 1, 2].map((i) => s.bounds(i));
    await s.step('After the restart: format each, volcheck each: results recorded', async () => {
      await s.pickScsi(3);
      for (const i of [0, 1, 2]) {
        const f = await s.format(i, `NEW${i}`);
        res.format.push(f);
        if (!f.ok) problem(`format of DH${i}: ${f.error}`);
        if (!new RegExp(`mounted as DH${i}:`).test(f.confirm)) problem(`DH${i} is not mounted as itself: ${f.confirm.replace(/\n/g, ' / ')}`);
      }
      for (const i of [0, 1, 2]) check('after format', i, await s.volcheck(`DH${i}`, 5, { file: 'new.dat' }), parts[i]);
    });
    await s.step('Restart again: every file reads back: results recorded', async () => {
      await s.restart();
      for (const i of [0, 1, 2]) check('after restart', i, await s.volcheck(`DH${i}`, 5, { file: 'new.dat', flags: VC.VERIFY }), parts[i]);
    });
    for (const q of s.requesters) problem(`requester "${q.title}" during ${q.context} (${q.shot})`);
    res.safe = res.problems.length === 0;
    res.done = true;
  } catch (err) { res.done = false; res.safe = false; res.err = err.message; } finally { await s.e.stop(); }
  res.requesters = s.requesters;
  res.log = s.log;
  writeFileSync(`${s.dir}/result.json`, JSON.stringify(res, null, 1));
  console.log(`  ${spec.key} restart path: ${res.done ? '' : 'INCOMPLETE '}works: ${res.safe ? 'yes' : 'NO'} (${res.problems.length} problems)`);
  return res;
}

/* ---- Step 3: how big a partition does ROM FFS really handle? ---- */
// Files go after the root block in the middle, up to the end of the partition, then wrap to its start.
// So filling half the volume puts the next file against the end of the partition.
async function limitRun(spec, mbSize, { leadMb = 0 } = {}) {
  const tag = leadMb ? `${leadMb}+${mbSize}` : `${mbSize}`;
  const card = `${ROOT}/out/target-quick/${spec.key}/limit-${tag}.hdf`;
  rmSync(card, { force: true });
  const s = new QuickSuite(spec, card, { name: `limit-${tag}`, cardBytes: 4000000000, cpuMax: true });
  s.prepare();
  const res = { test: 'limit', key: spec.key, mbSize, leadMb, problems: [], files: [] };
  const problem = (t) => { res.problems.push(t); s.note(`PROBLEM: ${t}`); };
  const idx = leadMb ? 1 : 0, vol = `DH${idx}`;
  try {
    await s.boot();
    let part;
    await s.step(`Blank 4 GB card: ${leadMb ? `${leadMb} MB then ` : ''}one ${mbSize} MB partition, Save, Format`, async () => {
      await s.pickScsi(0);
      if (leadMb) await s.newSized(leadMb);
      await s.newSized(mbSize);
      await s.save();
      part = s.bounds(idx);
      const rdb = readRdb(card);
      res.table = rdb.parts.map((p) => ({ name: p.name, low: p.low, high: p.high, dosType: p.dosType.toString(16), maxTransfer: p.maxTransfer.toString(16), mask: p.mask.toString(16) }));
      res.cylBlocks = rdb.cylBlocks;
      res.partBytes = part.blocks * 512;
      res.endByte = (part.last + 1) * 512;
      s.note(`table ${JSON.stringify(res.table)}, cylBlocks ${rdb.cylBlocks}, ${vol} ${res.partBytes} bytes, ends at byte ${res.endByte}`);
      if (leadMb) { const f0 = await s.format(0, 'LEAD'); if (!f0.ok) problem(`format of DH0: ${f0.error}`); }
      const f = await s.format(idx, 'BIG');
      res.format = f;
      if (!f.ok) throw new Error(`format: ${f.error}`);
    });
    await s.step('Info the volume', async () => {
      const i = await s.volcheck(vol, 0, { flags: VC.INFO });
      res.info = { blocks: i.blocks, used: i.used, bsize: i.bsize, dtype: i.dtype, state: i.state, phase: i.phase, err: i.err };
      if (i.phase !== 0) throw new Error(`Info failed: ${PHASES[i.phase]} IoErr ${i.err}`);
      const want = part.blocks - 2;
      if (i.blocks !== want) problem(`Info says ${i.blocks} blocks, partition has ${want}`);
      if (i.dtype !== '444f5303') problem(`volume type ${i.dtype}`);
    });
    const distance = (st) => part.last - st.max;
    const fillThenEnd = async () => {
      // Half the partition, less FFS overhead (an extension block per 72 data blocks), less the test file and some margin.
      let toFill = Math.floor((part.blocks / 2) * (72 / 73) / 2048) - 64 - 24;
      let n = 0;
      const fillTo = async (mb) => {
        while (mb > 0) {
          const c = Math.min(mb, 512);
          const r = await s.volcheck(vol, c, { file: `fill${n}.dat`, flags: VC.WRITE });
          n++; res.files.push(r);
          if (!passed(r)) throw new Error(`fill failed: ${JSON.stringify(r)}`);
          mb -= c;
        }
      };
      const slow = spec.machine.includes('cpu_model=68000');
      res.fillCpu = slow ? '68020' : spec.machine[0].split('=')[1];
      if (slow) { s.setMachine(FILL_68020); await s.restart(); }
      await fillTo(toFill);
      if (slow) { s.setMachine(spec.machine); await s.restart(); }
      let r, st;
      for (let round = 0; round < 3; round++) {
        r = await s.volcheck(vol, 64, { file: `end${round}.dat` });
        res.files.push(r);
        if (!passed(r)) { problem(`end${round}.dat: ${r.hung ? 'hung' : `${PHASES[r.phase]} IoErr ${r.err} bad ${r.bad}`}`); return { r, st: null }; }
        st = scanStamps(card).get(r.tag);
        s.note(`end${round}.dat: blocks ${st?.min}-${st?.max}, last block of the partition ${part.last}, ${st ? ((distance(st) * 512) / MiB).toFixed(1) : '?'} MB from its end`);
        if (!st || st.max >= part.last - 32 * 2048) break;
        if (st.min < part.first + (part.blocks >> 1)) break;
        await fillTo(Math.floor((distance(st) * 512) / MiB) - 8);
      }
      return { r, st };
    };
    const end = await s.step('Fill, then 64 MB near the end of the partition', fillThenEnd);
    await s.step('Past the end: 128 MB more, wrapping to the start of the partition', async () => {
      const r = await s.volcheck(vol, 128, { file: 'tail.dat' });
      res.tail = r;
      if (!passed(r)) problem(`tail.dat: ${r.hung ? 'hung' : `${PHASES[r.phase]} IoErr ${r.err} bad ${r.bad}`}`);
    });
    await s.step('Host check: every file whole, unharmed and inside the partition; table intact', async () => {
      const st = scanStamps(card);
      res.stamps = {};
      for (const f of [...res.files, res.tail]) {
        if (!f || !f.tag) continue;
        const x = st.get(f.tag), want = f.wrote / 512;
        res.stamps[f.file] = x ? { count: x.count, bad: x.bad, min: x.min, max: x.max, maxByte: (x.max + 1) * 512 } : null;
        if (!x) { problem(`${f.file}: no stamps on the card`); continue; }
        if (x.count !== want) problem(`${f.file}: ${x.count} sectors on the card, wrote ${want}`);
        if (x.bad) problem(`${f.file}: ${x.bad} damaged sectors`);
        if (x.min < part.first || x.max > part.last) problem(`${f.file}: at blocks ${x.min}-${x.max}, outside the partition ${part.first}-${part.last}`);
      }
      for (const [t, x] of st) {
        if (![...res.files, res.tail].some((f) => f && f.tag === t)) problem(`stamps of an unknown file ${t.toString(16)} at ${x.min}-${x.max}`);
      }
      readRdb(card);
      const highest = Math.max(...[...st.values()].map((x) => x.max));
      res.highestByte = (highest + 1) * 512;
      res.nearEndMb = end.st ? ((part.last - end.st.max) * 512) / MiB : null;
      s.note(`highest data at byte ${res.highestByte}; 64 MB file ended ${res.nearEndMb?.toFixed(1)} MB before the partition's end`);
    });
    await s.step('Restart: the volume mounts and the files read back', async () => {
      await s.restart();
      const i = await s.volcheck(vol, 0, { flags: VC.INFO });
      res.infoAfter = { blocks: i.blocks, used: i.used, state: i.state, phase: i.phase };
      if (i.phase !== 0) problem(`after restart, Info failed: ${PHASES[i.phase]} IoErr ${i.err}`);
      for (const f of [...res.files, res.tail]) {
        if (!f || !passed(f) || !f.wrote) continue;
        // The host has checked every fill sector. Reading 512 MB back on a 68000 would take half an hour.
        if (!(spec.machine.includes('cpu_model=68000') ? /^(end|tail)/ : /^(end|tail|fill0)/).test(f.file)) continue;
        const r = await s.volcheck(vol, f.mb, { file: f.file, flags: VC.VERIFY });
        if (!passed(r)) problem(`after restart ${f.file}: ${r.hung ? 'hung' : `${PHASES[r.phase]} IoErr ${r.err} bad ${r.bad}`}`);
        if (r.equal !== f.mb * MiB) problem(`after restart ${f.file}: ${r.equal} bytes equal of ${f.mb * MiB}`);
      }
    });
    for (const q of s.requesters) problem(`requester "${q.title}" during ${q.context} (${q.shot})`);
    res.done = true;
    res.ok = res.problems.length === 0 && res.nearEndMb !== null && res.nearEndMb < 64;
  } catch (err) { res.done = false; res.ok = false; res.err = err.message; } finally { await s.e.stop(); }
  res.requesters = s.requesters;
  res.log = s.log;
  writeFileSync(`${s.dir}/result.json`, JSON.stringify(res, null, 1));
  console.log(`  ${spec.key} ${tag} MB: ${res.done ? '' : 'INCOMPLETE '}reads back correctly: ${res.ok ? 'yes' : 'NO'} (${res.problems.length} problems)`);
  rmSync(card, { force: true });          // up to 4 GB each once written
  return res;
}
// Finishing is not passing: a run that finished with problems, or never got near the end, has res.ok false.
export const limitPassed = (...runs) => runs.every((r) => r.done === true && r.ok === true);
const limitErr = (...runs) => runs.filter((r) => !limitPassed(r)).map((r) => `${r.leadMb ? `${r.leadMb}+` : ''}${r.mbSize} MB: ${r.err || r.problems.join('; ') || 'did not reach the end of the partition'}`).join(' | ') || undefined;
const limit = async (spec) => { const a = await limitRun(spec, 3584); const b = await limitRun(spec, 2048); return { test: 'limit', key: spec.key, pass: limitPassed(a, b), err: limitErr(a, b), a, b }; };

// The layout the wizard makes: the partition starts high and ends near the top of the card.
const offset = async (spec) => { const a = await limitRun(spec, 2048, { leadMb: 1760 }); return { test: 'offset', key: spec.key, pass: limitPassed(a), err: limitErr(a), a }; };

/* ---- The IDE check: is scsi.device an internal IDE driver? ---- */
// expect: what g_qs_is_ide must be. controller: where the card sits. env: extra files the boot needs, from these variables.
// driver, noDevice, inRam: what must be running for the answer to mean anything. A 0 from a machine with no driver proves nothing about that driver.
const SOFTLOAD = ['If NOT EXISTS ENV:loaded', 'Echo >ENV:loaded x', 'LoadModule scsi.device', 'EndIf'];
const IDE_CHECK = {
  ks205a: { expect: 1 }, ks205b: { expect: 1 }, ks30: { expect: 1 }, ks31a600: { expect: 1 }, ks31a1200: { expect: 1 },
  ks314: { expect: 1 }, ks32: { expect: 1 }, ks31a4000: { expect: 1 }, ks314a4000: { expect: 1 }, ks32a4000: { expect: 1 },
  a3000ks204: { expect: 0, controller: 'scsi0', driver: true }, a3000ks31: { expect: 0, controller: 'scsi0', driver: true }, a3000ks32: { expect: 0, controller: 'scsi0', driver: true },
  a500pks204: { expect: 0, noDevice: true },
  // An A4000T has both ports. scsi.device is its IDE driver; the SCSI one is called "NCR scsi.device".
  a4000tks31: { expect: 1, also: 'NCR scsi.device' }, a4000tks32: { expect: 1, also: 'NCR scsi.device' },
  // An A3000 soft-kicked with another machine's ROM.
  a3000roma4000: { expect: 0, controller: 'scsi0', noDevice: true }, a3000roma1200: { expect: 0, controller: 'scsi0', noDevice: true },
  // SetPatch, as every real system runs it. The 3.2 one has its scsi.device LED patch on by default.
  ks314setpatch: { expect: 1, rom: 'ks314', env: { SetPatch: 'QUICK_SETPATCH_314' }, startup: ['SetPatch'] },
  ks32setpatch: { expect: 1, rom: 'ks32', env: { SetPatch: 'QUICK_SETPATCH_32' }, startup: ['SetPatch'] },
  // A driver loaded from disk: the ROM underneath says nothing about what runs.
  // LoadModule reboots every time here, so the marker on the ENV volume stops it after the first.
  ks31softload: { expect: 0, inRam: true, rom: 'ks31a1200', env: { LoadModule: 'QUICK_LOADMODULE', 'scsi.device': 'QUICK_MODULE_IDE' }, startup: SOFTLOAD },
  a3000softload: { expect: 0, inRam: true, rom: 'a3000ks31', controller: 'scsi0', env: { LoadModule: 'QUICK_LOADMODULE', 'scsi.device': 'QUICK_MODULE_A3000' }, startup: SOFTLOAD },
};

// Library bases by name from exec's device list, for the record: what is really running.
async function listedDevice(e, name) {
  const sys = await e.u32(4);
  for (let n = await e.u32(sys + 350); n; n = await e.u32(n)) {
    if (!(await e.u32(n))) break;
    if ((await e.cstr(await e.u32(n + 10), 40)) !== name) continue;
    const vec = async (lvo) => ((await e.u16(n + lvo)) === 0x4EF9 ? e.u32(n + lvo + 2) : 0);
    const idp = await e.u32(n + 24);
    return { id: (await e.cstr(idp, 60)).trim(), idAt: idp, version: `${await e.u16(n + 20)}.${await e.u16(n + 22)}`, beginIO: await vec(-30), abortIO: await vec(-36), open: await vec(-6) };
  }
  return null;
}

async function bootFor(s, c) {
  if (c.startup !== SOFTLOAD) { await s.boot(); return; }
  await s.step('Boot: LoadModule, its reboot, then HDPart and volcheck are running', async () => {
    // The reboot drops the debugger's connection, and the emulator listens again. Wait for that, then attach again.
    await s.e.start();
    const listens = () => (readFileSync(s.e.logPath, 'latin1').match(/GDB: listening/g) || []).length;
    await s.e.waitFor('the reboot', async () => listens() >= 2, 60000);
    await s.e.waitFor('debugger attached', async () => {
      try { await s.e.tool('winuae_attach'); return true; } catch (err) { return err.message.includes('Already connected'); }
    }, 30000);
    await s.e.go();
    await s.waitUp();
  });
}

async function ideCheck(spec) {
  const c = IDE_CHECK[spec.key], card = `${ROOT}/out/target-quick/${spec.key}/ide.hdf`;
  rmSync(card, { force: true });
  const files = Object.fromEntries(Object.entries(c.env || {}).map(([name, v]) => [name, process.env[v]]));
  const s = new QuickSuite({ ...spec, controller: c.controller, startup: c.startup, files }, card, { name: 'ide', cardBytes: 16 * MiB });
  s.prepare();
  const res = { test: 'ide', key: spec.key, expect: c.expect };
  try {
    await bootFor(s, c);
    await s.step(`Quick setup: IDE check gives ${c.expect}`, async () => {
      res.driver = await listedDevice(s.e, 'scsi.device');
      res.also = c.also ? await listedDevice(s.e, c.also) : undefined;
      res.got = (await s.e.g32('g_qs_is_ide')) | 0;
      s.note(`scsi.device: ${res.driver ? `${res.driver.id}, BeginIO $${res.driver.beginIO.toString(16)}, Open $${res.driver.open.toString(16)}` : 'not in the device list'}`);
      if (c.also) s.note(`${c.also}: ${res.also ? res.also.id : 'not in the device list'}`);
      if (c.noDevice) ok(!res.driver, 'expected no scsi.device on this machine');
      if (c.driver || c.inRam) ok(res.driver, 'expected a scsi.device in the device list');
      // Outside the ROM, not just below it: an A3000's RAM is above $1000000.
      if (c.inRam) ok(res.driver.beginIO && (res.driver.beginIO < 0xF80000 || res.driver.beginIO >= 0x1000000), `expected the loaded driver running, BeginIO is $${res.driver.beginIO.toString(16)}`);
      eq(res.got, c.expect, 'g_qs_is_ide');
    });
    res.pass = true;
  } catch (err) { res.pass = false; res.err = err.message; } finally { await s.e.stop(); }
  writeFileSync(`${s.dir}/result.json`, JSON.stringify(res, null, 1));
  return res;
}

/* ---- Quick setup: the entry point, the card screen and the refusals ---- */
const QS = { CONTINUE: 1, CANCEL: 0, NO_PORT: 1, UNPROVEN: 2, NO_CARD: 3, TWO_DRIVES: 4, UNREADABLE: 5, BOOTED: 6, BOOT_UNKNOWN: 7, NAME_TAKEN: 8, TOO_SMALL: 9, BACK: 2, WORK: 11, GAMES: 12, DEMOS: 13, ERASE: 1 };   // src/gui.c
const TECHNICAL = /max\s*transfer|mask|cylinder|\brdb\b|dos\s*type/i;
const SHOTS = `${ROOT}/out/target-quick/shots`;

function hashFile(file) {
  const h = createHash('sha256'), fd = openSync(file, 'r'), buf = Buffer.alloc(16 * MiB);
  for (let n; (n = readSync(fd, buf, 0, buf.length, null)) > 0;) h.update(buf.subarray(0, n));
  closeSync(fd);
  return h.digest('hex');
}

// A used card made by other tools: rdbtool's table with xdftool's volumes. amitools 0.4.0 cannot address a
// partition inside an image, so each volume is built alone and copied to where the table says it lives.
function makeAmigaCard(card, bytes, parts) {
  rmSync(card, { force: true });
  const pct = Math.floor(100 / parts.length);
  const add = parts.flatMap((p) => ['+', 'add', `size=${pct}%`, `name=${p.dev}`, 'dostype=ffs+intl', ...(p.bootable ? ['bootable=1', `pri=${p.pri ?? 0}`] : [])]);
  execFileSync('rdbtool', [card, 'create', `size=${bytes}`, '+', 'init', ...add], { stdio: 'pipe' });
  const r = readRdb(card), fd = openSync(card, 'r+');
  parts.forEach((p, i) => {
    const t = r.parts[i], tmp = `${card}.vol.hdf`;
    rmSync(tmp, { force: true });
    execFileSync('xdftool', [tmp, 'create', `chs=${t.high - t.low + 1},1,${r.cylBlocks}`, '+', 'format', p.vol, 'ffs+intl', ...(p.commands || [])], { stdio: 'pipe' });
    const d = readFileSync(tmp);
    writeSync(fd, d, 0, d.length, t.low * r.cylBlocks * 512);
    rmSync(tmp, { force: true });
  });
  closeSync(fd);
}

// One boot with one card. body(s, card) runs the checks. The card's hash after the emulator stops must equal the
// one taken with it running, unless the test wrote to it. Returns one line of the summary.
async function cardRun(spec, name, { bytes = 256 * MiB, make, keep = {}, cpuMax = false } = {}, body) {
  const card = `${ROOT}/out/target-quick/${spec.key}/card-${name}.hdf`;
  rmSync(card, { force: true });
  if (make) make(card);
  const s = new QuickSuite({ ...spec, ...keep }, card, { name: `card-${name}`, cardBytes: bytes, cpuMax });
  s.prepare();
  mkdirSync(SHOTS, { recursive: true });
  const fromStart = spec.noCard || keep.noCard ? null : hashFile(card);
  const r = { scenario: name, pass: false };
  let held = null;
  try {
    await bootFor(s, keep.bootCheck ?? {});
    held = await body(s, card, fromStart);
    r.pass = true;
  } catch (err) { r.err = err.message; } finally { await s.e.stop(); }
  if (r.pass && held) {
    for (const [file, was] of Object.entries(held)) {
      const now = hashFile(file);
      if (now !== was) { r.pass = false; r.err = `${path.basename(file)} changed after the emulator stopped`; }
    }
  }
  rmSync(card, { force: true });
  rmSync(`${card}.2`, { force: true });
  console.log(`  ${r.pass ? 'ok  ' : 'FAIL'} ${spec.key} ${name}${r.err ? `: ${r.err}` : ''}`);
  return r;
}

// Reads pause the guest, and a window can be seen before it has been drawn in: let it run first.
const qsShot = async (s, name) => { await s.e.go(); await sleep(1500); await s.e.shot(`${SHOTS}/quick-${s.key}-${name}.png`); };
const panelShown = async (s) => (await s.e.gadgets(await s.main())).some((g) => g.id === GID.QUICK);

async function openWizard(s, via = 'button') {
  if (via === 'menu') await s.e.pickMenu(1, 6); else if (via === 'button') await s.clickMain(GID.QUICK); else await via();
  const w = await s.e.waitWin('Quick setup', 120000);
  eq(await s.e.g32('g_qs_screen'), 2, 'g_qs_screen while open');
  const wide = await s.e.i16(w + 8), high = await s.e.i16(w + 10);
  ok(wide <= 640 && high <= 200, `window is ${wide}x${high}: must fit a 640x200 screen`);
  return w;
}
async function closed(s) {
  await s.e.waitFor('wizard closed', async () => (await s.e.g32('g_qs_screen')) === 0 && !(await s.e.win('Quick setup')));
  await s.idle();
}
// The figures the wizard will lay out from must add up to the drive, and its layout must stay on the drive.
async function checkGeometry(s) {
  const e = s.e, g = await e.mem(e.addr('g_qs_geo'), 20), m = await e.mem(e.addr('g_qs_layout'), 28);
  const cyl = g.readUInt32BE(4), heads = g.readUInt32BE(8), secs = g.readUInt32BE(12), total = g.readUInt32BE(16);
  const product = cyl * heads * secs, cb = m.readUInt32BE(16), usable = (m.readUInt32BE(24) + 1) * cb;
  s.note(`geometry ${cyl} x ${heads} x ${secs}, drive ${total} blocks, usable ${usable}`);
  ok(Math.abs(product - total) <= Math.floor(total / 64), `geometry gives ${product} blocks, the drive has ${total}`);
  eq(cb, heads * secs, 'blocks per cylinder of the layout');
  ok(usable <= total, `layout ends at block ${usable}, the drive has ${total}`);
  return { total, usable, cyl, heads, secs };
}
// Close the card screen and read back what it said.
async function cardScreen(s) {
  const e = s.e, w = await e.waitWin('Quick setup: your card', 120000);
  eq(await e.g32('g_qs_refusal'), 0, 'refusal');
  const said = { found: await e.gstr('g_qs_found'), size: await e.gstr('g_qs_size'), holds: await e.gstr('g_qs_holds') };
  s.note(`card screen: ${JSON.stringify(said)}`);
  ok(!TECHNICAL.test(Object.values(said).join(' ')), `technical word on the screen: ${JSON.stringify(said)}`);
  ok(said.found.length > 0, 'names the card');
  const usable = await checkGeometry(s);
  await e.go();
  return { w, said, usable };
}
// The main window must look as if nothing happened.
async function mainUntouched(s) {
  eq(await s.e.g32('g_have_model'), 0, 'no model in the main window');
  ok((await s.status()).includes('Select a driver'), `status: ${await s.status()}`);
}

// Open the wizard, expect refusal `code` saying `words`, and prove the cards were not written.
async function refused(s, files, code, words, shot, via) {
  const e = s.e, held = Object.fromEntries(files.map((f) => [f, hashFile(f)]));
  await s.step(`Refused with reason ${code}, no technical words, nothing written`, async () => {
    const w = await openWizard(s, via);
    await e.waitFor('refusal chosen', async () => (await e.g32('g_qs_refusal')) !== 0);
    const said = await e.gstr('g_qs_said', 0, 500);
    s.note(`refusal: ${said.replace(/\n/g, ' / ')}`);
    eq(await e.g32('g_qs_refusal'), code, 'refusal');
    ok(words.test(said), `refusal text ${JSON.stringify(said)} does not match ${words}`);
    ok(!TECHNICAL.test(said), `technical word in: ${said}`);
    ok(/Nothing was changed/.test(said), 'says nothing changed');
    await qsShot(s, shot);
    await e.clickClose(w, OK);
    await closed(s);
    await mainUntouched(s);
    for (const [f, was] of Object.entries(held)) eq(hashFile(f), was, `hash of ${path.basename(f)} after the refusal`);
  });
  return held;
}

// The next geometry answer states a capacity and leaves heads at zero. Its second argument is the DeviceInfo.
const ZERO_CHS_TOTAL = 520000;
const zeroHeadsNext = (s) => s.e.failNextCall('dev_geometry', () => s.clickMain(GID.QUICK), 0, async (sp) => {
  const info = await s.e.u32(sp + 8);
  // block_bytes, cylinders, heads, sectors, total_blocks, has_media
  await s.e.tool('winuae_memory_write', { address: info, data: [512, 1000, 0, 63, ZERO_CHS_TOTAL, 1].map(long).join('') });
});

const USED = [{ dev: 'DH0', vol: 'Workbench', bootable: true }, { dev: 'DH1', vol: 'Work' }];
const AFTER_CARD = { bootPri: 5 };       // Like a floppy: starts ahead of the card, whatever the card holds.
const pcCard = (card) => {
  const fd = openSync(card, 'w'), mbr = Buffer.alloc(512), bs = Buffer.alloc(512);
  ftruncateSync(fd, 256 * MiB);
  mbr.set([0x80, 1, 1, 0, 0x06, 0xFE, 0x3F, 0x20], 446);
  mbr.writeUInt32LE(63, 454); mbr.writeUInt32LE(500000, 458);
  mbr[510] = 0x55; mbr[511] = 0xAA;
  bs.set([0xEB, 0x3C, 0x90], 0); bs.write('MSDOS5.0', 3, 'latin1'); bs[510] = 0x55; bs[511] = 0xAA;
  writeSync(fd, mbr, 0, 512, 0); writeSync(fd, bs, 0, 512, 63 * 512);
  closeSync(fd);
};


// Screens 3 and 4. Everything is read from the program's own variables, so the test sees what it drew.
const MASK = { Work: 1, Games: 2, Demos: 4 };
async function chooseScreen(s, via) {      // from the card screen to the choice screen
  const e = s.e;
  await openWizard(s, via);
  const { w } = await cardScreen(s);
  await e.clickClose(w, QS.CONTINUE);
  const c = await e.waitWin('Quick setup: partitions', 120000);
  await e.waitFor('g_qs_screen on the choice screen', async () => (await e.g32('g_qs_screen')) === 3, 5000);    // set just after the window opens
  return c;
}
async function chooseState(s) {
  const e = s.e, n = await e.u32(e.addr('g_qs_layout') + 36);
  const state = { mask: await e.g32('g_qs_mask'), parts: n, msg: await e.gstr('g_qs_msg', 0, 100) };
  ok(!TECHNICAL.test(state.msg), `technical word in: ${state.msg}`);
  s.note(`choice: ${JSON.stringify(state)}`);
  return state;
}
async function tick(s, w, id) { await s.e.click(w, id); await s.e.go(); await sleep(600); }
async function confirmScreen(s) {     // from the choice screen to the last check
  const e = s.e;
  await e.clickClose(await e.waitWin('Quick setup: partitions', 120000), QS.CONTINUE);
  const w = await e.waitWin('Quick setup: ready?', 120000);
  await e.waitFor('g_qs_screen on the last check', async () => (await e.g32('g_qs_screen')) === 4, 5000);
  const rows = [];
  for (let i = 0; i < 4; i++) { const r = await e.gstr('g_qs_rows', i * 60, 60); if (r) rows.push(r); }
  const warn = await e.gstr('g_qs_warn', 0, 100);
  s.note(`last check: ${JSON.stringify(rows)} ${JSON.stringify(warn)}`);
  ok(!TECHNICAL.test(rows.join(' ') + warn), 'technical word on the last check');
  return { w, rows, warn };
}
const sizeOf = (row) => Number(/(\d+) MB/.exec(row)[1]);

const CARD_RUNS = {
  panel: (spec) => cardRun(spec, 'panel', {}, async (s, card, was) => {
    const e = s.e;
    await s.step('Quick setup: button shows until a driver is picked', async () => {
      ok(await panelShown(s), 'button at startup');
      eq(await e.g32('g_qs_screen'), 0, 'not open at startup');
      await qsShot(s, 'panel');
      const w = await openWizard(s);
      // The idle check must not pass while the program waits on anything but its main window.
      const notIdle = await s.idle(3000).then(() => false, () => true);
      ok(notIdle, 'idle check passed with a dialog open');
      await e.clickClose(w, QS.CANCEL);
      await closed(s);
      ok(await panelShown(s), 'button is still there after Cancel');
      await mainUntouched(s);
      await s.pickScsi(0);
      ok(!(await panelShown(s)), 'button is gone once a driver is picked');
      for (let i = 0; i < 40 && ((await e.g32('g_drv_idx')) | 0) !== 0; i++) { await s.clickMain(GID.DEVICE); await sleep(500); }
      eq((await e.g32('g_drv_idx')) | 0, 0, 'back at the prompt');
      await s.idle();
      ok(!(await panelShown(s)), 'stays gone for the session');
      await qsShot(s, 'panel-gone');
    });
    await s.step('Menu item opens it, also after the panel has gone', async () => {
      await e.clickClose(await openWizard(s, 'menu'), QS.CONTINUE);
      await e.clickClose(await e.waitWin('Quick setup: partitions', 120000), QS.CANCEL);
      await closed(s);
    });
    eq(hashFile(card), was, 'hash of the card');
    return { [card]: was };
  }),
  blank: (spec) => cardRun(spec, 'blank', {}, async (s, card, was) => {
    await s.step('Quick setup: blank card shows Empty', async () => {
      await openWizard(s);
      const { w, said } = await cardScreen(s);
      eq(said.holds, 'Empty', 'Now holds');
      ok(/^25[56] MB$/.test(said.size), `size: ${said.size}`);
      await qsShot(s, 'blank');
      await s.e.clickClose(w, QS.CANCEL);
      await closed(s);
      await mainUntouched(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return { [card]: was };
  }),
  choose: (spec) => cardRun(spec, 'choose', {}, async (s, card, was) => {
    await s.step('choose: ticking updates the layout', async () => {
      const e = s.e, c = await chooseScreen(s);
      let st = await chooseState(s);
      eq(st.mask, MASK.Work | MASK.Games, 'default ticks');
      eq(st.parts, 3, 'System, Work, Games');
      ok(/^The rest of the card is shared evenly between your choices\.$/.test(st.msg), st.msg);
      await qsShot(s, 'choose-default');
      await tick(s, c, QS.DEMOS);
      st = await chooseState(s);
      eq(st.mask, 7, 'all ticked'); eq(st.parts, 4, 'System and three');
      await qsShot(s, 'choose-all');
      for (const id of [QS.WORK, QS.GAMES, QS.DEMOS]) await tick(s, c, id);
      st = await chooseState(s);
      eq(st.mask, 0, 'none ticked'); eq(st.parts, 1, 'System only');
      eq(st.msg, 'Only System. The rest of the card stays unused.', 'message');
      await qsShot(s, 'choose-none');
      eq(hashFile(card), was, 'hash of the card while choosing');
      await e.clickClose(c, QS.CANCEL);
      await closed(s);
    });
    await s.step('choose: Back and Cancel write nothing', async () => {
      const e = s.e, c = await chooseScreen(s);
      await e.clickClose(c, QS.BACK);
      const w = await e.waitWin('Quick setup: your card', 120000);
      eq(await e.g32('g_qs_screen'), 2, 'back on the card screen');
      eq(hashFile(card), was, 'hash of the card after Back');
      await e.clickClose(w, QS.CONTINUE);
      const c2 = await e.waitWin('Quick setup: partitions', 120000);
      await e.clickClose(c2, QS.CANCEL);
      await closed(s);
      await mainUntouched(s);
      eq(hashFile(card), was, 'hash of the card after Cancel');
    });
    await s.step('choose: a layout that fails leaves Continue off and no stale table', async () => {
      const e = s.e, c = await chooseScreen(s);
      await e.failNextCall('quick_layout', () => e.click(c, QS.DEMOS), (-20) >>> 0);
      await e.go(); await sleep(600);
      const st = await chooseState(s);
      eq(st.parts, 0, 'no partitions after a failed layout');
      ok(/do not fit/.test(st.msg), `message: ${st.msg}`);
      const cont = await e.gadget(c, QS.CONTINUE);
      ok(cont.flags & 0x100, 'Continue is disabled');
      await qsShot(s, 'choose-failed');
      // A Continue already queued behind the failing tick arrives as if the gadget were on. The harness can't queue
      // clicks, so turn the gadget back on behind the program's back: the click reaches the loop the same way.
      await e.tool('winuae_memory_write', { address: cont.addr + 12, data: (cont.flags & ~0x100).toString(16).padStart(4, '0') });
      await e.click(c, QS.CONTINUE);
      await e.go(); await sleep(800);
      eq(await e.g32('g_qs_screen'), 3, 'still on the choice screen after a Continue that does not fit');
      ok(await e.win('Quick setup: partitions'), 'the choice screen is still open');
      await tick(s, c, QS.DEMOS);                 // back to the two defaults
      const ok2 = await chooseState(s);
      eq(ok2.parts, 3, 'a good layout again');
      ok(!((await e.gadget(c, QS.CONTINUE)).flags & 0x100), 'Continue is enabled again');
      await e.clickClose(c, QS.CANCEL);
      await closed(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return { [card]: was };
  }),
  confirm: (spec) => cardRun(spec, 'confirm', {}, async (s, card, was) => {
    await s.step('confirm: table lists device, volume, size', async () => {
      const e = s.e;
      await chooseScreen(s);
      const { w, rows, warn } = await confirmScreen(s);
      eq(rows.length, 3, 'rows');
      ok(/^DH0:\s+System\s+\d+ MB\s+starts the Amiga$/.test(rows[0]), rows[0]);
      ok(/^DH1:\s+Work\s+\d+ MB$/.test(rows[1]), rows[1]);
      ok(/^DH2:\s+Games\s+\d+ MB$/.test(rows[2]), rows[2]);
      const total = rows.reduce((a, r) => a + sizeOf(r), 0);
      ok(total <= 256 && total >= 250, `sizes add to ${total} MB on a 256 MB card`);
      eq(warn, `Erases ${(await e.gstr('g_qs_found')).slice(0, 31)}. This cannot be undone.`, 'warning');
      await qsShot(s, 'confirm');
      eq(hashFile(card), was, 'hash of the card on the last check');
      await e.clickClose(w, QS.BACK);
      const c = await e.waitWin('Quick setup: partitions', 120000);
      await e.waitFor('Back goes to the choice', async () => (await e.g32('g_qs_screen')) === 3, 5000);
      eq(await e.g32('g_qs_mask'), 3, 'still the default ticks');
      await e.clickClose(c, QS.CANCEL);
      await closed(s);
    });
    await s.step('confirm: the ticks are kept', async () => {
      const e = s.e, c = await chooseScreen(s);
      await tick(s, c, QS.DEMOS);
      eq(await e.g32('g_qs_mask'), 7, 'all ticked');
      const { w, rows } = await confirmScreen(s);
      eq(rows.length, 4, 'rows');
      await e.clickClose(w, QS.BACK);
      const c2 = await e.waitWin('Quick setup: partitions', 120000);
      eq(await e.g32('g_qs_mask'), 7, 'ticks after Back');
      await e.clickClose(c2, QS.CANCEL);
      await closed(s);
    });
    await s.step('confirm: Cancel writes nothing', async () => {
      const e = s.e;
      await chooseScreen(s);
      const { w } = await confirmScreen(s);
      await e.clickClose(w, QS.CANCEL);
      await closed(s);
      await mainUntouched(s);
      eq(hashFile(card), was, 'hash after Cancel');
    });
    return { [card]: was };
  }),
  confirm8: (spec) => cardRun(spec, 'confirm8', { bytes: 8000000000 }, async (s, card, was) => {
    await s.step('confirm: 8 GB card, Work only, says what stays unused and the table matches', async () => {
      const e = s.e, c = await chooseScreen(s);
      const first = await chooseState(s);         // the default ticks: half the map is empty, and this says why
      ok(/^This Amiga can use the first 4 GB, shared evenly between them\.$/.test(first.msg), first.msg);
      await qsShot(s, 'choose-first-4gb');
      await tick(s, c, QS.GAMES);                 // Work only
      const st = await chooseState(s);
      ok(/^Each partition is limited to 2 GB, so \d+ MB stays unused\.$/.test(st.msg), st.msg);
      await qsShot(s, 'choose-limit');
      const { w, rows } = await confirmScreen(s);
      eq(rows.length, 2, 'rows');
      const m = await e.mem(e.addr('g_qs_layout'), 28), cb = m.readUInt32BE(16), hi = m.readUInt32BE(24);
      const usable = Math.floor((hi + 1) * cb / 2048);
      const spare = Number(/so (\d+) MB/.exec(st.msg)[1]);
      ok(Math.abs(usable - rows.reduce((a, r) => a + sizeOf(r), 0) - spare) <= 2, `sizes + unused = ${usable} MB`);
      ok(sizeOf(rows[1]) >= 2000 && sizeOf(rows[1]) <= 2048, rows[1]);
      ok((hi + 1) * cb <= 8388608, `layout reaches block ${(hi + 1) * cb}, past 4 GiB`);
      ok(Math.abs(spare - 1546) <= 8, `${spare} MB stays unused, expected about 1546`);
      await qsShot(s, 'confirm8');
      await e.clickClose(w, QS.CANCEL);
      await closed(s);
    });
    return { [card]: was };
  }),
  used: (spec) => cardRun(spec, 'used', { make: (c) => makeAmigaCard(c, 256 * MiB, USED), keep: AFTER_CARD }, async (s, card) => {
    const was = hashFile(card);
    await s.step('Used Amiga card lists its partitions: its own mounted DH0 does not refuse it', async () => {
      const tasks = JSON.parse(await s.e.tool('winuae_tasks')).map((t) => t.name);
      await s.e.go();
      ok(tasks.includes('DH0') && tasks.includes('DH1'), `the card's partitions are mounted: ${tasks.join()}`);
      await openWizard(s);
      const { w, said } = await cardScreen(s);
      eq(said.holds, 'DH0 Workbench, DH1 Work', 'Now holds');
      await qsShot(s, 'used');
      await s.e.clickClose(w, QS.CONTINUE);
      await s.e.clickClose(await s.e.waitWin('Quick setup: partitions', 120000), QS.CANCEL);
      await closed(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return { [card]: was };
  }),
  many: (spec) => cardRun(spec, 'many', { make: (c) => makeAmigaCard(c, 256 * MiB, ['One', 'Two', 'Three', 'Four', 'Five', 'Six'].map((n, i) => ({ dev: `DH${i}`, vol: `Volume${n}` }))), keep: AFTER_CARD }, async (s, card) => {
    const was = hashFile(card);
    await s.step('A long list of partitions is cut, and says how many are left', async () => {
      await openWizard(s);
      const { w, said } = await cardScreen(s);
      ok(said.holds.startsWith('DH0 VolumeOne, DH1 VolumeTwo') && /and \d more$/.test(said.holds) && said.holds.length <= 46, `Now holds: ${said.holds}`);
      await qsShot(s, 'many');
      await s.e.clickClose(w, QS.CANCEL);
      await closed(s);
    });
    return { [card]: was };
  }),
  pc: (spec) => cardRun(spec, 'pc', { make: pcCard }, async (s, card, was) => {
    await s.step('PC card says Not an Amiga disk', async () => {
      await openWizard(s);
      const { w, said } = await cardScreen(s);
      eq(said.holds, 'Not an Amiga disk', 'Now holds');
      await qsShot(s, 'pc');
      await s.e.clickClose(w, QS.CANCEL);
      await closed(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return { [card]: was };
  }),
  gb8: (spec) => cardRun(spec, 'gb8', { bytes: 8000000000 }, async (s, card, was) => {
    await s.step('8 GB card says this Amiga can use 4 GB', async () => {
      await openWizard(s);
      const { w, said } = await cardScreen(s);
      eq(said.size, '8 GB (this Amiga can use 4 GB)', 'Size');
      await qsShot(s, 'gb8');
      await s.e.clickClose(w, QS.CANCEL);
      await closed(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return { [card]: was };
  }),
  gb4: (spec) => cardRun(spec, 'gb4', { bytes: 4000000000 }, async (s, card, was) => {
    await s.step('4 GB card fits whole, so there is no note', async () => {
      await openWizard(s);
      const { w, said, usable } = await cardScreen(s);
      eq(said.size, '4 GB', 'Size');
      ok(usable.total - usable.usable <= Math.floor(usable.total / 64), `uses ${usable.usable} of ${usable.total} blocks`);
      await s.e.clickClose(w, QS.CANCEL);
      await closed(s);
    });
    return { [card]: was };
  }),
  nocard: (spec) => cardRun(spec, 'nocard', { keep: { noCard: true } }, async (s) => {
    // No card, so there is no file to hash: the check is that the refusal comes. With no drive on the port
    // WinUAE's ROM driver does not start, so the driver is missing too. The next test covers a driver with no card.
    const running = await listedDevice(s.e, 'scsi.device');
    await s.e.go();
    s.note(`scsi.device with no card: ${running ? 'running' : 'not running'}`);
    await refused(s, [], running ? QS.NO_CARD : QS.NO_PORT, running ? /No card was found/ : /no card answered/, 'nocard');
  }),
  nocapacity: (spec) => cardRun(spec, 'nocapacity', {}, async (s, card) => {
    // WinUAE cannot present a drive with no capacity, so make the drive's answer an empty one.
    const held = await refused(s, [card], QS.NO_CARD, /No card was found/, 'nocapacity',
      () => s.e.failNextCall('dev_geometry', () => s.clickMain(GID.QUICK)));
    return held;
  }),
  beside: (spec) => {
    // A drive with no capacity is no card, so the real one beside it is the only card. It sits on unit 1 here.
    // Blank on unit 0 and used on unit 1, so the screen shows which one was read.
    const second = `${ROOT}/out/target-quick/${spec.key}/card-beside.hdf.2`;
    const make = () => { makeAmigaCard(`${second}.hdf`, 256 * MiB, USED); renameSync(`${second}.hdf`, second); };
    return cardRun(spec, 'beside', { make, keep: { bootPri: 5, cfg: () => [`hardfile2=rw,:${second},0,0,0,512,-128,,ide1`] } }, async (s, card) => {
      const held = { [card]: hashFile(card), [second]: hashFile(second) };
      await s.step('A drive with no capacity beside a card: the card is used', async () => {
        await s.e.failNextCall('dev_geometry', () => s.clickMain(GID.QUICK));
        const { w, said } = await cardScreen(s);
        eq(said.holds, 'DH0 Workbench, DH1 Work', 'Now holds');
        await s.e.clickClose(w, QS.CANCEL);
        await closed(s);
        for (const [f, was] of Object.entries(held)) eq(hashFile(f), was, `hash of ${path.basename(f)}`);
      });
      return held;
    });
  },
  // A driver can state a capacity and leave the rest at zero. That is still a drive.
  zerochs: (spec) => cardRun(spec, 'zerochs', {}, async (s, card, was) => {
    await s.step('A lone card with a capacity and zero heads is used, with figures made from its capacity', async () => {
      await zeroHeadsNext(s);
      const { w, usable } = await cardScreen(s);
      eq(usable.heads, 16, 'heads'); eq(usable.secs, 63, 'sectors'); eq(usable.cyl, Math.floor(ZERO_CHS_TOTAL / 1008), 'cylinders');
      await s.e.clickClose(w, QS.CANCEL);
      await closed(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return { [card]: was };
  }),
  zerochsbeside: (spec) => {
    const second = `${ROOT}/out/target-quick/${spec.key}/card-zerochsbeside.hdf.2`;
    const make = () => { const fd = openSync(second, 'w'); ftruncateSync(fd, 256 * MiB); closeSync(fd); };
    return cardRun(spec, 'zerochsbeside', { make, keep: { cfg: () => [`hardfile2=rw,:${second},0,0,0,512,-128,,ide1`] } }, (s, card) =>
      refused(s, [card, second], QS.TWO_DRIVES, /More than one drive/, 'zerochsbeside', () => zeroHeadsNext(s)));
  },
  nowindow: (spec) => cardRun(spec, 'nowindow', {}, async (s, card, was) => {
    await s.step('If the card screen cannot open, it says so', async () => {
      // Lets the wizard find the card, then fails the window for the card screen only.
      const w = await openWizard(s, () => s.e.failNextCall('dlg_open', () => s.clickMain(GID.QUICK)));
      eq(await s.e.cstr(await s.e.u32(w + 32), 40), 'Quick setup', 'title');
      eq(await s.e.g32('g_qs_refusal'), 0, 'not a refusal');
      await qsShot(s, 'nowindow');
      await s.e.clickClose(w, OK);
      await closed(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return { [card]: was };
  }),
  two: (spec) => {
    const second = `${ROOT}/out/target-quick/${spec.key}/card-two.hdf.2`;
    const make = () => { const fd = openSync(second, 'w'); ftruncateSync(fd, 256 * MiB); closeSync(fd); };
    return cardRun(spec, 'two', { make, keep: { cfg: () => [`hardfile2=rw,:${second},0,0,0,512,-128,,ide1`] } }, (s, card) =>
      refused(s, [card, second], QS.TWO_DRIVES, /More than one drive/, 'two'));
  },
  booted: (spec) => {
    const dir = `${ROOT}/out/target-quick/${spec.key}/card-booted-files`;
    const make = (card) => {
      mkdirSync(dir, { recursive: true });
      writeFileSync(`${dir}/startup-sequence`, 'Run >NIL: volcheck serve\nHDPart\n');
      makeAmigaCard(card, 256 * MiB, [{ dev: 'DH0', vol: 'CardBoot', bootable: true, pri: 10, commands: ['+', 'makedir', 's', '+', 'write', `${dir}/startup-sequence`, 's/startup-sequence',
        '+', 'write', `${ROOT}/out/HDPart.exe`, 'HDPart', '+', 'write', `${ROOT}/out/volcheck`, 'volcheck'] }, { dev: 'DH1', vol: 'Work' }]);
    };
    return cardRun(spec, 'booted', { make, keep: { bootPri: -128 } }, async (s, card) => {
      await s.step('The Amiga really started from the card', async () => {
        const seg = JSON.parse(await s.e.tool('winuae_tasks')).map((t) => t.name);
        await s.e.go();
        ok(seg.includes('DH0'), `handlers: ${seg.join()}`);
      });
      return refused(s, [card], QS.BOOTED, /started from this card/, 'booted');
    });
  },
  // scsi.device 40.20 on an A4000 gives figures that don't add up, so the wizard makes its own. That is only safe if the
  // driver addresses by offset and ignores them. Write a table with those figures and see where the data lands.
  synth: (spec) => cardRun(spec, 'synth', { bytes: 8000000000, keep: { bootPri: 5 } }, async (s, card) => {
    const e = s.e;
    let g;
    await s.step('The wizard replaces the figures with 16 x 63', async () => {
      const w = await openWizard(s);
      const { said, usable } = await cardScreen(s);
      g = usable;
      eq(said.size, '8 GB (this Amiga can use 4 GB)', 'Size');
      eq(g.heads, 16, 'heads'); eq(g.secs, 63, 'sectors'); eq(g.cyl, Math.floor(g.total / 1008), 'cylinders');
      await e.clickClose(w, QS.CANCEL);
      await closed(s);
    });
    await s.step('A table with those figures reaching the top of what the wizard would use, three volumes formatted on the Amiga', async () => {
      await e.stop();
      rmSync(card);
      const add = (start, size, name) => ['+', 'add', `start=${start}`, `size=${size}`, `name=${name}`, 'dostype=ffs+intl', 'max_transfer=0x1FE00'];
      execFileSync('rdbtool', [card, 'create', `chs=${g.cyl},${g.heads},${g.secs}`, '+', 'init', 'rdb_cyls=2', ...add(2, 4160, 'DH0'), ...add(4162, 4030, 'DH1'), ...add(8192, 130, 'DH2')], { stdio: 'pipe' });
      const fd = openSync(card, 'r+'); ftruncateSync(fd, 8000000000); closeSync(fd);
      await e.start();
      await s.waitUp();
      await s.pickScsi(3);
      for (const i of [0, 1, 2]) ok((await s.format(i, `SYN${i}`)).ok, `format of DH${i}`);
    });
    const parts = [0, 1, 2].map((i) => s.bounds(i));
    await s.step('Each root block is exactly where the table puts it', async () => {
      eq(readRdb(card).cylBlocks, 1008, 'blocks per cylinder on the card');
      eq(parts[2].last, 8322 * 1008 - 1, 'last block of the last partition');
      parts.forEach((p, i) => {
        const b = peek(card, p.first + ((p.blocks + 1) >> 1));
        eq(b.readUInt32BE(0), 2, `DH${i} root block type`); eq(b.readUInt32BE(508), 1, `DH${i} root block secondary type`);
        eq(b.toString('latin1', 433, 433 + b[432]), `SYN${i}`, `DH${i} volume name`);
      });
    });
    const files = [];
    const write = async (vol, mb, file, part) => {
      const r = await s.volcheck(vol, mb, { file });
      ok(passed(r), `volcheck ${vol}:${file}: ${JSON.stringify(r)}`);
      eq(r.blocks, part.blocks - 2, `${vol} blocks in Info`);
      files.push({ ...r, part });
    };
    await s.step('Files on each volume, the last ones running to the end of the last partition', async () => {
      await write('DH0', 5, 'a.dat', parts[0]);
      await write('DH1', 5, 'b.dat', parts[1]);
      await write('DH2', 20, 'fill.dat', parts[2]);
      await write('DH2', 16, 'end.dat', parts[2]);
    });
    await s.step('Host scan: every file sector is whole and inside its partition, and data reaches the top', async () => {
      const st = scanStamps(card);
      for (const f of files) {
        const x = st.get(f.tag);
        ok(x, `${f.file}: no stamps on the card`);
        eq(x.count, f.wrote / 512, `${f.file} sectors`); eq(x.bad, 0, `${f.file} damaged sectors`);
        ok(x.min >= f.part.first && x.max <= f.part.last, `${f.file} at ${x.min}-${x.max}, partition is ${f.part.first}-${f.part.last}`);
      }
      eq(st.size, files.length, 'files on the card');
      const top = Math.max(...files.map((f) => st.get(f.tag).max));
      s.note(`highest file block ${top}, the partition ends at ${parts[2].last}`);
      ok(top >= parts[2].last - 8192, `data stops ${parts[2].last - top} blocks short of the end`);
      const fd = openSync(card, 'r'), buf = Buffer.alloc(16 * MiB);
      for (let pos = (parts[2].last + 1) * 512, n; (n = readSync(fd, buf, 0, buf.length, pos)) > 0; pos += n) {
        ok(buf.subarray(0, n).every((v) => v === 0), `written past the last partition, near byte ${pos}`);
      }
      closeSync(fd);
      readRdb(card);
    });
    await s.step('Restart: the volumes mount and every file reads back', async () => {
      await s.restart();
      for (const f of files) {
        const r = await s.volcheck(f.vol, f.mb, { file: f.file, flags: VC.VERIFY });
        ok(passed(r) && r.equal === f.mb * MiB, `${f.vol}:${f.file}: ${JSON.stringify(r)}`);
      }
      eq(s.requesters.length, 0, 'system requesters');
    });
    await qsShot(s, 'synth');
    return null;
  }),
  bootunknown: (spec) => cardRun(spec, 'bootunknown', {}, async (s, card) =>
    // Every boot the emulator can do has a known boot device, so make the answer "don't know".
    refused(s, [card], QS.BOOT_UNKNOWN, /can't tell which disk/, 'bootunknown',
      () => s.e.failNextCall('safety_classify', () => s.clickMain(GID.QUICK), 1))),
  dh0: (spec) => {
    const keep = { dirs: ['other'], bootPri: 5, cfg: (dir) => [`filesystem2=ro,DH0:Other:${dir}/other,-128`] };
    return cardRun(spec, 'dh0', { keep }, async (s, card) => {
      const held = await refused(s, [card], QS.NAME_TAKEN, /DH0:/, 'dh0');
      return held;
    });
  },
  dh3used: (spec) => {
    const keep = { dirs: ['other'], bootPri: 5, cfg: (dir) => [`filesystem2=ro,DH3:Other:${dir}/other,-128`] };
    return cardRun(spec, 'dh3used', { make: (c) => makeAmigaCard(c, 256 * MiB, USED), keep }, (s, card) =>
      refused(s, [card], QS.NAME_TAKEN, /DH3:/, 'dh3used'));
  },
  small: (spec) => cardRun(spec, 'small', { bytes: 64 * MiB }, (s, card) =>
    refused(s, [card], QS.TOO_SMALL, /too small/, 'small')),
};

// Machines Quick setup is not for: the card there must not be touched.
const REFUSED_MACHINES = {
  a3000ks204: { code: QS.UNPROVEN, words: /can't confirm/ }, a3000ks31: { code: QS.UNPROVEN, words: /can't confirm/ }, a3000ks32: { code: QS.UNPROVEN, words: /can't confirm/ },
  ks31softload: { code: QS.UNPROVEN, words: /can't confirm/ }, a3000softload: { code: QS.UNPROVEN, words: /can't confirm/ },
  a500pks204: { code: QS.NO_PORT, words: /no built-in IDE port/ },
  // On the emulator another machine's ROM leaves an A3000 with no scsi.device, so these end at "no port", not "can't confirm".
  a3000roma4000: { code: QS.NO_PORT, words: /no built-in IDE port/ }, a3000roma1200: { code: QS.NO_PORT, words: /no built-in IDE port/ },
};

/* ---- Screens 5 and 6: the run itself ---- */
const FMT_ERR_FORMAT = 5;                     // src/format.h
const RDB_ERR_IO = (-6) >>> 0;                // src/rdb.h
const RESTART = 1, BACK_TO_MAIN = 2;          // src/gui.c, Finished screen
const QS_CARD_UNIT = 40;                      // offset of unit in DiscDisk
const write = (e, address, data) => e.tool('winuae_memory_write', { address, data });
const cstrHex = (t) => Buffer.from(`${t}\0`, 'latin1').toString('hex');
const exactWin = async (s, title) => (await s.e.windows()).find((w) => w.title === title)?.addr || 0;
const said = (s) => s.e.gstr('g_qs_said', 0, 500);
const stepRows = async (s) => { const rows = []; for (let i = 0; i < 6; i++) { const r = await s.e.gstr('g_qs_steps', i * 40, 40); if (r) rows.push(r); } return rows; };

async function lastCheck(s, ticks = [], via) {
  const c = await chooseScreen(s, via);
  for (const t of ticks) await tick(s, c, t);
  return (await confirmScreen(s)).w;
}

// Stop at the nth call of a function, with its breakpoint gone. Returns where it stopped.
async function stopAtCall(e, sym, n, act) {
  const bp = e.addr(sym);
  await e.tool('winuae_breakpoint_set', { address: bp });
  await act();
  for (let i = 1; ; i++) {
    await e.tool('winuae_wait_stop', { timeout_ms: 60000 });
    const regs = await e.tool('winuae_registers_get');
    const sp = parseInt(regs.match(/A7=\$([0-9A-F]+)/)[1], 16), pc = parseInt(regs.match(/PC=\$([0-9A-F]+)/)[1], 16);
    if (pc !== bp) throw new Error(`stopped at ${pc.toString(16)}, not ${sym}`);
    await e.tool('winuae_breakpoint_clear', { address: bp });
    if (i === n) return { sp, ret: await e.u32(sp) };
    await e.tool('winuae_step');
    await e.tool('winuae_breakpoint_set', { address: bp });
    await e.go();
  }
}
// From such a stop: return from the function at once, without running it.
async function returnNow(e, { sp, ret }, value) {
  await e.tool('winuae_registers_set', { D0: value, PC: ret, A7: sp + 4 });
  await e.go();
}

// A stop or failure box from the wizard: check its words, close it. Returns the text.
async function wizardBox(s, words, shot) {
  const e = s.e, w = await e.waitFor('the wizard to say something', () => exactWin(s, 'Quick setup'), 300000);
  await windowFits(e, w);
  const t = await said(s);
  s.note(`said: ${t.replace(/\n/g, ' / ')}`);
  ok(words.test(t), `${JSON.stringify(t)} does not match ${words}`);
  ok(!TECHNICAL.test(t), `technical word in: ${t}`);
  if (shot) await qsShot(s, shot);
  await e.clickClose(w, OK);
  await closed(s);
  return t;
}
// The run ends on the Finished screen. Anything else is a failure, with what it said.
async function finished(s, timeout = 900000) {
  const e = s.e, end = Date.now() + timeout;
  for (;;) {
    if ((await e.g32('g_qs_screen')) === 6) { const w = await exactWin(s, 'Quick setup: done'); if (w) { await e.go(); return w; } }
    if (await exactWin(s, 'Quick setup')) throw new Error(`the run stopped: ${await said(s)}`);
    await s.collectRequesters('the run');
    await e.go();
    if (Date.now() > end) throw new Error('timed out waiting for the Finished screen');
    await sleep(500);
  }
}
async function backToMain(s) {
  const e = s.e;
  await e.clickClose(await exactWin(s, 'Quick setup: done'), BACK_TO_MAIN);
  await closed(s);
}
// Start from the last check and go all the way.
async function fullRun(s, { ticks = [], via } = {}) {
  await s.e.click(await lastCheck(s, ticks, via), QS.ERASE);
  return finished(s);
}

const VOLUMES = ['System', 'Work', 'Games'];
// From the host: the table is the wizard's, every checksum is good (readRdb), nothing past 4 GiB.
function checkTable(s, card, n) {
  const r = readRdb(card);
  eq(r.parts.length, n, 'partitions on the card');
  r.parts.forEach((p, i) => {
    eq(p.name, `DH${i}`, 'name');
    eq(p.dosType, 0x444F5303, `${p.name} filesystem`);
    eq(p.maxTransfer, 0x1FE00, `${p.name} MaxTransfer`);
    eq(p.mask, 0x7FFFFFFE, `${p.name} mask`);
    eq(p.flags & 1, i === 0 ? 1 : 0, `${p.name} bootable`);
    eq(p.flags & 2, 0, `${p.name} mounts by itself`);
    if (i) eq(p.low, r.parts[i - 1].high + 1, `${p.name} starts where the last one ended`);
    ok((p.high + 1) * r.cylBlocks <= 8388608, `${p.name} ends past 4 GiB`);
  });
  ok(!peek(card, 0).toString('latin1').includes('QUICKSETUP'), 'no setup-pending marker on a finished card');
  return r;
}
// From the host: each volume is on the card, not in buffers. Its root block is whole, names it and says the map is valid.
function checkVolumes(s, card, names) {
  const r = readRdb(card);
  names.forEach((name, i) => {
    const p = s.bounds(i), first = peek(card, p.first);
    eq(first.readUInt32BE(0), 0x444F5303, `${name}: first block of its partition`);
    const b = peek(card, p.first + ((p.blocks + 1) >> 1));
    let sum = 0;
    for (let o = 0; o < 512; o += 4) sum = (sum + b.readUInt32BE(o)) >>> 0;
    eq(sum, 0, `${name}: root block checksum`);
    eq(b.readUInt32BE(0), 2, `${name}: root block type`); eq(b.readUInt32BE(508), 1, `${name}: root block secondary type`);
    eq(b.readUInt32BE(312), 0xFFFFFFFF, `${name}: bitmap marked valid`);
    eq(b.toString('latin1', 433, 433 + b[432]), name, `${name}: name in the root block`);
  });
  return r;
}
// From inside the Amiga: through the device name, and through the volume name. Same blocks, validated.
async function checkLive(s, names) {
  for (let i = 0; i < names.length; i++) {
    const part = s.bounds(i);
    for (const vol of [`DH${i}`, names[i]]) {
      const r = await s.volcheck(vol, 0, { flags: VC.INFO });
      ok(passed(r), `Info on ${vol}: ${JSON.stringify(r)}`);
      eq(r.blocks, part.blocks - 2, `${vol} blocks`); eq(r.state, 82, `${vol} validated`); eq(r.dtype, '444f5303', `${vol} type`);
    }
  }
}
async function volcheckEach(s, names, mb = 5, file = 'run.dat') {
  const files = [];
  for (let i = 0; i < names.length; i++) {
    const part = s.bounds(i), r = await s.volcheck(names[i], mb, { file });
    ok(passed(r) && r.equal === mb * MiB, `volcheck ${names[i]}: ${JSON.stringify(r)}`);
    files.push({ ...r, part });
  }
  return files;
}
function checkStamps(card, files) {
  const st = scanStamps(card);
  for (const f of files) {
    const x = st.get(f.tag);
    ok(x, `${f.vol}:${f.file}: no stamps on the card`);
    eq(x.count, f.wrote / 512, `${f.vol}:${f.file} sectors`); eq(x.bad, 0, `${f.vol}:${f.file} damaged sectors`);
    ok(x.min >= f.part.first && x.max <= f.part.last, `${f.vol}:${f.file} at ${x.min}-${x.max}, partition is ${f.part.first}-${f.part.last}`);
  }
  return st;
}
const noRequesters = (s) => eq(s.requesters.length, 0, `system requesters: ${JSON.stringify(s.requesters)}`);
// The first byte at or past fromByte that isn't zero, or -1. Compares whole buffers: a 4 GB scan must not take minutes.
export function firstNonZero(file, fromByte) {
  const fd = openSync(file, 'r'), buf = Buffer.alloc(16 * MiB), zero = Buffer.alloc(buf.length);
  try {
    for (let pos = fromByte, n; (n = readSync(fd, buf, 0, buf.length, pos)) > 0; pos += n) {
      if (buf.compare(zero, 0, n, 0, n)) return pos + buf.subarray(0, n).findIndex((v) => v !== 0);
    }
    return -1;
  } finally { closeSync(fd); }
}
// A scan that never finds anything proves nothing. On a throwaway card of the same size: found at the very first byte
// from 4 GiB and at the very last, and not fooled by a byte just below 4 GiB.
function scanCanFail(card, bytes) {
  const t = `${card}.scan`, poke = (at, v) => { const fd = openSync(t, 'r+'); writeSync(fd, Buffer.from([v]), 0, 1, at); closeSync(fd); };
  try {
    execFileSync('truncate', ['-s', String(bytes), t]);
    eq(firstNonZero(t, GiB4), -1, 'scan of a blank card');
    poke(GiB4 - 1, 1);
    eq(firstNonZero(t, GiB4), -1, 'scan with a byte just below 4 GiB');
    poke(bytes - 1, 1);
    eq(firstNonZero(t, GiB4), bytes - 1, 'scan with a byte in the last place');
    poke(GiB4, 1);
    eq(firstNonZero(t, GiB4), GiB4, 'scan with a byte at 4 GiB');
  } finally { rmSync(t, { force: true }); }
}
function nothingPast(card, block) {
  const fd = openSync(card, 'r'), buf = Buffer.alloc(16 * MiB);
  for (let pos = (block + 1) * 512, n; (n = readSync(fd, buf, 0, buf.length, pos)) > 0; pos += n) {
    ok(buf.subarray(0, n).every((v) => v === 0), `written past the last partition, near byte ${pos}`);
  }
  closeSync(fd);
}
const mainShows = async (s, n, unit = 0) => {
  eq(await s.e.g32('g_cur_unit'), unit, 'unit in the main window');
  eq(await s.e.g32('g_have_model'), 1, 'main window has a table');
  eq(await s.e.g32('g_dirty'), 0, 'main window has nothing unsaved');
  eq(await s.nparts(), n, 'partitions in the main window');
  for (let i = 0; i < n; i++) ok((await s.row(i)).startsWith(`DH${i}`), `row ${i}: ${await s.row(i)}`);
  eq(await s.e.gstr('g_cur_driver'), 'scsi.device', 'driver in the main window');
};

// Nothing may load from disk, and no requester may rise, from the Finished screen to the reboot: the floppy is out by then.
async function loadersAndRequesters(e) {
  const bases = { exec: await e.u32(4), dos: await e.g32('DOSBase'), gfx: await e.g32('GfxBase'), intuition: await e.g32('IntuitionBase') };
  const lvos = [['exec', 'OpenLibrary', 552], ['exec', 'OldOpenLibrary', 408], ['exec', 'OpenDevice', 444], ['exec', 'OpenResource', 498],
    ['dos', 'Open', 30], ['dos', 'Lock', 84], ['dos', 'LoadSeg', 150], ['dos', 'NewLoadSeg', 768], ['dos', 'ErrorReport', 480],
    ['gfx', 'OpenFont', 72], ['intuition', 'EasyRequestArgs', 588], ['intuition', 'BuildEasyRequestArgs', 594], ['intuition', 'AutoRequest', 348]];
  const out = new Map();
  for (const [lib, name, lvo] of lvos) {
    const j = await e.mem(bases[lib] - lvo, 6);
    ok(j.readUInt16BE(0) === 0x4EF9, `${lib} ${name} is not a plain jump`);
    out.set(j.readUInt32BE(2), name);
  }
  return out;
}
// volcheck, the test's own tool, and Intuition handling the press (graphics' OpenFont looks only among fonts in memory).
const RESTART_ALLOWED = new Set(['OpenLibrary by Background CLI', 'OpenFont by input.device']);
// Press Restart with those watched. Ends stopped at qs_reboot, the last thing before ColdReboot.
async function restartWatched(s, { title = 'Quick setup: done', flushFn = 'qs_flush' } = {}) {
  const e = s.e, watched = await loadersAndRequesters(e), done = await exactWin(s, title);
  ok(done, `on the screen "${title}"`);
  const flush = e.addr(flushFn), reboot = e.addr('qs_reboot'), others = new Set();
  // The pointer goes there first: only the press itself happens under the breakpoints.
  const g = await e.gadget(done, RESTART);
  await e.moveTo((await e.i16(done + 4)) + g.left + (g.width >> 1), (await e.i16(done + 6)) + g.top + (g.height >> 1));
  await e.inputIdle();
  for (const a of watched.keys()) await e.tool('winuae_breakpoint_set', { address: a });
  await e.tool('winuae_breakpoint_set', { address: flush });
  await e.tool('winuae_breakpoint_set', { address: reboot });
  await e.seq([{ after_frames: 2, action: 'button', button: 0, pressed: true, port: 0 }, { after_frames: 6, action: 'button', button: 0, pressed: false, port: 0 }]);
  const stepOver = async (pc) => {
    await e.tool('winuae_breakpoint_clear', { address: pc });
    await e.tool('winuae_step');
    await e.tool('winuae_breakpoint_set', { address: pc });
    await e.go();
  };
  let flushed = false;
  for (;;) {
    await e.tool('winuae_wait_stop', { timeout_ms: 60000 });
    const pc = parseInt((await e.tool('winuae_registers_get')).match(/PC=\$([0-9A-F]+)/)[1], 16);
    if (watched.has(pc)) {
      // Only the two callers ever seen are let through, and neither is on this path. Anything else fails.
      const task = await e.u32((await e.u32(4)) + 276), name = await e.cstr(await e.u32(task + 10), 40);
      if (task === e.process || !RESTART_ALLOWED.has(`${watched.get(pc)} by ${name}`)) {
        throw new Error(`${watched.get(pc)} was called by ${name} on the way to the reboot`);
      }
      others.add(`${watched.get(pc)} by ${name}`);
      await stepOver(pc);
      continue;
    }
    if (pc === flush) {
      flushed = true;
      await e.tool('winuae_breakpoint_clear', { address: flush });
      await e.tool('winuae_step');
      await e.go();
      continue;
    }
    if (pc !== reboot) throw new Error(`stopped at ${pc.toString(16)}`);
    break;
  }
  if (others.size) s.note(`other tasks, not this path: ${[...others].join(', ')}`);
  for (const a of watched.keys()) await e.tool('winuae_breakpoint_clear', { address: a });
  await e.tool('winuae_breakpoint_clear', { address: reboot });
  ok(flushed, 'volumes flushed on the way to the reboot');
  s.note(`watched ${[...watched.values()].join(', ')}: none called between the Finished screen and the reboot`);
}

// From a stop at qs_reboot: let ColdReboot happen, for real, and wait for HDPart on the other side.
async function realReboot(s) {
  const e = s.e, listens = () => (readFileSync(e.logPath, 'latin1').match(/GDB: listening/g) || []).length, was = listens();
  await e.go();
  await e.waitFor('the reboot', async () => listens() > was, 90000).catch(() => { throw new Error('no reboot seen'); });
  await e.waitFor('debugger attached', async () => {
    try { await e.tool('winuae_attach'); return true; } catch (err) { return err.message.includes('Already connected'); }
  }, 30000);
  await e.go();
  await s.waitUp();
}

// Press Save and answer its confirmations, stopping where it writes. Returns the unit it writes to.
async function saveToWrite(s) {
  const e = s.e, bp = e.addr('qs_write_verified');
  await e.tool('winuae_breakpoint_set', { address: bp });
  await s.clickMain(GID.SAVE);
  for (let i = 0; i < 200; i++) {
    const regs = await e.tool('winuae_registers_get');
    if (parseInt(regs.match(/PC=\$([0-9A-F]+)/)[1], 16) === bp) {
      await e.tool('winuae_breakpoint_clear', { address: bp });
      return e.u32(parseInt(regs.match(/A7=\$([0-9A-F]+)/)[1], 16) + 8);
    }
    const w = await e.win('Save');
    if (w) { await e.click(w, PROCEED); await sleep(600); } else { await e.go(); await sleep(200); }
  }
  throw new Error('Save never reached the write');
}
async function finishSave(s) {
  const e = s.e, w = await e.waitWin('Save', 60000);
  ok(!(await s.isChoice(w)), 'the result box');
  await e.clickClose(w, OK);
  eq(await e.g32('g_dirty'), 0, 'table is saved');
  await s.idle();
}

const RUN_RUNS = {
  run: (spec) => cardRun(spec, 'run', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    let done;
    await s.step('run: the working screen ticks off each step as it goes', async () => {
      const w = await lastCheck(s);
      const at = await stopAtCall(e, 'format_partition', 2, () => e.click(w, QS.ERASE));
      eq(await e.g32('g_qs_screen'), 5, 'g_qs_screen while working');
      const rows = await stepRows(s);
      s.note(`rows: ${JSON.stringify(rows)}`);
      eq(rows.join('|'), 'ok  Partition table written|ok  Table read back and checked|ok  System formatted|..  Formatting Work|    Formatting Games', 'checklist');
      ok(await exactWin(s, 'Quick setup: working'), 'working window');
      await e.shot(`${SHOTS}/quick-${s.key}-working.png`);   // also lets it run on
      void at;
      done = await finished(s);
    });
    await s.step('run: blank card ends with a valid table, verified from the host', async () => {
      checkTable(s, card, 3);
      eq(await e.g32('g_qs_failed'), 0, 'no step failed');
    });
    await s.step('finished: the screen says what to do next, word for word', async () => {
      const a = e.addr('kQsDone'), lines = [];
      for (let i = 0; i < 5; i++) lines.push(await e.cstr(await e.u32(a + i * 4), 80));
      eq(lines.join('|'), 'Your card is ready for Workbench.|1. Remove this disk and insert your Install disk.|2. Press Restart.|3. Install Workbench to System (DH0:).|4. Remove the floppy and restart. Your Amiga now starts from the card.', 'Finished screen');
      ok(!TECHNICAL.test(lines.join(' ')), 'technical word');
      const wide = await e.i16(done + 8), high = await e.i16(done + 10);
      ok(wide <= 640 && high <= 200, `window is ${wide}x${high}: must fit a 640x200 screen`);
      await qsShot(s, 'done');
    });
    await s.step('run: every volume exists with the right name', async () => {
      checkVolumes(s, card, VOLUMES);
      await checkLive(s, VOLUMES);
    });
    await s.step('finished: Back to HDPart shows the table, not dirty', async () => {
      await backToMain(s);
      await mainShows(s, 3);
      await s.idle();
      await qsShot(s, 'back');
      ok(!(await panelShown(s)), 'the Quick setup button has gone');
    });
    await s.step('run: volcheck passes on each, inside its partition, with no requesters', async () => {
      checkStamps(card, await volcheckEach(s, VOLUMES));
      noRequesters(s);
    });
    return null;
  }),
  // Restart pressed at once, and on to the real reboot.
  restart: (spec) => cardRun(spec, 'run-restart', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('run to the Finished screen', async () => { await fullRun(s); });
    await s.step('Restart: nothing loads from disk and no requester rises before the reboot', async () => {
      await restartWatched(s);
      checkVolumes(s, card, VOLUMES);
    });
    await s.step('Restart reboots, and DH0 auto-mounts as System', async () => {
      await realReboot(s);
      const tasks = JSON.parse(await e.tool('winuae_tasks')).map((t) => t.name);
      await e.go();
      for (const dh of ['DH0', 'DH1', 'DH2']) ok(tasks.includes(dh), `${dh} mounted: ${tasks.join()}`);
      await checkLive(s, VOLUMES);
      checkStamps(card, await volcheckEach(s, VOLUMES));
      noRequesters(s);
    });
    return null;
  }),
  // Power cut at the moment of the reboot: what is on the card is all there is.
  cut: (spec) => cardRun(spec, 'run-cut', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('run to the Finished screen', async () => { await fullRun(s); });
    await s.step('Restart at once, power cut instead of the reboot: the card alone holds the volumes', async () => {
      await restartWatched(s);
      await e.stop();
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await e.start();
      await s.waitUp();
      await checkLive(s, VOLUMES);
      noRequesters(s);
    });
    return null;
  }),
  // The card on the port's second position. WinUAE's ROM driver does not start with a drive on ide1 alone (refused as no
  // port), so a blank drive sits on ide0, answering with no capacity to the wizard, as in `beside`. Its hash must never change.
  unit1: (spec) => {
    const decoy = `${ROOT}/out/target-quick/${spec.key}/card-run-unit1.hdf.2`;
    const make = () => { const fd = openSync(decoy, 'w'); ftruncateSync(fd, 256 * MiB); closeSync(fd); };
    const keep = { ...AFTER_CARD, controller: 'ide1', cfg: () => [`hardfile2=rw,:${decoy},0,0,0,512,-128,,ide0`] };
    return cardRun(spec, 'run-unit1', { make, keep }, async (s, card) => {
      const e = s.e, was = hashFile(decoy);
      const untouched = () => eq(hashFile(decoy), was, 'hash of the drive on unit 0');
      await s.step('run: a card on unit 1, every write goes there', async () => {
        const w = await lastCheck(s, [], () => e.failNextCall('dev_geometry', () => s.clickMain(GID.QUICK)));
        eq(await e.u32(e.addr('g_qs_card') + QS_CARD_UNIT), 1, 'the card found');
        const wr = await stopAtCall(e, 'qs_write_verified', 1, () => e.failNextCall('dev_geometry', () => e.click(w, QS.ERASE)));
        eq(await e.u32(wr.sp + 8), 1, 'unit the table goes to');
        const fm = await stopAtCall(e, 'format_partition', 1, () => e.go());
        eq(await e.u32(fm.sp + 8), 1, 'unit the format goes to');
        await e.go();
        await finished(s);
        checkTable(s, card, 3);
        checkVolumes(s, card, VOLUMES);
        await checkLive(s, VOLUMES);
        untouched();
      });
      await s.step('Back to HDPart shows unit 1', async () => {
        await backToMain(s);
        await mainShows(s, 3, 1);
      });
      await s.step('Scan in the main window lists unit 1, and picking it shows the table', async () => {
        await s.clickMain(GID.SCAN);
        await e.waitFor('both units after Scan', async () => (await e.g32('g_nunits')) === 2 && ((await e.g32('g_cur_unitidx')) | 0) === 0, 180000);
        await s.idle();
        eq(await e.g32('g_cur_unit'), 0, 'Scan starts on the first row');
        const m = await s.main(), b = await e.mem(e.addr('g_ulist'), 16), rowH = b.readInt16BE(8);
        await e.clickAt((await e.i16(m + 4)) + b.readInt16BE(0) + (b.readInt16BE(4) >> 1), (await e.i16(m + 6)) + b.readInt16BE(2) + rowH + (rowH >> 1));
        await e.waitFor('unit 1 shown', async () => ((await e.g32('g_cur_unitidx')) | 0) === 1);
        await s.idle();
        await mainShows(s, 3, 1);
      });
      await s.step('Save from the main window goes to unit 1, and the host sees that table', async () => {
        await s.selectPart(2);
        await s.clickMain(GID.DELETE);
        await e.clickClose(await e.waitWin('Delete Partition'), PROCEED);
        eq(await e.g32('g_dirty'), 1, 'an edit to save');
        eq(await saveToWrite(s), 1, 'unit Save goes to');
        await e.go();
        await finishSave(s);
        eq(await e.g32('g_cur_unit'), 1, 'unit in the main window');
        eq(readRdb(card).parts.map((p) => p.name).join(), 'DH0,DH1', 'table on the card');
        untouched();
        noRequesters(s);
      });
      return null;
    });
  },
  // Every row but one says ok, so the failure must not sit beside a list of nothing but ok.
  failflush: (spec) => cardRun(spec, 'run-failflush', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('run: a failed flush names the step and marks a row', async () => {
      const w = await lastCheck(s);
      await e.failNextCall('qs_flush', () => e.click(w, QS.ERASE), 2);     // the second volume, Work
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      eq(await e.g32('g_qs_failed'), 4, 'failed step');
      const rows = await stepRows(s);
      s.note(`rows: ${JSON.stringify(rows)}`);
      eq(rows.join('|'), 'ok  Partition table written|ok  Table read back and checked|ok  System formatted|!!  Saving Work|ok  Games formatted', 'checklist');
      await wizardBox(s, /Saving the new volumes to the card failed/, 'fail-flush');
      checkTable(s, card, 3);
      await mainShows(s, 3);
    });
    return null;
  }),
  pc: (spec) => cardRun(spec, 'run-pc', { make: pcCard, keep: AFTER_CARD }, async (s, card) => {
    await s.step('run: a PC-formatted card ends with a valid table', async () => {
      await fullRun(s);
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await backToMain(s);
      await mainShows(s, 3);
      noRequesters(s);
    });
    return null;
  }),
  // Before the first write, everything the card screen checked is checked again.
  swap: (spec) => cardRun(spec, 'run-swap', { keep: AFTER_CARD }, async (s, card, was) => {
    const e = s.e;
    const stops = async (name, words, shot, change) => {
      await s.step(`run: ${name} stops before writing`, async () => {
        const w = await lastCheck(s);
        await change(w);
        await wizardBox(s, words, shot);
        await mainUntouched(s);
        eq(await e.g32('g_qs_failed'), 0, 'no step began');
        eq(hashFile(card), was, 'hash of the card');
      });
    };
    const poked = (fn) => async (w) => { await fn(); await e.go(); await e.click(w, QS.ERASE); };
    const CHANGED = /has changed/;
    await stops('card swapped after confirm (another model)', CHANGED, 'swap', poked(() => write(e, e.addr('g_qs_geo') + 24, cstrHex('OTHER CARD'))));
    await stops('card swapped after confirm (another size)', CHANGED, null, poked(async () => write(e, e.addr('g_qs_geo') + 16, long((await e.u32(e.addr('g_qs_geo') + 16)) + 2048))));
    await stops('card swapped after confirm (other contents)', CHANGED, null, poked(() => write(e, e.addr('g_qs_holds'), cstrHex('DH0 Other'))));
    await stops('card now on another unit', CHANGED, null, poked(() => write(e, e.addr('g_qs_card') + QS_CARD_UNIT, long(1))));
    await stops('card removed after confirm', /No card was found/, null, (w) => e.failNextCall('dev_geometry', () => e.click(w, QS.ERASE)));
    for (const [what, code, words] of [['a second drive', QS.TWO_DRIVES, /More than one drive/], ['now started from the card', QS.BOOTED, /started from this card/],
      ['DH0 taken by another disk', QS.NAME_TAKEN, /already using/], ['the IDE check no longer passing', QS.UNPROVEN, /can't confirm/]]) {
      await stops(what, words, null, (w) => e.failNextCall('qs_find_card', () => e.click(w, QS.ERASE), code));
    }
    await stops('started from the card after confirm', /started from this card/, null, async (w) => {
      // qs_find_card asks twice. The third answer is the run's own.
      const at = await stopAtCall(e, 'safety_classify', 3, () => e.click(w, QS.ERASE));
      await returnNow(e, at, 2);
    });
    await stops('a model and choices that disagree (no ticks)', /lost track/, null, poked(() => write(e, e.addr('g_qs_mask'), long(0))));
    await stops('a model and choices that disagree (too many ticks)', /lost track/, null, poked(() => write(e, e.addr('g_qs_mask'), long(7))));
    await s.step('After all those stops, the same card still runs to the end', async () => {
      await fullRun(s);
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await backToMain(s);
      await mainShows(s, 3);
      noRequesters(s);
    });
    return null;
  }),
  // Unsaved edits in the main window: Back to HDPart would replace them, so ask first.
  dirty: (spec) => cardRun(spec, 'run-dirty', { keep: AFTER_CARD }, async (s, card, was) => {
    const e = s.e;
    await s.step('Unsaved edits: refusing the prompt means the wizard does not open', async () => {
      await s.pickScsi(0);
      await s.split(2);
      eq(await e.g32('g_dirty'), 1, 'edits made');
      await e.pickMenu(1, 6);
      const p = await e.waitFor('the prompt', () => exactWin(s, 'Quick setup'), 30000);
      ok(await s.isChoice(p), 'it asks');
      eq(await e.g32('g_qs_screen'), 0, 'wizard not open while asking');
      await qsShot(s, 'dirty');
      await e.clickClose(p, CANCEL);
      await e.go(); await sleep(500);
      eq(await e.g32('g_qs_screen'), 0, 'wizard stays shut');
      ok(!(await e.win('Quick setup')), 'no window');
      eq(await e.g32('g_dirty'), 1, 'edits kept');
      eq(await s.nparts(), 2, 'partitions kept');
    });
    await s.step('Agreeing opens it. Cancel there still keeps the edits', async () => {
      await e.pickMenu(1, 6);
      await e.clickClose(await e.waitFor('the prompt', () => exactWin(s, 'Quick setup'), 30000), PROCEED);
      const w = await e.waitWin('Quick setup: your card', 120000);
      await e.clickClose(w, QS.CANCEL);
      await closed(s);
      eq(await e.g32('g_dirty'), 1, 'edits kept');
      eq(await s.nparts(), 2, 'partitions kept');
      eq(hashFile(card), was, 'hash of the card');
    });
    await s.step('Agreeing and running: Back to HDPart replaces them, nothing unsaved', async () => {
      await e.pickMenu(1, 6);
      await e.clickClose(await e.waitFor('the prompt', () => exactWin(s, 'Quick setup'), 30000), PROCEED);
      const w = await e.waitWin('Quick setup: your card', 120000);
      await e.clickClose(w, QS.CONTINUE);
      await e.clickClose(await e.waitWin('Quick setup: partitions', 120000), QS.CONTINUE);
      await e.click(await e.waitWin('Quick setup: ready?', 120000), QS.ERASE);
      await finished(s);
      await backToMain(s);
      await mainShows(s, 3);
      checkTable(s, card, 3);
    });
    return null;
  }),
  failwrite: (spec) => cardRun(spec, 'run-failwrite', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('run: a failed write names the step', async () => {
      const w = await lastCheck(s);
      // The second block: the head of the table is on the card and the rest is not.
      const fmt = e.addr('format_partition');
      await e.tool('winuae_breakpoint_set', { address: fmt });    // a stop here would hold the failure box back
      const at = await stopAtCall(e, 'dev_write_block', 2, () => e.click(w, QS.ERASE));
      await returnNow(e, at, 1);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      eq(await e.g32('g_qs_failed'), 1, 'failed step');
      await wizardBox(s, /Writing the partition table failed/, 'fail-write');
      await e.tool('winuae_breakpoint_clear', { address: fmt });
      await e.go();
      eq(await e.g32('g_dirty'), 0, 'nothing unsaved');
      eq(peek(card, 0).toString('latin1', 0, 4), 'RDSK', 'the head of the table is on the card');
      nothingPast(card, 0);                                       // blank before, so every later block is still zero
    });
    await s.step('A further run recovers the card', async () => {
      await fullRun(s, { via: 'menu' });
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await backToMain(s);
      await mainShows(s, 3);
      noRequesters(s);
    });
    return null;
  }),
  failcheck: (spec) => cardRun(spec, 'run-failcheck', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('run: a read-back that fails names the step', async () => {
      const w = await lastCheck(s);
      await stopAtCall(e, 'qs_write_verified', 1, () => e.click(w, QS.ERASE));
      await e.failNextCall('rdb_parse', () => e.go(), RDB_ERR_IO);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      eq(await e.g32('g_qs_failed'), 2, 'failed step');
      await wizardBox(s, /Checking the partition table failed/, 'fail-check');
    });
    await s.step('run: a read-back that differs in any field names the step', async () => {
      const w = await lastCheck(s, [], 'menu');
      await stopAtCall(e, 'qs_table_matches', 1, () => e.click(w, QS.ERASE));
      // parts[1].maxtransfer in the table read back.
      await write(e, e.addr('g_wv_chk') + 40 + 72 + 64, long(0x1000));
      await e.go();
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      eq(await e.g32('g_qs_failed'), 2, 'failed step');
      await wizardBox(s, /Checking the partition table failed/);
    });
    await s.step('A further run recovers the card', async () => {
      await fullRun(s, { via: 'menu' });
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      noRequesters(s);
    });
    return null;
  }),
  // The first format fails, so nothing is mounted: this holds today.
  failformat: (spec) => cardRun(spec, 'run-failformat', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('run: a failed format names the step and leaves a valid table', async () => {
      const w = await lastCheck(s);
      const at = await stopAtCall(e, 'format_partition', 1, () => e.click(w, QS.ERASE));
      await returnNow(e, at, FMT_ERR_FORMAT);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      eq(await e.g32('g_qs_failed'), 3, 'failed step');
      const rows = await stepRows(s);
      s.note(`rows: ${JSON.stringify(rows)}`);
      ok(rows[2].startsWith('!!'), `row: ${rows[2]}`);
      await wizardBox(s, /Formatting System failed/, 'fail-format');
      checkTable(s, card, 3);
      await mainShows(s, 3);
    });
    await s.step('A further run recovers the card', async () => {
      await fullRun(s, { via: 'menu' });
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await backToMain(s);
      await checkLive(s, VOLUMES);
      noRequesters(s);
    });
    return null;
  }),
  // A later format fails, so System is live. A run after that can't format through it, so it goes by the restart.
  failformat2: (spec) => cardRun(spec, 'run-failformat2', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('run: a later format fails, names that step, and leaves a valid table', async () => {
      const w = await lastCheck(s);
      const at = await stopAtCall(e, 'format_partition', 2, () => e.click(w, QS.ERASE));
      await returnNow(e, at, FMT_ERR_FORMAT);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      eq(await e.g32('g_qs_failed'), 4, 'failed step');
      const t = await wizardBox(s, /Formatting Work failed/);
      ok(/Restart/.test(t) && /run Quick setup again/.test(t), `says what to do next: ${t}`);
      checkTable(s, card, 3);
      checkVolumes(s, card, ['System']);
      await mainShows(s, 3);
    });
    await s.step('A re-run goes by the restart, and finishes', async () => {
      await rerunToOffer(s, card, 'menu');
      await finishByRestart(s, card);
    });
    return null;
  }),
  // Rulings: the volume check goes by device. A volume named System elsewhere must not satisfy it.
  collision: (spec) => {
    const keep = { dirs: ['other'], bootPri: 5, cfg: (dir) => [`filesystem2=rw,OTH:System:${dir}/other,-128`] };
    return cardRun(spec, 'run-collision', { keep }, async (s, card) => {
      const e = s.e;
      await s.step('Volume check: a volume named System elsewhere does not pass for the card', async () => {
        const named = await s.volcheck('System', 0, { flags: VC.INFO });
        ok(passed(named), `the other System is there: ${JSON.stringify(named)}`);
        const w = await lastCheck(s);
        // The format claims success without doing anything, so no DH0 exists. By name, a lock would succeed anyway.
        const at = await stopAtCall(e, 'format_partition', 1, () => e.click(w, QS.ERASE));
        await returnNow(e, at, 0);
        await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 120000);
        eq(await e.g32('g_qs_failed'), 3, 'failed step');
        await wizardBox(s, /Checking System failed/, 'fail-collision');
      });
      await s.step('With the other System still there, a real run passes through the device names', async () => {
        await fullRun(s, { via: 'menu' });
        checkTable(s, card, 3);
        checkVolumes(s, card, VOLUMES);
        const part = s.bounds(0), r = await s.volcheck('DH0', 0, { flags: VC.INFO });
        eq(r.blocks, part.blocks - 2, 'DH0 is the card');
        await backToMain(s);
        noRequesters(s);
      });
      return null;
    });
  },
  wrongname: (spec) => cardRun(spec, 'run-wrongname', { keep: AFTER_CARD }, async (s) => {
    const e = s.e;
    await s.step('Volume check: a volume with the wrong name fails the step', async () => {
      const w = await lastCheck(s);
      const { sp } = await stopAtCall(e, 'format_partition', 1, () => e.click(w, QS.ERASE));
      await write(e, 0x1E0, cstrHex('Wrong'));
      await write(e, sp + 20, long(0x1E0));       // volname: return address, driver, unit, model, index, then this
      await e.go();
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 180000);
      eq(await e.g32('g_qs_failed'), 3, 'failed step');
      await wizardBox(s, /Checking System failed/);
    });
    return null;
  }),
  // 8 GB card: the layout stops at 4 GiB, and nothing is written above it.
  gb8: (spec) => cardRun(spec, 'run-gb8', { bytes: 8000000000, keep: AFTER_CARD }, async (s, card) => {
    await s.step('run: 8 GB card, three volumes under 4 GiB, nothing written past the last', async () => {
      await fullRun(s);
      const r = checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await backToMain(s);
      await mainShows(s, 3);
      await s.e.stop();
      nothingPast(card, (r.parts[2].high + 1) * r.cylBlocks - 1);
    });
    return null;
  }),
  // The 4 GiB proof: a whole run with files, a restart so the volumes mount from the table, then from the host nothing at all
  // from 4 GiB to the end of an 8 GB card. Default machines in runTest.
  proof4g: (spec) => cardRun(spec, 'run-proof4g', { bytes: 8000000000, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('proof4g: the scan can fail', async () => { scanCanFail(card, 8000000000); });
    await s.step('proof4g: 8 GB card, full run, volcheck on each volume, every sector inside its partition', async () => {
      await fullRun(s);
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await backToMain(s);
      checkStamps(card, await volcheckEach(s, VOLUMES));
      noRequesters(s);
    });
    await s.step('proof4g: restart, the volumes mount and validate, and take another file', async () => {
      await s.restart();
      await checkLive(s, VOLUMES);
      checkStamps(card, await volcheckEach(s, VOLUMES, 1, 'again.dat'));
      noRequesters(s);
    });
    await s.step('proof4g: from the host, the last partition ends below 4 GiB and all is zero from there to the end', async () => {
      await e.stop();
      const r = readRdb(card), p = r.parts[r.parts.length - 1], end = (p.high + 1) * r.cylBlocks * 512;
      eq(statSync(card).size, 8000000000, 'card size');
      ok(end <= GiB4, `${p.name} ends at byte ${end}, past 4 GiB`);
      const at = firstNonZero(card, GiB4);
      s.note(`last partition ${p.name} ends at byte ${end}, ${GiB4 - end} below 4 GiB. Scanned ${8000000000 - GiB4} bytes from byte ${GiB4}: ${at < 0 ? 'all zero' : `non-zero at ${at}`}`);
      eq(at, -1, 'first non-zero byte at or past 4 GiB');
    });
    return null;
  }),
  // scsi.device 40.20 needs the wizard's own figures. Earlier the proof used a table made by rdbtool: now the wizard's.
  synth: (spec) => cardRun(spec, 'run-synth', { bytes: 8000000000, cpuMax: true, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e, names = [...VOLUMES, 'Demos'], files = [];
    let parts, last;
    await s.step('synth: the wizard makes the table on an 8 GB card, with its own figures', async () => {
      const c = await chooseScreen(s);
      await tick(s, c, QS.DEMOS);
      const g = await checkGeometry(s);
      eq(g.heads, 16, 'heads'); eq(g.secs, 63, 'sectors');
      const { w } = await confirmScreen(s);
      await e.click(w, QS.ERASE);
      await finished(s);
      const r = checkTable(s, card, 4);
      eq(r.cylBlocks, 1008, 'blocks per cylinder on the card');
      checkVolumes(s, card, names);
      await backToMain(s);
      parts = names.map((_, i) => s.bounds(i));
      last = parts[3];
      s.note(`partitions: ${JSON.stringify(parts)}`);
      ok(last.last + 1 > 8388608 - 2 * 1008, `the table stops at ${last.last}, short of the top of 4 GiB`);
    });
    const add = async (vol, mb, file, part, flags = 0) => {
      const r = await s.volcheck(vol, mb, { file, flags });
      ok(passed(r), `volcheck ${vol}:${file}: ${JSON.stringify(r)}`);
      eq(r.blocks, part.blocks - 2, `${vol} blocks in Info`);
      files.push({ ...r, part });
      return r;
    };
    await s.step('synth: files on each volume, with data running to the top of the last', async () => {
      await add('System', 5, 'a.dat', parts[0]);
      await add('Work', 5, 'b.dat', parts[1]);
      await add('Games', 5, 'c.dat', parts[2]);
      let toFill = Math.floor((last.blocks / 2) * (72 / 73) / 2048) - 64 - 24, n = 0;
      while (toFill > 0) { const c = Math.min(toFill, 256); await add('Demos', c, `fill${n++}.dat`, last, VC.WRITE); toFill -= c; }
      for (let round = 0; round < 3; round++) {
        await add('Demos', 64, `end${round}.dat`, last);
        const st = scanStamps(card).get(files[files.length - 1].tag);
        s.note(`end${round}.dat at ${st.min}-${st.max}, the partition ends at ${last.last}`);
        if (st.max >= last.last - 32 * 2048) break;
      }
      await add('Demos', 128, 'tail.dat', last);
    });
    await s.step('synth: host scan, every file sector whole and inside its partition, data at the top, nothing past it', async () => {
      const st = checkStamps(card, files);
      eq(st.size, files.length, 'files on the card');
      const top = Math.max(...files.map((f) => st.get(f.tag).max));
      s.note(`highest file block ${top}, the partition ends at ${last.last}`);
      ok(top >= last.last - 8192, `data stops ${last.last - top} blocks short of the end`);
      checkTable(s, card, 4);
      nothingPast(card, last.last);
    });
    await s.step('synth: restart, the volumes mount and files read back', async () => {
      await s.restart();
      for (const f of files) {
        if (/^fill/.test(f.file)) continue;
        const r = await s.volcheck(f.vol, f.mb, { file: f.file, flags: VC.VERIFY });
        ok(passed(r) && r.equal === f.mb * MiB, `${f.vol}:${f.file}: ${JSON.stringify(r)}`);
      }
      noRequesters(s);
      await qsShot(s, 'synth-wizard');
    });
    return null;
  }),
};

/* ---- Quick setup: the restart path and the popup ---- */
const QS_POPUP = 7, QS_OFFER = 8;                 // g_qs_screen
const POPUP_TITLE = 'Quick setup: finish', OFFER_TITLE = 'Quick setup: restart';
const ACTION_FORMAT = 1020;
const usedCard = (c) => makeAmigaCard(c, 256 * MiB, USED);

// The marker from the host: the mask it holds, or -1 for none. Offsets in src/rdb.c.
function markerOn(card) {
  const b = peek(card, 0);
  return b.toString('latin1', 188, 212) === 'HDPART  QUICKSETUP      ' && b.toString('latin1', 213, 216) === '   ' ? b[212] - 48 : -1;
}
const noFormatYet = async (e) => {
  const pc = parseInt((await e.tool('winuae_registers_get')).match(/PC=\$([0-9A-F]+)/)[1], 16);
  ok(pc !== e.addr('format_partition'), 'format_partition was called');
};
// Everything past the table's own blocks.
function hashPast(file, block) {
  const h = createHash('sha256'), fd = openSync(file, 'r'), buf = Buffer.alloc(16 * MiB);
  for (let pos = block * 512, n; (n = readSync(fd, buf, 0, buf.length, pos)) > 0; pos += n) h.update(buf.subarray(0, n));
  closeSync(fd);
  return h.digest('hex');
}
const windowFits = async (e, w) => {
  const wide = await e.i16(w + 8), high = await e.i16(w + 10);
  ok(wide <= 640 && high <= 200, `window is ${wide}x${high}: must fit a 640x200 screen`);
};

// The Restart offer: the words, one button, and the marker on the card. Fails if the run says anything else.
async function offerScreen(s, card, mask = 3) {
  const e = s.e;
  const w = await e.waitFor('the Restart offer', async () => {
    if (await exactWin(s, 'Quick setup')) throw new Error(`the run stopped: ${await said(s)}`);
    await s.collectRequesters('the first run');
    // g_qs_screen is set just after the window opens.
    return (await e.g32('g_qs_screen')) === QS_OFFER && exactWin(s, OFFER_TITLE);
  }, 300000);
  const line = await e.cstr(e.addr('kQsRestartLine'), 80), more = await e.cstr(e.addr('kQsRestartMore'), 80);
  eq(line, 'Restart with this same disk to finish setting up the card', 'the offer');
  ok(!TECHNICAL.test(`${line} ${more}`), `technical word: ${line} ${more}`);
  const gads = await e.gadgets(w);
  eq(gads.map((g) => g.id).join(), String(RESTART), 'the only button is Restart');
  eq((await e.u32(w + 24)) & 12, 0, 'no close or depth gadget');
  await windowFits(e, w);
  eq(markerOn(card), mask, 'marker on the card');
  return w;
}
// The popup: by itself, once, with the names the marker gives.
async function popupScreen(s, names = VOLUMES, { files = false } = {}) {
  const e = s.e, w = await e.waitFor('the popup', async () => (await e.g32('g_qs_screen')) === QS_POPUP && exactWin(s, POPUP_TITLE), 120000);
  eq(await e.g32('g_qs_resume_checks'), 1, 'checks since the start');
  const rows = [];
  for (let i = 0; i < 4; i++) { const r = await e.gstr('g_qs_resume_rows', i * 60, 60); if (r) rows.push(r); }
  const text = rows.join(' ');
  s.note(`popup: ${text}`);
  eq(text, `Finish setting up this card? ${names.join(', ')}. Everything on them will be erased.${files ? ' This card already has files on it.' : ''}`, 'popup');
  ok(!TECHNICAL.test(text), `technical word: ${text}`);
  eq((await e.gadgets(w)).map((g) => g.id).sort().join(), '0,1', 'Continue and Cancel');
  eq((await e.u32(w + 24)) & 12, 0, 'no close or depth gadget');
  eq(await e.g32('g_qs_check_stop'), 0, 'the check went all the way');
  await windowFits(e, w);
  return w;
}
// Which gate stopped the startup check: g_qs_check_stop in src/gui.c. REFUSED + a QS_R_* reason when the card was refused.
const CHECK_STOP = { NOT_IDE: 1, NO_MARKER: 3, REFUSED: 10 };
// Nothing at all after the window opens, the card untouched, and it was this gate that said so.
async function noPopup(s, card, was, stop = CHECK_STOP.NO_MARKER) {
  const e = s.e;
  await s.idle(120000);
  eq(await e.g32('g_qs_resume_checks'), 1, 'the check ran once');
  eq(await e.g32('g_qs_check_stop'), stop, 'what stopped the check');
  eq(await e.g32('g_qs_screen'), 0, 'g_qs_screen after the window opens');
  ok(!(await e.win('Quick setup')), 'no wizard window');
  await mainUntouched(s);
  if (card && was) eq(hashFile(card), was, 'hash of the card');
}
// Power off, on again, and no popup: what is on the card is all that counts.
async function nextBootNoPopup(s, card) {
  const e = s.e;
  await sleep(3000);
  await e.stop();
  const was = hashFile(card);
  await e.start();
  await s.waitUp();
  await noPopup(s, card, was);
}
// Restart on the offer, watched, then the real reboot, ending on the popup.
async function offerRestart(s, names, opts) {
  await restartWatched(s, { title: OFFER_TITLE, flushFn: 'qs_flush_live' });
  await realReboot(s);
  return popupScreen(s, names, opts);
}
async function rerunToOffer(s, card, via, ticks = [], mask = 3) {
  await s.e.click(await lastCheck(s, ticks, via), QS.ERASE);
  return offerScreen(s, card, mask);
}
// From the offer: Restart, popup, Continue, Finished, Restart, and the volumes valid on the host and live.
async function finishByRestart(s, card, names = VOLUMES) {
  const e = s.e;
  await e.click(await offerRestart(s, names), QS.CONTINUE);
  await finished(s);
  checkTable(s, card, names.length);
  checkVolumes(s, card, names);
  await restartWatched(s);
  await realReboot(s);
  await noPopup(s);
  checkVolumes(s, card, names);
  await checkLive(s, names);
  checkStamps(card, await volcheckEach(s, names));
  noRequesters(s);
}
// Stopped on the call to format_partition: the handler is really up by the time ACTION_FORMAT is sent. Its answer is forced to
// failure inside that format, so the mount stays live. DoPkt is the only thing the program itself calls for it.
async function failFormatPacket(s) {
  const e = s.e, dos = await e.g32('DOSBase'), j = await e.mem(dos - 240, 6);
  eq(j.readUInt16BE(0), 0x4EF9, 'DoPkt is a plain jump');
  const bp = j.readUInt32BE(2);
  await e.tool('winuae_breakpoint_set', { address: bp });
  await e.go();
  for (;;) {
    await e.tool('winuae_wait_stop', { timeout_ms: 60000 });
    const regs = await e.tool('winuae_registers_get');
    const reg = (r) => parseInt(regs.match(new RegExp(`${r}=\\$([0-9A-F]+)`))[1], 16);
    ok(reg('PC') === bp, `stopped at ${reg('PC').toString(16)}`);
    await e.tool('winuae_breakpoint_clear', { address: bp });
    const task = await e.u32((await e.u32(4)) + 276);
    if (task === e.process && reg('D2') === ACTION_FORMAT) {
      const sp = reg('A7');
      await e.tool('winuae_registers_set', { D0: 0, PC: await e.u32(sp), A7: sp + 4 });
      await e.go();
      return;
    }
    await e.tool('winuae_step');
    await e.tool('winuae_breakpoint_set', { address: bp });
    await e.go();
  }
}
// The nth format of the run fails for real, with DHn live.
async function realFormatFailure(s, n, act) {
  await stopAtCall(s.e, 'format_partition', n, act);
  await failFormatPacket(s);
  await s.e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 120000);
}
// HDPart's next n reads fail as a driver's would: exec's DoIO answers with an error for CMD_READ, from HDPart's task only.
async function failReads(s, n) {
  const e = s.e, sys = await e.u32(4), j = await e.mem(sys - 456, 6), CMD_READ = 2;
  eq(j.readUInt16BE(0), 0x4EF9, 'DoIO is a plain jump');
  const bp = j.readUInt32BE(2);
  let failed = 0;
  await e.tool('winuae_breakpoint_set', { address: bp });
  await e.go();
  while (failed < n) {
    await e.tool('winuae_wait_stop', { timeout_ms: 60000 });
    const regs = await e.tool('winuae_registers_get');
    const reg = (r) => parseInt(regs.match(new RegExp(`${r}=\\$([0-9A-F]+)`))[1], 16);
    ok(reg('PC') === bp, `stopped at ${reg('PC').toString(16)}`);
    await e.tool('winuae_breakpoint_clear', { address: bp });
    if ((await e.u32(sys + 276)) === e.process && (await e.u16(reg('A1') + 28)) === CMD_READ) {
      const sp = reg('A7');
      failed++;
      await e.tool('winuae_registers_set', { D0: 20, PC: await e.u32(sp), A7: sp + 4 });
    } else {
      await e.tool('winuae_step');
    }
    if (failed < n) await e.tool('winuae_breakpoint_set', { address: bp });
    await e.go();
  }
  s.note(`${n} reads failed`);
}
// The partition index of each of the next n calls of format_partition, starting with act. Leaves the run going.
async function formatCalls(s, act, n) {
  const e = s.e, seen = [];
  for (let i = 0; i < n; i++) {
    const { sp } = await stopAtCall(e, 'format_partition', 1, i ? () => e.go() : act);
    seen.push(await e.u32(sp + 16));     // return address, driver, unit, model, then the index
    await e.tool('winuae_step');
  }
  await e.go();
  return seen;
}
// A device's node in the DOS list, from guest memory.
async function dosMount(e, name) {
  const dos = await e.g32('DOSBase'), root = await e.u32(dos + 34), info = (await e.u32(root + 24)) * 4;
  for (let n = (await e.u32(info + 4)) * 4, i = 0; n && i < 64; n = (await e.u32(n)) * 4, i++) {
    if (await e.u32(n + 4)) continue;
    const b = (await e.u32(n + 40)) * 4, len = await e.u8(b);
    if ((await e.mem(b + 1, len)).toString('latin1') !== name) continue;
    const startup = (await e.u32(n + 28)) * 4;
    return { node: n, startup, env: (await e.u32(startup + 8)) * 4 };
  }
  throw new Error(`no mount ${name}`);
}
// Every device in the DOS list: name, driver, unit, cylinders.
async function dosMounts(e) {
  const dos = await e.g32('DOSBase'), root = await e.u32(dos + 34), info = (await e.u32(root + 24)) * 4, out = [];
  for (let n = (await e.u32(info + 4)) * 4, i = 0; n && i < 64; n = (await e.u32(n)) * 4, i++) {
    if (await e.u32(n + 4)) continue;
    const b = (await e.u32(n + 40)) * 4, name = (await e.mem(b + 1, await e.u8(b))).toString('latin1');
    const startup = (await e.u32(n + 28)) * 4;
    if (startup < 1024) { out.push({ name }); continue; }
    const d = (await e.u32(startup + 4)) * 4, env = (await e.u32(startup + 8)) * 4;
    if (d < 1024 || env < 1024) { out.push({ name }); continue; }
    out.push({ name, driver: (await e.mem(d + 1, await e.u8(d))).toString('latin1'), unit: await e.u32(startup), low: await e.u32(env + 36), high: await e.u32(env + 40) });
  }
  return out;
}
function rootName(s, card, i) {
  const p = s.bounds(i), b = peek(card, p.first + ((p.blocks + 1) >> 1));
  return b.toString('latin1', 433, 433 + b[432]);
}
// Up to the popup on a used card.
async function toPopup(s, card, ticks = [], mask = 3, names = VOLUMES) {
  await rerunToOffer(s, card, undefined, ticks, mask);
  return offerRestart(s, names);
}

const RESUME_RUNS = {
  used: (spec) => cardRun(spec, 'resume-used', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e, oldRoots = [];
    let past, w;
    await s.step('used card: first run writes table, formats nothing, offers Restart', async () => {
      past = hashPast(card, 64);
      const t = readRdb(card);
      t.parts.forEach((p) => { const blocks = (p.high - p.low + 1) * t.cylBlocks; oldRoots.push(p.low * t.cylBlocks + ((blocks + 1) >> 1)); });
      const conf = await lastCheck(s);
      await e.tool('winuae_breakpoint_set', { address: e.addr('format_partition') });
      await e.click(conf, QS.ERASE);
      await e.waitFor('the offer', async () => {
        await noFormatYet(e);
        if (await exactWin(s, 'Quick setup')) throw new Error(`the run stopped: ${await said(s)}`);
        return exactWin(s, OFFER_TITLE);
      }, 300000);
      await e.tool('winuae_breakpoint_clear', { address: e.addr('format_partition') });
      w = await offerScreen(s, card);
      eq((await stepRows(s)).join('|'), 'ok  Partition table written|ok  Table read back and checked', 'checklist');
      eq(readRdb(card).parts.map((p) => p.name).join(), 'DH0,DH1,DH2', 'the new table on the card');
      eq(hashPast(card, 64), past, 'hash of everything past the table');
      oldRoots.forEach((b) => eq(peek(card, b).readUInt32BE(0), 2, 'old root block still there'));
      await qsShot(s, 'offer');
    });
    await s.step('after restart: popup opens by itself', async () => {
      await restartWatched(s, { title: OFFER_TITLE, flushFn: 'qs_flush_live' });
      await realReboot(s);
      w = await popupScreen(s);
      const was = hashFile(card);
      await qsShot(s, 'popup');
      eq(hashFile(card), was, 'hash with the popup up');
      eq(hashPast(card, 64), past, 'hash of everything past the table, on the popup');
      eq(markerOn(card), 3, 'marker still on the card');
    });
    await s.step('resume: Continue formats all, marker gone, no popup on next boot', async () => {
      await stopAtCall(e, 'format_partition', 2, () => e.click(w, QS.CONTINUE));
      eq((await stepRows(s)).join('|'), 'ok  System formatted|..  Formatting Work|    Formatting Games|    Finishing the card', 'checklist');
      await e.go();
      await finished(s);
      eq(markerOn(card), -1, 'marker gone');
      checkTable(s, card, 3);
      checkVolumes(s, card, VOLUMES);
      await checkLive(s, VOLUMES);
      await backToMain(s);
      await mainShows(s, 3);
      eq(await e.g32('g_qs_resume_checks'), 1, 'the check ran once');
      await nextBootNoPopup(s, card);
      await checkLive(s, VOLUMES);
      checkStamps(card, await volcheckEach(s, VOLUMES));
      noRequesters(s);
    });
    return null;
  }),
  // Another choice of partitions, and Restart at the end instead of Back.
  names: (spec) => cardRun(spec, 'resume-names', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const names = [...VOLUMES, 'Demos'];
    await s.step('Four partitions: the popup names them all, and Restart finishes', async () => {
      await rerunToOffer(s, card, undefined, [QS.DEMOS], 7);
      await finishByRestart(s, card, names);
    });
    return null;
  }),
  system: (spec) => cardRun(spec, 'resume-system', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    await s.step('System alone: the popup names only that', async () => {
      await rerunToOffer(s, card, undefined, [QS.WORK, QS.GAMES], 0);
      await finishByRestart(s, card, ['System']);
    });
    return null;
  }),
  cancel: (spec) => cardRun(spec, 'resume-cancel', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('resume: Cancel clears marker, no popup on next boot', async () => {
      const w = await toPopup(s, card), past = hashPast(card, 64);
      await e.tool('winuae_breakpoint_set', { address: e.addr('format_partition') });
      await e.click(w, QS.CANCEL);
      await e.waitFor('the main window', async () => { await noFormatYet(e); return (await e.g32('g_qs_screen')) === 0 && !(await e.win('Quick setup')); }, 120000);
      await e.tool('winuae_breakpoint_clear', { address: e.addr('format_partition') });
      await s.idle();
      eq(markerOn(card), -1, 'marker gone');
      eq(hashPast(card, 64), past, 'nothing formatted');
      eq(readRdb(card).parts.map((p) => p.name).join(), 'DH0,DH1,DH2', 'the table stays');
      await mainShows(s, 3);
      await nextBootNoPopup(s, card);
    });
    await s.step('After Cancel the card is a used card: the full flow, then the popup again', async () => {
      await rerunToOffer(s, card, 'button');
      await finishByRestart(s, card);
    });
    return null;
  }),
  // Off at the offer, instead of pressing Restart: the card alone holds what was told.
  cut: (spec) => cardRun(spec, 'resume-cut', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('Power off at the Restart offer: the marked table is on the card, and the popup comes', async () => {
      // The driver is asked to let go of the marked table before the offer can be pressed.
      const conf = await lastCheck(s);
      await stopAtCall(e, 'dev_update', 1, () => e.click(conf, QS.ERASE));
      eq(markerOn(card), 3, 'marker on the card at the update');
      ok(!(await exactWin(s, OFFER_TITLE)), 'not offered yet');
      await e.go();
      await offerScreen(s, card);
      await restartWatched(s, { title: OFFER_TITLE, flushFn: 'qs_flush_live' });
      await e.stop();
      eq(markerOn(card), 3, 'marker on the card');
      await e.start();
      await s.waitUp();
      // The same for the table without its marker, before the Finished screen.
      await stopAtCall(e, 'dev_update', 1, async () => e.click(await popupScreen(s), QS.CONTINUE));
      eq(markerOn(card), -1, 'marker gone at the update');
      await e.go();
      await finished(s);
      checkVolumes(s, card, VOLUMES);
    });
    return null;
  }),
  // Release gate: a real failure inside the format, so the mount is live, then the run after it. No restart first.
  realfail: (spec) => cardRun(spec, 'resume-realfail', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('A real format failure on a blank card, with Work live', async () => {
      const w = await lastCheck(s);
      await realFormatFailure(s, 2, () => e.click(w, QS.ERASE));
      eq(await e.g32('g_qs_failed'), 4, 'failed step');
      await dosMount(e, 'DH1');
      const t = await wizardBox(s, /Formatting Work failed/, 'fail-real');
      ok(/Restart the Amiga/.test(t) && /run Quick setup again/.test(t), `says what to do next: ${t}`);
      await mainShows(s, 3);
      eq(markerOn(card), -1, 'no marker on a blank card\'s first run');
    });
    await s.step('Re-run with no restart sees the mounts, marks the table, restart, popup, finish', async () => {
      await rerunToOffer(s, card, 'menu');
      await finishByRestart(s, card);
    });
    return null;
  }),
  // As the failure box says: restart first, then run it again. The first format failing, so nothing is usable.
  realfail1: (spec) => cardRun(spec, 'resume-realfail1', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('A real failure on the first format, with System live', async () => {
      const w = await lastCheck(s);
      await realFormatFailure(s, 1, () => e.click(w, QS.ERASE));
      eq(await e.g32('g_qs_failed'), 3, 'failed step');
      await dosMount(e, 'DH0');
      await wizardBox(s, /Formatting System failed/);
    });
    await s.step('Restart first: no popup, for there is no marker. Then a run, restart, popup, finish', async () => {
      eq(markerOn(card), -1, 'no marker');
      await nextBootNoPopup(s, card);
      await rerunToOffer(s, card, 'button');
      await finishByRestart(s, card);
    });
    return null;
  }),
  resumefail: (spec) => cardRun(spec, 'resume-fail', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('resume: a format failure keeps the marker and the next run finishes', async () => {
      const w = await toPopup(s, card);
      await realFormatFailure(s, 2, () => e.click(w, QS.CONTINUE));
      const rows = await stepRows(s);
      s.note(`rows: ${JSON.stringify(rows)}`);
      eq(rows.join('|'), 'ok  System formatted|!!  Formatting Work|    Formatting Games|    Finishing the card', 'checklist');
      const t = await wizardBox(s, /Formatting Work failed/, 'fail-resume');
      ok(/Restart the Amiga with this same disk/.test(t) && /offer to finish/.test(t), `says what to do next: ${t}`);
      eq(markerOn(card), 3, 'marker stays');
      await mainShows(s, 3);
      await sleep(3000);
      await e.stop();
      eq(markerOn(card), 3, 'marker stays after the power goes');
      await e.start();
      await s.waitUp();
      const again = await popupScreen(s);
      eq((await formatCalls(s, () => e.click(again, QS.CONTINUE), 3)).join(), '0,1,2', 'partitions formatted by the next run, System too');
      await finished(s);
      eq(markerOn(card), -1, 'marker gone');
      checkVolumes(s, card, VOLUMES);
      await backToMain(s);
      await nextBootNoPopup(s, card);
      await checkLive(s, VOLUMES);
      noRequesters(s);
    });
    return null;
  }),
  // After a failed resume, without a restart, the user runs Quick setup: the volume check fails this time.
  resumerun: (spec) => cardRun(spec, 'resume-rerun', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('A failed volume check on resume, then Quick setup with no restart, goes by the restart too', async () => {
      const w = await toPopup(s, card);
      await e.failNextCall('qs_volume_ok', () => e.click(w, QS.CONTINUE), 0);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 180000);
      await wizardBox(s, /Checking System failed/);
      eq(markerOn(card), 3, 'marker stays');
      await rerunToOffer(s, card, 'menu');
      await finishByRestart(s, card);
    });
    return null;
  }),
  resumeflush: (spec) => cardRun(spec, 'resume-flush', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('A failed flush on resume keeps the marker, and the next start finishes', async () => {
      const w = await toPopup(s, card);
      await e.failNextCall('qs_flush', () => e.click(w, QS.CONTINUE), 2);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 180000);
      const t = await wizardBox(s, /Saving the new volumes/);
      ok(/offer to finish/.test(t), `says what to do next: ${t}`);
      eq(markerOn(card), 3, 'marker stays');
      await s.restart();
      await e.click(await popupScreen(s), QS.CONTINUE);
      await finished(s);
      checkVolumes(s, card, VOLUMES);
    });
    return null;
  }),
  // The formats went well and the marker can't be cleared.
  clearfail: (spec) => cardRun(spec, 'resume-clearfail', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('Clearing the marker fails after the formats: says so, and the next start is still true to its words', async () => {
      const w = await toPopup(s, card);
      const at = await stopAtCall(e, 'dev_write_block', 1, () => e.click(w, QS.CONTINUE));
      await returnNow(e, at, 1);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      const t = await wizardBox(s, /formatted, but/, 'fail-clear');
      ok(/Cancel/.test(t) && /Continue/.test(t), `says what the next start offers: ${t}`);
      eq(markerOn(card), 3, 'marker still on the card');
      checkVolumes(s, card, VOLUMES);
      await mainShows(s, 3);
    });
    await s.step('The next start brings the popup, and Cancel there keeps the volumes, as it said', async () => {
      await s.restart();
      const w = await popupScreen(s);
      await e.clickClose(w, QS.CANCEL);
      await closed(s);
      eq(markerOn(card), -1, 'marker gone');
      await mainShows(s, 3);
      checkVolumes(s, card, VOLUMES);
      await checkLive(s, VOLUMES);
      await nextBootNoPopup(s, card);
    });
    return null;
  }),
  // The other way of failing: the read-back disagrees, and the marker may well be gone. The words hold either way.
  clearcheck: (spec) => cardRun(spec, 'resume-clearcheck', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('Reading back the cleared table fails: says so, and there is no popup if the marker did go', async () => {
      const w = await toPopup(s, card);
      await stopAtCall(e, 'qs_table_matches', 1, () => e.click(w, QS.CONTINUE));
      await write(e, e.addr('g_wv_chk') + 40 + 72 + 64, long(0x1000));
      await e.go();
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      await wizardBox(s, /formatted, but/);
      eq(markerOn(card), -1, 'the write itself went through');
      checkVolumes(s, card, VOLUMES);
      await nextBootNoPopup(s, card);
      await checkLive(s, VOLUMES);
    });
    return null;
  }),
  cancelfail: (spec) => cardRun(spec, 'resume-cancelfail', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('Cancel can\'t clear the marker: says so, and the next start asks again', async () => {
      const w = await toPopup(s, card), past = hashPast(card, 64);
      const at = await stopAtCall(e, 'dev_write_block', 1, () => e.click(w, QS.CANCEL));
      await returnNow(e, at, 1);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      await wizardBox(s, /may ask/);
      eq(markerOn(card), 3, 'marker stays');
      eq(hashPast(card, 64), past, 'nothing erased');
      await mainShows(s, 3);
      await s.restart();
      await e.clickClose(await popupScreen(s), QS.CANCEL);
      await closed(s);
      eq(markerOn(card), -1, 'marker gone');
    });
    return null;
  }),
  // Rulings: only through mounts that match the table. A mismatch fails the step and formats nothing further.
  guard: (spec) => cardRun(spec, 'resume-guard', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    const CHECKS = [
      ['its bounds differ', async (m) => write(e, m.env + 10 * 4, long((await e.u32(m.env + 10 * 4)) - 1))],
      ['its start differs', async (m) => write(e, m.env + 9 * 4, long((await e.u32(m.env + 9 * 4)) + 1))],
      ['it is on another unit', async (m) => write(e, m.startup, long(5))],
      ['it is on another driver', async (m) => {
        const at = e.addr('g_qs_block'), name = 'other.device';     // the program's own memory: readable, long-aligned, not in use by now
        await write(e, at, Buffer.concat([Buffer.from([name.length]), Buffer.from(name, 'latin1')]).toString('hex'));
        await write(e, m.startup + 4, long(at / 4));
      }],
      ['what backs it can\'t be read', async (m) => write(e, m.startup + 4, long(0x1E0 / 4))],     // below all memory: not listed as a mount at all
      ['its filesystem differs', async (m) => write(e, m.env + 16 * 4, long(0x50465303))],
    ];
    let w = await toPopup(s, card);
    for (const [what, poke] of CHECKS) {
      await s.step(`Resume: a mount that does not match the table (${what}) fails the step, and formats nothing further`, async () => {
        await stopAtCall(e, 'format_partition', 1, () => e.click(w, QS.CONTINUE));
        await poke(await dosMount(e, 'DH1'));
        await e.go();
        await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 120000);
        eq(await e.g32('g_qs_failed'), 2, 'failed step');
        eq((await stepRows(s)).join('|'), 'ok  System formatted|!!  Formatting Work|    Formatting Games|    Finishing the card', 'checklist');
        await wizardBox(s, /Formatting Work failed/);
        eq(markerOn(card), 3, 'marker stays');
        ok(rootName(s, card, 0) === 'System', 'System formatted');
        ok(rootName(s, card, 1) !== 'Work', 'Work not formatted');
        ok(rootName(s, card, 2) !== 'Games', 'Games not formatted');
      });
      await s.restart();
      w = await popupScreen(s);
    }
    await s.step('After all those, the next start finishes', async () => {
      await e.click(w, QS.CONTINUE);
      await finished(s);
      checkVolumes(s, card, VOLUMES);
    });
    return null;
  }),
  // The answer on the popup acts on the card it asked about, or stops.
  swap: (spec) => cardRun(spec, 'resume-swap', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    let w = await toPopup(s, card);
    const stops = async (what, words, change, btn) => {
      await s.step(`Popup: ${what} stops and writes nothing`, async () => {
        const was = hashFile(card);
        await change(btn);
        await wizardBox(s, words);
        await mainUntouched(s);
        eq(hashFile(card), was, 'hash of the card');
        eq(await e.g32('g_qs_screen'), 0, 'wizard shut');
      });
      await s.restart();
      w = await popupScreen(s);
    };
    const poked = (fn) => async (btn) => { await fn(); await e.go(); await e.click(w, btn); };
    await stops('another model, on Continue', /has changed/, poked(() => write(e, e.addr('g_qs_geo') + 24, cstrHex('OTHER CARD'))), QS.CONTINUE);
    await stops('another model, on Cancel', /has changed/, poked(() => write(e, e.addr('g_qs_geo') + 24, cstrHex('OTHER CARD'))), QS.CANCEL);
    await stops('a second drive, on Continue', /More than one drive/, (btn) => e.failNextCall('qs_find_card', () => e.click(w, btn), QS.TWO_DRIVES), QS.CONTINUE);
    await stops('the marker gone from the card, on Continue', /has changed/, (btn) => e.failNextCall('qs_marker_on_card', () => e.click(w, btn), 0), QS.CONTINUE);
    await s.step('The same card still finishes', async () => {
      await e.click(w, QS.CONTINUE);
      await finished(s);
      checkVolumes(s, card, VOLUMES);
    });
    return null;
  }),
  // A table write failing on a used card: as the box says, run it again.
  usedfail: (spec) => cardRun(spec, 'resume-usedfail', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('Used card: the table write fails halfway, and a further run gets to the offer', async () => {
      const w = await lastCheck(s);
      const at = await stopAtCall(e, 'dev_write_block', 2, () => e.click(w, QS.ERASE));
      await returnNow(e, at, 1);
      await e.waitFor('the failure box', () => exactWin(s, 'Quick setup'), 60000);
      eq(await e.g32('g_qs_failed'), 1, 'failed step');
      await wizardBox(s, /Writing the partition table failed/);
      await rerunToOffer(s, card, 'menu');
      await finishByRestart(s, card);
    });
    return null;
  }),
  // No memory for the offer's window: it still says what to do, and what it says stays true.
  offernowin: (spec) => cardRun(spec, 'resume-offernowin', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('The Restart offer can\'t open: says the same in a box, and the next start brings the popup', async () => {
      const conf = await lastCheck(s);
      const at = await stopAtCall(e, 'dlg_open_flags', 2, () => e.click(conf, QS.ERASE));      // the working window, then the offer
      await returnNow(e, at, 0);
      const w = await e.waitFor('the box', () => exactWin(s, 'Quick setup'), 60000);
      eq(markerOn(card), 3, 'marker on the card');
      await e.clickClose(w, OK);
      await closed(s);
      await mainShows(s, 3);
      await s.restart();
      await e.click(await popupScreen(s), QS.CONTINUE);
      await finished(s);
      checkVolumes(s, card, VOLUMES);
    });
    return null;
  }),
  // A file on a partition of the marked table: the popup adds a line, and does the same either way. Once formatted, empty: no line.
  fileline: (spec) => cardRun(spec, 'resume-fileline', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('A marked card with a file on a partition: the popup says so', async () => {
      await fullRun(s);
      await backToMain(s);
      ok(passed(await s.volcheck('Work', 1, { file: 'keep.dat' })), 'a file on Work');
      await sleep(3000);
      await rerunToOffer(s, card, 'menu');
      const w = await offerRestart(s, VOLUMES, { files: true });
      await qsShot(s, 'popup-files');
      await e.click(w, QS.CONTINUE);
      await finished(s);
      checkVolumes(s, card, VOLUMES);
      eq(markerOn(card), -1, 'marker gone');
      await backToMain(s);
    });
    await s.step('The same card, formatted and empty: no line', async () => {
      await rerunToOffer(s, card, 'menu');
      await e.clickClose(await offerRestart(s), QS.CANCEL);
      await closed(s);
      eq(markerOn(card), -1, 'Cancel still clears the marker');
      noRequesters(s);
    });
    return null;
  }),
  // A read that fails while looking for files changes nothing but the line.
  fileread: (spec) => cardRun(spec, 'resume-fileread', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('A read failure while looking for files: no line, and the popup works as ever', async () => {
      await fullRun(s);
      await backToMain(s);
      ok(passed(await s.volcheck('Work', 1, { file: 'keep.dat' })), 'a file on Work');
      await sleep(3000);
      await rerunToOffer(s, card, 'menu');
      let held = false, bp = 0;
      for (let tries = 0; tries < 12 && !held; tries++) {
        await e.stop();
        await e.start();
        await e.locate(90000, 0);      // as soon as it is loaded: the check is short
        bp = e.addr('qs_card_has_files');   // the program may load elsewhere on each boot
        await e.tool('winuae_breakpoint_set', { address: bp });
        await e.go();
        try { await e.tool('winuae_wait_stop', { timeout_ms: 4000 }); held = true; } catch { s.note(`try ${tries + 1}: the check went by first`); }
      }
      ok(held, 'the check was caught');
      await e.tool('winuae_breakpoint_clear', { address: bp });
      await e.tool('winuae_step');
      await failReads(s, 3);
      const w = await popupScreen(s, VOLUMES, { files: false });
      await e.click(w, QS.CONTINUE);
      await finished(s);
      checkVolumes(s, card, VOLUMES);
    });
    return null;
  }),
  // The mount check's answer changes after the last check: same path as a used card, marker and all.
  lateuse: (spec) => cardRun(spec, 'resume-lateuse', { keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('Volumes mounted after confirm: the table goes on with the marker, and formats nothing', async () => {
      const w = await lastCheck(s);
      const at = await stopAtCall(e, 'safety_classify', 3, () => e.click(w, QS.ERASE));
      await returnNow(e, at, 1);
      await offerScreen(s, card);
      for (let i = 0; i < 3; i++) ok(rootName(s, card, i) !== VOLUMES[i], `${VOLUMES[i]} not formatted`);
    });
    return null;
  }),
};

/* The cards that must never bring the popup. Each edit starts from a table the popup does accept, made by the wizard on this
   machine and proved by `control`, so what stops the popup is that edit and nothing else. */
const markedPath = (key) => `${ROOT}/out/target-quick/${key}/marked-template.hdf`;
function sumFix(b, summed = b.readUInt32BE(4)) {
  b.writeUInt32BE(0, 8);
  let t = 0;
  for (let i = 0; i < summed; i++) t = (t + b.readUInt32BE(i * 4)) >>> 0;
  b.writeUInt32BE((-t) >>> 0, 8);
}
function editBlock(card, lba, fn) {
  const fd = openSync(card, 'r+'), b = Buffer.alloc(512);
  readSync(fd, b, 0, 512, lba * 512);
  fn(b);
  writeSync(fd, b, 0, 512, lba * 512);
  closeSync(fd);
}
const fromMarked = (key, edit) => (card) => { copyFileSync(markedPath(key), card); if (edit) edit(card); };
const PART_ENV = 128;
const MARKED_EDITS = {
  bounds: (c) => editBlock(c, 3, (b) => { b.writeUInt32BE(b.readUInt32BE(PART_ENV + 10 * 4) - 1, PART_ENV + 10 * 4); sumFix(b); }),
  start: (c) => editBlock(c, 2, (b) => { b.writeUInt32BE(b.readUInt32BE(PART_ENV + 9 * 4) + 1, PART_ENV + 9 * 4); sumFix(b); }),
  name: (c) => editBlock(c, 2, (b) => { b[36 + 3] = 0x37; sumFix(b); }),
  fstype: (c) => editBlock(c, 1, (b) => { b.writeUInt32BE(0x444F5301, PART_ENV + 16 * 4); sumFix(b); }),
  maxtransfer: (c) => editBlock(c, 1, (b) => { b.writeUInt32BE(0x7FFFFFFF, PART_ENV + 13 * 4); sumFix(b); }),
  notboot: (c) => editBlock(c, 1, (b) => { b.writeUInt32BE(b.readUInt32BE(20) & ~1, 20); sumFix(b); }),
  fewer: (c) => editBlock(c, 2, (b) => { b.writeUInt32BE(0xFFFFFFFF, 16); sumFix(b); }),
  digit: (c) => editBlock(c, 0, (b) => { b[212] = 0x31; sumFix(b); }),
  geometry: (c) => editBlock(c, 0, (b) => { b.writeUInt32BE(b.readUInt32BE(72) + 1, 72); sumFix(b); }),
  badsum: (c) => editBlock(c, 0, (b) => { b[212] ^= 1; }),
  shortsum: (c) => editBlock(c, 0, (b) => { b.writeUInt32BE(40, 4); sumFix(b, 40); }),
  highbits: (c) => editBlock(c, 0, (b) => { b[215] = 0x21; sumFix(b); }),
};
// A wizard's table with the marker, left on the card by a power cut at the offer.
async function makeMarked(spec) {
  mkdirSync(path.dirname(markedPath(spec.key)), { recursive: true });
  rmSync(markedPath(spec.key), { force: true });
  return cardRun(spec, 'marked', { make: usedCard, keep: AFTER_CARD }, async (s, card) => {
    await s.step('A marked table, for the rest to start from', async () => {
      await rerunToOffer(s, card);
      await sleep(1000);
      await s.e.stop();
      eq(markerOn(card), 3, 'marker on the card');
      copyFileSync(card, markedPath(spec.key));
    });
    return null;
  });
}
// A press on Quick setup that lands inside the startup check, every time. The check is short, so it is held: stopped on the way
// into its last read of the card, HDPart is sent through dos.library's Delay first and comes back to the same place. Only that
// task waits, so Intuition takes the press as it would on a slow drive. Leaves HDPart past the check.
async function pressDuringCheck(s, more) {
  const e = s.e, m = await s.main(), g = await e.gadget(m, GID.QUICK);
  const x = (await e.i16(m + 4)) + g.left + (g.width >> 1), y = (await e.i16(m + 6)) + g.top + (g.height >> 1);
  const reg = (regs, r) => parseInt(regs.match(new RegExp(`${r}=\\$([0-9A-F]+)`))[1], 16);
  for (let tries = 0; tries < 12; tries++) {
    await e.stop();
    await e.start();
    await e.locate(90000, 0);      // as soon as it is loaded: the check is short
    const bp = e.addr('qs_marker_on_card');
    await e.tool('winuae_breakpoint_set', { address: bp });
    await e.go();
    try { await e.tool('winuae_wait_stop', { timeout_ms: 4000 }); } catch { s.note(`try ${tries + 1}: the check went by first`); continue; }
    const regs = await e.tool('winuae_registers_get');
    if (reg(regs, 'PC') !== bp) { s.note(`try ${tries + 1}: stopped elsewhere`); continue; }
    eq(await e.g32('g_qs_resume_checks'), 1, 'stopped inside the startup check');
    const a6 = reg(regs, 'A6'), sp = reg(regs, 'A7'), dos = await e.g32('DOSBase');
    await write(e, sp - 4, long(bp));
    await e.tool('winuae_registers_set', { D1: 500, A6: dos, A7: sp - 4, PC: dos - 198 });
    await e.go();
    await e.moveTo(x, y);
    await e.seq([{ after_frames: 1, action: 'button', button: 0, pressed: true, port: 0 }, { after_frames: 2, action: 'button', button: 0, pressed: false, port: 0 }]);
    eq(await s.status(), 'Looking for your card...', 'the status during the check');
    if (more) await more(await s.main());
    ok(reg(await e.tool('winuae_registers_get'), 'PC') !== bp, 'the input landed before the check went on');
    eq(await e.g32('g_qs_screen'), 0, 'nothing open yet');
    await e.go();
    await e.tool('winuae_wait_stop', { timeout_ms: 60000 });
    const back = await e.tool('winuae_registers_get');
    eq(reg(back, 'PC'), bp, 'back from the wait');
    eq(reg(back, 'A7'), sp, 'stack as it was');
    await e.tool('winuae_breakpoint_clear', { address: bp });
    await e.tool('winuae_registers_set', { A6: a6 });
    s.note(`try ${tries + 1}: the press landed inside the check`);
    await e.go();
    await e.waitFor('volcheck listening', async () => (await e.u32(0x17C)) === 0x53524459, 30000);
    return;
  }
  throw new Error('the check was not caught in 12 boots');
}
// Quick setup, then Driver, then a shortcut key, all during the check on a marked card. Then the popup and `answer`.
function pressThenPopup(spec, name, what, answer, marker = -1) {
  return cardRun(spec, name, { make: fromMarked(spec.key), keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step(`Input during the check on a marked card, then ${what}: the popup waits, and what was queued is dropped`, async () => {
      await popupScreen(s);       // this first boot is only for the button's place
      await pressDuringCheck(s, async (m) => { await e.click(m, GID.DEVICE); await e.type('t'); });
      const w = await popupScreen(s), past = hashFile(card);
      await sleep(1500);
      eq(await e.g32('g_qs_screen'), QS_POPUP, 'the input was not taken for an answer');
      eq(hashFile(card), past, 'hash of the card with the popup up');
      await answer(s, w, card);
      await e.go();
      await sleep(3000);
      eq((await e.windows()).map((x) => x.title).filter((t) => !t.startsWith('HDPart ')).join(), '', 'windows opened by what was queued');
      await s.idle();
      eq(await e.g32('g_qs_screen'), 0, 'wizard shut');
      await mainShows(s, 3);
      eq(markerOn(card), marker, 'marker');
      noRequesters(s);
    });
    return null;
  });
}
const npStep = (s, card, was, what, stop) => s.step(`No popup: ${what}`, async () => { await noPopup(s, card, was, stop); noRequesters(s); return null; });
const NOPOPUP_RUNS = {
  // The control: this table does bring the popup.
  control: (spec) => cardRun(spec, 'np-control', { make: fromMarked(spec.key), keep: AFTER_CARD }, async (s, card, was) => {
    await s.step('The table before any edit does bring the popup, and the check itself writes nothing', async () => {
      await popupScreen(s);
      eq(hashFile(card), was, 'hash of the card');
    });
    return null;
  }),
  blank: (spec) => cardRun(spec, 'np-blank', { keep: AFTER_CARD }, async (s, card, was) => npStep(s, card, was, 'blank card')),
  pc: (spec) => cardRun(spec, 'np-pc', { make: pcCard, keep: AFTER_CARD }, async (s, card, was) => npStep(s, card, was, 'PC card')),
  othertool: (spec) => cardRun(spec, 'np-othertool', { make: usedCard, keep: AFTER_CARD }, async (s, card, was) => npStep(s, card, was, 'a table from another tool')),
  // A table laid out just like the wizard's, with no marker: the layout alone must not trigger it.
  samelayout: (spec) => cardRun(spec, 'np-samelayout', { make: fromMarked(spec.key, (c) => editBlock(c, 0, (b) => { b.fill(0, 188, 216); sumFix(b); })), keep: AFTER_CARD }, async (s, card, was) =>
    npStep(s, card, was, 'the same layout and names with no marker')),
  normalsave: (spec) => cardRun(spec, 'np-normalsave', { keep: AFTER_CARD }, async (s, card) => {
    await s.step('A table from HDPart\'s normal Save', async () => {
      await s.pickScsi(0);
      await s.split(3);
      await s.save();
      eq(markerOn(card), -1, 'no marker');
      await nextBootNoPopup(s, card);
    });
    return null;
  }),
  // The main window with a marked table in it: only after a failed resume. Save over it. Save needs an edit to enable it.
  saveover: (spec) => cardRun(spec, 'np-saveover', { make: fromMarked(spec.key), keep: AFTER_CARD }, async (s, card) => {
    const e = s.e;
    await s.step('A normal Save over a marked table clears it for good', async () => {
      await realFormatFailure(s, 1, async () => e.click(await popupScreen(s), QS.CONTINUE));
      await wizardBox(s, /Formatting System failed/);
      eq(markerOn(card), 3, 'still marked');
      await s.selectPart(2);
      await s.clickMain(GID.DELETE);
      await e.clickClose(await e.waitWin('Delete Partition'), PROCEED);
      await s.save();
      eq(markerOn(card), -1, 'marker gone');
      eq(readRdb(card).parts.length, 2, 'the saved table');
      await nextBootNoPopup(s, card);
    });
    return null;
  }),
  finished: (spec) => cardRun(spec, 'np-finished', { keep: AFTER_CARD }, async (s, card) => {
    await s.step('A finished wizard card', async () => {
      await fullRun(s);
      await backToMain(s);
      await nextBootNoPopup(s, card);
    });
    return null;
  }),
  booted: (spec) => {
    const dir = `${ROOT}/out/target-quick/${spec.key}/card-np-booted-files`;
    const make = (card) => {
      copyFileSync(markedPath(spec.key), card);
      mkdirSync(dir, { recursive: true });
      writeFileSync(`${dir}/startup-sequence`, 'Run >NIL: volcheck serve\nHDPart\n');
      const r = readRdb(card), t = r.parts[0], tmp = `${card}.vol.hdf`, fd = openSync(card, 'r+');
      rmSync(tmp, { force: true });
      execFileSync('xdftool', [tmp, 'create', `chs=${t.high - t.low + 1},1,${r.cylBlocks}`, '+', 'format', 'CardBoot', 'ffs+intl', '+', 'makedir', 's', '+', 'write', `${dir}/startup-sequence`, 's/startup-sequence',
        '+', 'write', `${ROOT}/out/HDPart.exe`, 'HDPart', '+', 'write', `${ROOT}/out/volcheck`, 'volcheck'], { stdio: 'pipe' });
      const d = readFileSync(tmp);
      writeSync(fd, d, 0, d.length, t.low * r.cylBlocks * 512);
      closeSync(fd);
      rmSync(tmp, { force: true });
    };
    return cardRun(spec, 'np-booted', { make, keep: { bootPri: -128 } }, async (s, card, was) => {
      await s.step('The Amiga really started from the card', async () => {
        const seg = JSON.parse(await s.e.tool('winuae_tasks')).map((t) => t.name);
        await s.e.go();
        ok(seg.includes('DH0'), `handlers: ${seg.join()}`);
        eq(markerOn(card), 3, 'marker on the card');
      });
      return npStep(s, card, was, 'a marked card it started from', CHECK_STOP.REFUSED + QS.BOOTED);
    });
  },
  two: (spec) => {
    const second = `${ROOT}/out/target-quick/${spec.key}/card-np-two.hdf.2`;
    const make = (c) => { fromMarked(spec.key)(c); const fd = openSync(second, 'w'); ftruncateSync(fd, 256 * MiB); closeSync(fd); };
    return cardRun(spec, 'np-two', { make, keep: { ...AFTER_CARD, cfg: () => [`hardfile2=rw,:${second},0,0,0,512,-128,,ide1`] } }, async (s, card, was) =>
      npStep(s, card, was, 'a marked card with another drive beside it', CHECK_STOP.REFUSED + QS.TWO_DRIVES));
  },
  // The ROM mounts the card ahead of this disk, so DH0 goes to the card and the other is renamed: nothing is taken. DH3 is free
  // for the other disk to hold, and the refusals ask for all four names.
  dh3: (spec) => {
    const keep = { dirs: ['other'], bootPri: 5, cfg: (dir) => [`filesystem2=ro,DH3:Other:${dir}/other,-128`] };
    return cardRun(spec, 'np-dh3', { make: fromMarked(spec.key), keep }, async (s, card, was) => {
      ok((await dosMounts(s.e)).some((m) => m.name === 'DH3' && m.driver.startsWith('uaehf')), 'another disk holds DH3');
      return npStep(s, card, was, 'a marked card with a name taken by another disk', CHECK_STOP.REFUSED + QS.NAME_TAKEN);
    });
  },
  // Input during the startup check is queued, and acted on after it. It must not be lost, doubled or taken for an answer.
  startupclick: (spec) => cardRun(spec, 'np-startupclick', { keep: AFTER_CARD }, async (s, card, was) => {
    const e = s.e;
    await s.step('A press on Quick setup during the startup check opens the wizard after it, once, and writes nothing', async () => {
      await pressDuringCheck(s);
      const w = await e.waitWin('Quick setup: your card', 120000);
      eq(await e.g32('g_qs_resume_checks'), 1, 'the check ran once');
      await e.clickClose(w, QS.CANCEL);
      await closed(s);
      await sleep(1000);
      ok(!(await e.win('Quick setup')), 'opened once');
      await mainUntouched(s);
      eq(hashFile(card), was, 'hash of the card');
      noRequesters(s);
    });
    return null;
  }),
  // On a marked card the popup comes first, and the input is not its answer. Whatever the popup led to, what was queued behind
  // it is dropped: it must not open the erase screen, or anything else, on a card that was just dealt with.
  startupclickcancel: (spec) => pressThenPopup(spec, 'np-startupclickc', 'Cancel', async (s, w) => {
    await s.e.clickClose(w, QS.CANCEL);
    await closed(s);
  }),
  startupclickcontinue: (spec) => pressThenPopup(spec, 'np-startupclickg', 'Continue, Finished, Back to HDPart', async (s, w, card) => {
    await s.e.click(w, QS.CONTINUE);
    await finished(s);
    checkVolumes(s, card, VOLUMES);
    await backToMain(s);
  }),
  startupclickfail: (spec) => pressThenPopup(spec, 'np-startupclickf', 'Continue and a failure box', async (s, w) => {
    await realFormatFailure(s, 2, () => s.e.click(w, QS.CONTINUE));
    await wizardBox(s, /Formatting Work failed/);
  }, 3),
  ...Object.fromEntries(Object.entries(MARKED_EDITS).map(([name, edit]) => [`edit-${name}`, (spec) =>
    cardRun(spec, `np-edit-${name}`, { make: fromMarked(spec.key, edit), keep: AFTER_CARD }, async (s, card, was) =>
      npStep(s, card, was, `a marked table edited: ${name}`))])),
};
// Machines answering 0 from the IDE check, with a marked table on the card.
async function noPopupNotIde(spec, key) {
  const c = IDE_CHECK[key], rom = romFor(c.rom || key);
  if (!rom || !existsSync(rom)) { console.log(`--- skip no-popup on ${key}: ROM not set`); return null; }
  const envs = Object.values(c.env || {});
  if (envs.some((v) => !process.env[v] || !existsSync(process.env[v]))) { console.log(`--- skip no-popup on ${key}: files not set`); return null; }
  const files = Object.fromEntries(Object.entries(c.env || {}).map(([name, v]) => [name, process.env[v]]));
  const other = { key, rom, machine: MACHINES[key] };
  return cardRun(other, 'np-notide', { make: fromMarked(spec.key), keep: { ...AFTER_CARD, controller: c.controller, startup: c.startup, files, bootCheck: c } }, async (s, card, was) => {
    await s.step(`No popup on a marked table: ${key} answers 0 from the IDE check`, async () => {
      eq((await s.e.g32('g_qs_is_ide')) | 0, 0, 'the IDE check');
      await noPopup(s, card, was, CHECK_STOP.NOT_IDE);
    });
    return null;
  });
}

async function resumeTest(spec) {
  const res = { test: 'resume', key: spec.key, runs: [] };
  const only = (process.env.QUICK_CARD_ONLY || '').split(',').filter(Boolean);
  const wants = (n) => !only.length || only.includes(n);
  for (const [name, run] of Object.entries(RESUME_RUNS)) if (wants(name)) res.runs.push(await run(spec));
  const np = Object.entries(NOPOPUP_RUNS).filter(([n]) => wants(`np-${n}`));
  const notIde = wants('np-notide') && spec.key === 'ks31a1200';
  if (np.length || notIde) {
    const made = await makeMarked(spec);
    res.runs.push(made);
    for (const [, run] of made.pass ? np : []) res.runs.push(await run(spec));
    if (notIde && made.pass) for (const k of ['a3000ks31', 'ks31softload']) { const r = await noPopupNotIde(spec, k); if (r) res.runs.push(r); }
    rmSync(markedPath(spec.key), { force: true });
  }
  res.pass = res.runs.every((r) => r.pass);
  res.err = res.runs.filter((r) => !r.pass).map((r) => r.scenario).join() || undefined;
  mkdirSync(`${ROOT}/out/target-quick/${spec.key}`, { recursive: true });
  writeFileSync(`${ROOT}/out/target-quick/${spec.key}/resume.json`, JSON.stringify(res, null, 1));
  return res;
}

async function runTest(spec) {
  const res = { test: 'run', key: spec.key, runs: [] };
  const only = (process.env.QUICK_CARD_ONLY || '').split(',').filter(Boolean);
  for (const [name, run] of Object.entries(RUN_RUNS)) {
    if (only.length && !only.includes(name)) continue;
    if (name === 'synth' && spec.key !== 'ks31a4000') continue;     // the only driver seen to need it
    if (name === 'proof4g' && !only.length && !['ks31a1200', 'ks32'].includes(spec.key)) continue;
    res.runs.push(await run(spec));
  }
  res.pass = res.runs.every((r) => r.pass);
  res.err = res.runs.filter((r) => !r.pass).map((r) => r.scenario).join() || undefined;
  mkdirSync(`${ROOT}/out/target-quick/${spec.key}`, { recursive: true });
  writeFileSync(`${ROOT}/out/target-quick/${spec.key}/run.json`, JSON.stringify(res, null, 1));
  return res;
}

async function cardTest(spec) {
  const res = { test: 'card', key: spec.key, runs: [] };
  const only = (process.env.QUICK_CARD_ONLY || '').split(',').filter(Boolean);
  if (REFUSED_MACHINES[spec.key]) {
    const c = IDE_CHECK[spec.key], m = REFUSED_MACHINES[spec.key];
    const files = Object.fromEntries(Object.entries(c.env || {}).map(([name, v]) => [name, process.env[v]]));
    res.runs.push(await cardRun(spec, 'machine', { keep: { controller: c.controller, startup: c.startup, files, bootCheck: c } }, (s, card) =>
      refused(s, [card], m.code, m.words, 'machine')));
  } else {
    for (const [name, run] of Object.entries(CARD_RUNS)) {
      if (only.length && !only.includes(name)) continue;
      if (name === 'synth' && spec.key !== 'ks31a4000') continue;      // the only driver seen to need it
      res.runs.push(await run(spec));
    }
  }
  res.pass = res.runs.every((r) => r.pass);
  res.err = res.runs.filter((r) => !r.pass).map((r) => r.scenario).join() || undefined;
  mkdirSync(`${ROOT}/out/target-quick/${spec.key}`, { recursive: true });
  writeFileSync(`${ROOT}/out/target-quick/${spec.key}/card.json`, JSON.stringify(res, null, 1));
  return res;
}

const TESTS = {
  // keys: the default machines. allowed: what the test can run on when keys are named. Engine tests need a card on IDE.
  ide: { fn: ideCheck, keys: Object.keys(IDE_CHECK), allowed: Object.keys(IDE_CHECK) },
  fresh: { fn: fresh, keys: Object.keys(ENGINE), allowed: Object.keys(ENGINE) },
  used: { fn: used, keys: ['ks31a1200', 'ks32'], allowed: Object.keys(ENGINE) },
  restart: { fn: restartPath, keys: ['ks31a1200', 'ks32'], allowed: Object.keys(ENGINE) },
  limit: { fn: limit, keys: ['ks31a1200', 'ks205a'], allowed: Object.keys(ENGINE) },
  offset: { fn: offset, keys: ['ks31a1200', 'ks205a'], allowed: Object.keys(ENGINE) },
  run: { fn: runTest, keys: Object.keys(ENGINE), allowed: Object.keys(ENGINE) },
  resume: { fn: resumeTest, keys: Object.keys(ENGINE), allowed: Object.keys(ENGINE) },
  card: { fn: cardTest, keys: [...Object.keys(ENGINE), 'ks314a4000', 'ks32a4000', ...Object.keys(REFUSED_MACHINES)], allowed: [...Object.keys(ENGINE), 'ks314a4000', 'ks32a4000', ...Object.keys(REFUSED_MACHINES)] },
};

async function main() {
  const args = process.argv.slice(2), names = args.filter((a) => TESTS[a]), keys = args.filter((a) => !TESTS[a]);
  for (const k of keys) if (!MACHINES[k]) { console.error(`Unknown key or test: ${k}`); process.exit(2); }
  if (!existsSync(ELF) || !existsSync(`${ROOT}/out/volcheck`)) { console.error('Build first: make target-quick'); process.exit(2); }
  const summary = [];
  for (const name of names.length ? names : Object.keys(TESTS)) {
    for (const key of keys.length ? keys : TESTS[name].keys) {
      if (!TESTS[name].allowed.includes(key)) {
        if (names.length) console.log(`--- skip ${name} on ${key}: this test does not run on that machine`);
        continue;
      }
      const cfg = name === 'ide' || (name === 'card' && REFUSED_MACHINES[key]) ? IDE_CHECK[key] : null;
      const romKey = cfg?.rom || key;
      const rom = romFor(romKey);
      if (!rom || !existsSync(rom)) { console.log(`--- skip ${name} on ${key}: QUICK_ROM_${romKey.toUpperCase()} not set`); continue; }
      const missing = Object.values(cfg?.env || {}).find((v) => !process.env[v] || !existsSync(process.env[v]));
      if (missing) { console.log(`--- skip ${name} on ${key}: ${missing} not set`); continue; }
      console.log(`\n=== ${name} on ${key} (${path.basename(rom)}) ===`);
      summary.push(await TESTS[name].fn({ key, rom, machine: MACHINES[key] }));
    }
  }
  console.log('\n=== Summary ===');
  let bad = 0;
  for (const r of summary) {
    // A safe-or-not answer is a result: only a test that could not finish fails the run.
    const pass = r.test === 'used' || r.test === 'restart' ? r.done : r.pass;
    if (!pass) bad++;
    console.log(`${r.test.padEnd(6)} ${r.key.padEnd(10)} ${pass ? 'finished' : 'FAILED'}${r.safe !== undefined ? `, safe: ${r.safe}` : ''}${r.err ? `: ${r.err}` : ''}`);
  }
  process.exit(bad ? 1 : 0);
}
if (import.meta.url === `file://${process.argv[1]}`) main().catch((e) => { console.error(e); process.exit(2); });

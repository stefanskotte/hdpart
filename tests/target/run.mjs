#!/usr/bin/env node
// HDPart on-target test suite: runs every feature on KS 2.04, 3.1 and 3.2 in WinUAE.
// Usage: node tests/target/run.mjs [ks204 ks31 ks32]     See tests/target/README.md.
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import path from 'node:path';
import { Emu, sleep } from './emu.mjs';
import { readRdb, partInfo } from './rdbcheck.mjs';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const ROMS = process.env.KICKSTART_DIR || `${process.env.HOME}/Documents/FS-UAE/Kickstarts`;
const HANDLER = process.env.HDPART_TEST_FS || `${ROOT}/amiga_hd/pfs3aio`;
const ADFVER = readFileSync(`${ROOT}/Makefile`, 'utf8').match(/^ADFVER = (\S+)/m)[1];
const ELF = `${ROOT}/out/HDPart.elf`;

const A500P = ['cpu_model=68000', 'chipset=ecs', 'chipset_compatible=A500+', 'chipmem_size=4'];
const A1200 = ['cpu_model=68020', 'chipset=aga', 'chipset_compatible=A1200', 'chipmem_size=4', 'fastmem_size=8'];
const KICKSTARTS = {
  ks204: { label: 'KS 2.04', rom: process.env.KS204_ROM || `${ROMS}/amiga-os-204.rom`, machine: A500P, major: 37 },
  ks31: { label: 'KS 3.1', rom: process.env.KS31_ROM || `${ROMS}/amiga-os-310-a1200.rom`, machine: A1200, major: 40 },
  ks32: { label: 'KS 3.2', rom: process.env.KS32_ROM || `${ROMS}/kicka1200.rom`, machine: A1200, major: 47 },
};

// src/gui.c gadget IDs and menu indices.
const GID = { DEVICE: 1, SCAN: 2, DRIVER: 3, NEW: 5, DELETE: 6, EDIT: 7, SAVE: 9, UNIT: 11, SPLIT: 12, REFRESH: 13, RESIZE: 14, FORMAT: 15, FS: 16 };
const PROCEED = 1, CANCEL = 0, OK = 1;                     // gui_request
const ED = { NAME: 1, FS: 4, OK: 10, CANCEL: 11 };         // gui_edit_dialog
const FSD = { LIST: 1, PRESET: 3, ADD: 4, REMOVE: 5, DONE: 6, COPY: 7 };   // gui_filesystems
const PFS3 = 0x50465303;
const RAMIGA = 0x67;

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${JSON.stringify(b)}, got ${JSON.stringify(a)}`); }
function ok(c, what) { if (!c) throw new Error(what); }
const sha = (f) => createHash('sha1').update(readFileSync(f)).digest('hex');

class Suite {
  constructor(key, ks) {
    this.key = key; this.ks = ks;
    this.dir = `${ROOT}/out/target-test/${key}`;
    this.results = [];
  }

  prepare() {
    rmSync(this.dir, { recursive: true, force: true });
    for (const d of ['boot/s', 'files', 'shots']) mkdirSync(`${this.dir}/${d}`, { recursive: true });
    copyFileSync(`${ROOT}/out/HDPart.exe`, `${this.dir}/boot/HDPart`);
    writeFileSync(`${this.dir}/boot/s/startup-sequence`, 'HDPart\n');
    copyFileSync(HANDLER, `${this.dir}/files/pfs3aio`);
    writeFileSync(`${this.dir}/files/notes.txt`, 'not a load file\n');
    this.diskA = `${this.dir}/a.hdf`; this.diskB = `${this.dir}/b.hdf`;
    writeFileSync(this.diskA, Buffer.alloc(8 << 20)); writeFileSync(this.diskB, Buffer.alloc(16 << 20));
    this.cfg = `${this.dir}/test.uae`;
    writeFileSync(this.cfg, [
      'use_gui=no', `kickstart_rom_file=${this.ks.rom}`, ...this.ks.machine, 'nr_floppies=1', 'sound_output=none',
      `filesystem2=ro,TESTBOOT:TESTBOOT:${this.dir}/boot,0`, `filesystem2=ro,FILES:FILES:${this.dir}/files,-128`,
      `hardfile2=rw,:${this.diskA},0,0,0,512,-128,,uae`, `hardfile2=rw,:${this.diskB},0,0,0,512,-128,,uae`, '',
    ].join('\n'));
    this.handler = readFileSync(HANDLER);
    this.e = new Emu(this.cfg, ELF, `${this.dir}/emulator.log`);
  }

  async step(name, fn) {
    const t = Date.now();
    try {
      await fn();
      this.results.push({ name, pass: true });
      console.log(`  ok   ${name} (${((Date.now() - t) / 1000).toFixed(1)}s)`);
    } catch (err) {
      this.results.push({ name, pass: false, err: err.message });
      console.log(`  FAIL ${name}: ${err.message}`);
      try { await this.e.shot(`${this.dir}/shots/FAIL-${this.results.length}.png`); } catch { /* emulator gone */ }
      try {   // what the program was doing: a timeout alone can't tell busy from stuck
        const task = JSON.parse(await this.e.tool('winuae_tasks')).find((x) => x.address === this.e.process);
        console.log(`       task: ${JSON.stringify(task)}`);
        console.log(`       windows: ${JSON.stringify((await this.e.windows()).map((w) => w.title))}`);
        console.log(`       unit=${await this.e.g32('g_cur_unitidx')} dirty=${await this.dirty()} status="${await this.status()}"`);
      } catch { /* emulator gone */ }
      throw err;
    }
  }

  /* ---- GUI state ---- */
  main() { return this.e.win('HDPart '); }
  async dirty() { return this.e.g32('g_dirty'); }
  async nparts() { return this.e.u32(this.e.addr('g_model') + 36); }
  async sel() { return (await this.e.g32('g_sel_part')) | 0; }
  async status() { return this.e.gstr('g_statusbuf'); }
  async row(i) { return this.e.gstr('g_partrows', i * 80); }
  async rows() { const n = await this.nparts(), o = []; for (let i = 0; i < n; i++) o.push(await this.row(i)); await this.e.go(); return o; }
  async clickMain(id) { await this.e.click(await this.main(), id); }
  // Menus and keys go to the active window; clicking the title bar activates ours.
  async activateMain() {
    const e = this.e, w = await this.main();
    await e.clickAt((await e.i16(w + 4)) + ((await e.i16(w + 8)) >> 1), (await e.i16(w + 6)) + 4);
  }

  // Wait until the program blocks on its main window, i.e. has finished reacting.
  // Reading a disk with an embedded handler takes seconds on a 7 MHz 68000.
  async idle(timeout = 60000) {
    const e = this.e, w = await this.main();
    const sig = await e.u8((await e.u32(w + 86)) + 15), mask = (1 << sig) >>> 0;
    await e.waitFor('program idle', async () => {
      const t = JSON.parse(await e.tool('winuae_tasks')).find((x) => x.address === e.process);
      return t && t.list === 'waiting' && t.signals_wait === mask;
    }, timeout);
  }

  // Two-button gui_request boxes carry a gadget with ID 0.
  async isChoice(w) { return (await this.e.gadgets(w)).some((g) => g.id === CANCEL); }
  async answer(title, id) { const w = await this.e.waitWin(title); await this.e.clickClose(w, id); }

  async pickCycle(gid, idxSym, labelsSym, want) {
    const e = this.e;
    for (let i = 0; i < 40; i++) {
      const idx = (await e.g32(idxSym)) | 0;
      const label = await e.cstr(await e.u32(e.addr(labelsSym) + Math.max(idx, 0) * 4), 40);
      if (idx >= 0 && label.includes(want)) { await this.idle(); return; }
      await this.clickMain(gid);
      await sleep(500);
    }
    throw new Error(`no ${labelsSym} entry containing "${want}"`);
  }
  pickDriver(name) { return this.pickCycle(GID.DEVICE, 'g_drv_idx', 'g_drvlabels', name); }
  // Click row `i` of a hand-drawn list (ListGeom: x y w h rowh vis top count).
  async clickRow(sym, i) {
    const e = this.e, w = await this.main(), b = await e.mem(e.addr(sym), 16);
    const x = b.readInt16BE(0), y = b.readInt16BE(2), width = b.readInt16BE(4), rowh = b.readInt16BE(8), top = b.readInt16BE(12);
    await e.clickAt((await e.i16(w + 4)) + x + (width >> 1), (await e.i16(w + 6)) + y + (i - top) * rowh + (rowh >> 1));
  }
  async unitRow(text) {
    const e = this.e, n = await e.g32('g_nunits');
    for (let i = 0; i < n; i++) if ((await e.gstr('g_unittext', i * 24, 24)).includes(text)) return i;
    throw new Error(`no unit containing "${text}"`);
  }
  async pickUnit(text) {
    const row = await this.unitRow(text);
    if (((await this.e.g32('g_cur_unitidx')) | 0) !== row) await this.clickRow('g_ulist', row);
    await this.e.waitFor('unit shown', async () => ((await this.e.g32('g_cur_unitidx')) | 0) === row);
    await this.idle();
  }
  // A listed unit other than the current one.
  async otherUnitRow() {
    const cur = (await this.e.g32('g_cur_unitidx')) | 0;
    return cur === 0 ? 1 : 0;
  }

  async scanTo(unit) {
    await this.pickDriver('uaehf.device');
    await this.clickMain(GID.SCAN);
    await this.e.waitFor('units after Scan', async () => (await this.e.g32('g_nunits')) >= 2, 30000);
    await this.pickUnit(unit);
  }

  // Select partition i by clicking its slice of the disk-map bar.
  async selectPart(i) {
    const e = this.e, w = await this.main();
    const cyl = await e.u32(e.addr('g_model'));
    const nums = (await this.row(i)).trim().split(/\s+/).filter((x) => /^\d+$/.test(x)).map(Number);
    const [low, high] = nums.slice(-2);
    const bar = await e.mem(e.addr('g_bar'), 8);
    const x = bar.readInt16BE(0) + Math.floor(((low + high + 1) / 2 * bar.readInt16BE(4)) / cyl);
    await e.clickAt((await e.i16(w + 4)) + x, (await e.i16(w + 6)) + bar.readInt16BE(2) + (bar.readInt16BE(6) >> 1));
    await e.waitFor(`partition ${i} selected`, async () => (await this.sel()) === i);
  }

  async pickFile(title, file) {
    const e = this.e;
    if (await e.g32('AslBase')) throw new Error('asl.library is open: this test expects the typed-path dialog');
    const w = await e.waitWin(title);
    await e.typeInto(w, 20, file);
    await e.clickClose(w, 21);
  }

  // Answers Save's confirmations and its result box. With `atLastProceed`, that
  // function presses the first Proceed instead and the flow stops there (only valid
  // on a disk with nothing mounted, where there is a single confirmation).
  async save({ atLastProceed } = {}) {
    const e = this.e;
    await this.clickMain(GID.SAVE);
    for (let i = 0; i < 4; i++) {
      const w = await e.waitWin('Save', 30000);
      if (!(await this.isChoice(w))) { await e.clickClose(w, OK); return; }
      if (atLastProceed) { await atLastProceed(w); return; }
      await e.clickClose(w, PROCEED, 40000);
    }
    throw new Error('too many Save dialogs');
  }

  async addHandler() {
    const e = this.e, w = await e.waitWin('Filesystems');
    await e.click(w, FSD.ADD);
    await this.pickFile('Select a filesystem handler', 'FILES:pfs3aio');
  }

  // Format partition i. Returns the confirm text so callers can check what it says.
  async format(i, label) {
    const e = this.e;
    await this.selectPart(i);
    await e.tool('winuae_memory_write', { address: e.addr('g_statusbuf'), data: '00' });   // no stale "Formatted"
    await e.go();
    await this.clickMain(GID.FORMAT);
    const w = await e.waitWin('Format');
    const text = await e.gstr('msg', 0, 200);
    await e.clickClose(w, PROCEED);
    const v = await e.waitWin('Volume name');
    await e.typeInto(v, 20, label);
    await e.clickClose(v, 21);
    await e.waitFor('format result', async () => {
      if ((await this.status()).includes('Formatted')) return true;
      if (await e.win('Format')) throw new Error('Format reported an error');
      return false;
    }, 60000);
    return text;
  }

  /* ---- the suite ---- */
  async run() {
    const e = this.e;
    console.log(`\n=== ${this.ks.label} (${path.basename(this.ks.rom)}) ===`);
    await e.start();
    await this.boot();

    await this.step('dialog: New then Cancel leaves the table untouched', async () => {
      await this.clickMain(GID.NEW);
      await this.answer('Edit Partition', ED.CANCEL);
      eq(await this.nparts(), 0, 'partitions'); eq(await this.dirty(), 0, 'dirty');
    });
    await this.step('New: creates DH0 spanning the free space', async () => {
      await this.clickMain(GID.NEW);
      await this.answer('Edit Partition', ED.OK);
      eq(await this.nparts(), 1, 'partitions'); eq(await this.dirty(), 1, 'dirty');
      ok((await this.row(0)).includes('DH0'), 'row names DH0');
      ok((await this.row(0)).includes('FFS Intl'), 'row shows FFS Intl');
    });
    await this.step('Edit: rename + change filesystem', async () => {
      await this.clickMain(GID.EDIT);
      const w = await e.waitWin('Edit Partition');
      await e.typeInto(w, ED.NAME, 'WORK');
      await e.click(w, ED.FS); await sleep(300);
      await e.clickClose(w, ED.OK);
      const r = await this.row(0);
      ok(r.includes('WORK') && r.includes('OFS'), `row after edit: ${r}`);
    });
    await this.step('Delete: Cancel keeps it, Proceed removes it', async () => {
      await this.clickMain(GID.DELETE);
      await this.answer('Delete Partition', CANCEL);
      eq(await this.nparts(), 1, 'after Cancel');
      await this.clickMain(GID.DELETE);
      await this.answer('Delete Partition', PROCEED);
      eq(await this.nparts(), 0, 'after Proceed');
    });
    await this.step('Split: stepper picks 2 partitions', async () => {
      await this.clickMain(GID.SPLIT);
      const w = await e.waitWin('Split remaining');
      await e.click(w, 2); await sleep(200); await e.click(w, 2); await sleep(200);   // 4 -> 2
      await e.clickClose(w, 10);
      const r = await this.rows();
      eq(r.length, 2, 'partitions');
      ok(r[0].includes('DH0') && r[1].includes('DH1'), `rows ${r}`);
    });
    await this.step('Disk map and table: clicking selects that partition', async () => {
      await this.selectPart(1); await this.selectPart(0);
      await this.clickRow('g_plist', 1);
      await e.waitFor('table row selected', async () => (await this.sel()) === 1);
      await this.clickRow('g_plist', 0);
      await e.waitFor('table row selected', async () => (await this.sel()) === 0);
    });
    await this.step('Resize: shrink with confirmation', async () => {
      const before = await this.row(0);
      await this.clickMain(GID.RESIZE);
      const w = await e.waitWin('Resize partition');
      await sleep(1500);                 // gadgets take a moment to render on a 68000
      await e.shot(`${this.dir}/shots/resize.png`);
      await e.typeInto(w, 2, '1');       // no Return: clicking Ok must still take the number
      await sleep(400);
      await e.click(w, 10);
      await e.waitFor('destructive-change confirmation',
        async () => (await e.windows()).filter((x) => x.title === 'Resize partition').length === 2);
      const c = await e.win('Resize partition');
      await e.clickClose(c, PROCEED);
      await e.waitFor('resize dialog closed', async () => !(await e.win('Resize partition')));
      ok((await this.row(0)) !== before, 'row unchanged after resize');
    });
    await this.step('New: fills the gap the resize left', async () => {
      await this.clickMain(GID.NEW);
      await this.answer('Edit Partition', ED.OK);
      eq(await this.nparts(), 3, 'partitions');
    });
    await this.step('Resize: Ok with the size untouched changes nothing', async () => {
      const before = await this.rows();  // 1008-block cylinders: 1.97 MB shows as 1, which is 3 cylinders
      await this.selectPart(1);
      await this.clickMain(GID.RESIZE);
      const w = await e.waitWin('Resize partition');
      await sleep(400);
      await e.clickClose(w, 10);
      await this.idle();
      eq(JSON.stringify(await this.rows()), JSON.stringify(before), 'table');
    });
    await this.step('Save: writes a valid RDB, verified from the host', async () => {
      await this.save();
      eq(await this.dirty(), 0, 'dirty');
      const r = readRdb(this.diskA);
      eq(r.parts.map((p) => p.name).join(), 'DH0,DH1,DH2', 'names on disk');
      eq(r.fs.length, 0, 'no filesystem embedded');
      eq(r.parts[0].maxTransfer, 0x1FE00, 'new partition uses the IDE-safe MaxTransfer');
      this.expectedRows = await this.rows();
    });
    await this.step('Refresh: re-reads the same table', async () => {
      await this.clickMain(GID.REFRESH);
      await sleep(1500);
      eq((await this.rows()).join('|').replace(/>/g, ' '), this.expectedRows.join('|').replace(/>/g, ' '), 'rows');
    });
    await this.step('Unsaved edits: Unit, Driver, Scan, Load, Refresh all ask; Cancel keeps them', async () => {
      await this.selectPart(2);
      await this.clickMain(GID.DELETE);
      await this.answer('Delete Partition', PROCEED);
      eq(await this.dirty(), 1, 'dirty');
      const unit = await e.g32('g_cur_unitidx'), drv = await e.g32('g_drv_idx');
      for (const [gid, title] of [[0, 'Unit'], [GID.DEVICE, 'Driver'], [GID.SCAN, 'Scan'], [GID.DRIVER, 'Load Driver'], [GID.REFRESH, 'Refresh']]) {
        if (gid) await this.clickMain(gid); else await this.clickRow('g_ulist', await this.otherUnitRow());
        await this.answer(title, CANCEL);
        eq(await this.dirty(), 1, `${title}: still dirty`);
        eq(await this.nparts(), 2, `${title}: edits kept`);
        eq(await e.g32('g_cur_unitidx'), unit, `${title}: unit`);
        eq(await e.g32('g_drv_idx'), drv, `${title}: driver`);
      }
      await e.shot(`${this.dir}/shots/guards.png`);
    });
    await this.step('Unsaved edits: Unit + Proceed discards and switches', async () => {
      const unit = await e.g32('g_cur_unitidx');
      await this.clickRow('g_ulist', await this.otherUnitRow());
      await this.answer('Unit', PROCEED);
      await e.waitFor('unit change', async () => (await e.g32('g_cur_unitidx')) !== unit);
      eq(await this.dirty(), 0, 'dirty');
      await this.pickUnit(' 8 MB');
      eq(await this.nparts(), 3, 'saved table reloaded');
    });
    await this.step('Edit: presets, help buttons and validation for the advanced settings', async () => {
      await this.selectPart(2);
      await this.clickMain(GID.EDIT);
      const w = await e.waitWin('Edit Partition');
      await sleep(1500);                 // gadgets take a moment to render on a 68000
      await e.shot(`${this.dir}/shots/edit-advanced.png`);
      for (const [q, title] of [[20, 'MaxTransfer'], [21, 'Mask'], [22, 'Buffers'], [23, 'Buffer memory']]) {
        await e.click(w, q);
        const help = await e.waitWin(title);
        if (q === 22) { await sleep(1500); await e.shot(`${this.dir}/shots/help-buffers.png`); }
        await e.clickClose(help, OK);
      }
      // Custom MaxTransfer at the top bit is refused, with the dialog left open.
      for (let i = 0; i < 3; i++) { await e.click(w, 7); await sleep(200); }   // IDE safe -> Custom
      await e.typeInto(w, 12, '0x80000000');
      await e.click(w, ED.OK);
      const box = await e.waitFor('refusal message', async () => (await e.windows()).find((x) => x.title === 'Edit')?.addr);
      await e.clickClose(box, OK);
      ok(await e.win('Edit Partition'), 'dialog stays open after a refused value');
      await e.typeInto(w, 12, '0x00020000');
      await e.click(w, 16); await sleep(200); await e.click(w, 16); await sleep(200);   // Buffers -> More cache
      await e.click(w, 18); await sleep(200);                                            // Buffer memory -> Chip
      await e.click(w, 8); await sleep(200);                                             // Mask -> Zorro II DMA
      await e.clickClose(w, ED.OK);
      await this.save();
      const p = readRdb(this.diskA).parts[2];
      eq(p.maxTransfer, 0x20000, 'MaxTransfer on disk');
      eq(p.numBuffers, 100, 'Buffers on disk');
      eq(p.bufMemType, 3, 'Buffer memory on disk');
      eq(p.mask, 0x00FFFFFE, 'Mask on disk');
    });
    await this.step('Filesystems: Add from file detects PFS\\3 and its version', async () => {
      await this.clickMain(GID.FS);
      await this.addHandler();
      await e.waitFor('pool entry', async () => (await e.g32('g_fs_npool')) === 1);
      const r = await e.gstr('g_fsrows');
      ok(r.includes('PFS\\3') && /v\d+\.\d+/.test(r) && r.includes('pfs3aio'), `row: ${r}`);
    });
    await this.step('Filesystems: a non-load file is refused', async () => {
      const w = await e.win('Filesystems');
      await e.click(w, FSD.ADD);
      await this.pickFile('Select a filesystem handler', 'FILES:notes.txt');
      await e.waitFor('error box', async () => (await e.windows()).filter((x) => x.title === 'Filesystems').length === 2);
      await this.answer('Filesystems', OK);
      eq(await e.g32('g_fs_npool'), 1, 'pool unchanged');
    });
    await this.step('Filesystems: Remove empties the pool, Add restores it', async () => {
      const w = await e.win('Filesystems');
      await e.click(w, FSD.REMOVE);
      await e.waitFor('removed', async () => (await e.g32('g_fs_npool')) === 0);
      await this.addHandler();
      await e.waitFor('re-added', async () => (await e.g32('g_fs_npool')) === 1);
      await this.answer('Filesystems', FSD.DONE);
    });
    await this.step('Edit: the loaded filesystem can be assigned', async () => {
      await this.selectPart(1);
      await this.clickMain(GID.EDIT);
      const w = await e.waitWin('Edit Partition');
      await e.click(w, ED.FS); await sleep(250); await e.click(w, ED.FS); await sleep(250);
      await e.shot(`${this.dir}/shots/edit.png`);
      await e.clickClose(w, ED.OK);
      ok((await this.row(1)).includes('PFS\\3'), `row: ${await this.row(1)}`);
      eq(await this.dirty(), 1, 'dirty');
    });
    await this.step('Save: out of memory for the handler writes nothing and says so', async () => {
      const before = sha(this.diskA);
      await this.save({ atLastProceed: (w) => e.failNextCall('rdb_seg_alloc', () => e.click(w, PROCEED)) });
      await e.waitFor('error box', async () => {
        const w = await e.win('Save');
        return w && !(await this.isChoice(w));
      });
      await this.answer('Save', OK);
      eq(sha(this.diskA), before, 'disk bytes');
      eq(await this.dirty(), 1, 'still dirty');
    });
    await this.step('Save: embeds the handler byte-for-byte with SegList in PatchFlags', async () => {
      await this.save();
      const r = readRdb(this.diskA);
      eq(r.fs.length, 1, 'filesystems on disk');
      eq(r.fs[0].dosType, PFS3, 'FSHD DosType');
      ok(r.fs[0].patchFlags & 0x80, 'PatchFlags has SegList');
      ok(r.fs[0].seg.subarray(0, this.handler.length).equals(this.handler), 'LSEG bytes differ from the handler');
      eq(r.parts[1].dosType, PFS3, 'partition DosType');
    });
    await this.step('Format: fresh PFS partition, no reboot, handler stays live', async () => {
      await this.format(1, 'PFSVOL');
      const r = readRdb(this.diskA), p = partInfo(this.diskA, r, r.parts[1]);
      eq((p.magic >>> 8).toString(16), '504653', 'PFS signature');
      ok(p.hasLabel('PFSVOL'), 'label on disk');
      ok(JSON.parse(await e.tool('winuae_tasks')).some((t) => t.name === 'DH1'), 'DH1 handler running');
      await e.go();
    });
    await this.step('Format: fresh FFS partition', async () => {
      await this.format(0, 'FFSVOL');
      const r = readRdb(this.diskA), p = partInfo(this.diskA, r, r.parts[0]);
      eq((p.magic >>> 8).toString(16), '444f53', 'DOS signature');
      ok(p.hasLabel('FFSVOL'), 'label on disk');
    });
    await this.step('Format: a mounted partition is named in the warning and reformatted', async () => {
      const text = await this.format(1, 'PFSTWO');
      ok(/mounted as/.test(text), `confirm text: ${text}`);
      const r = readRdb(this.diskA);
      ok(partInfo(this.diskA, r, r.parts[1]).hasLabel('PFSTWO'), 'new label on disk');
    });
    await this.step('Filesystems: Copy from disk imports the embedded handler', async () => {
      const label = await e.gstr('g_unittext', (await e.g32('g_cur_unitidx')) * 24, 24);
      const srcUnit = Number(label.match(/Unit (\d+)/)[1]);
      await this.pickUnit(' 16 MB');
      await this.clickMain(GID.FS);
      const w = await e.waitWin('Filesystems');
      const before = await e.g32('g_fs_npool');
      await e.click(w, FSD.COPY);
      const p = await e.waitWin('Copy FS from disk');
      // The picker starts at entry 0 of each cycle, so count clicks to the target.
      for (let i = 0; i < 12; i++) {
        const name = await e.cstr(await e.u32(e.addr('plabels') + i * 4), 40);
        if (name === 'uaehf.device') break;
        await e.click(p, 1); await sleep(250);
      }
      for (let i = 0; i < srcUnit; i++) { await e.click(p, 2); await sleep(250); }
      await e.clickClose(p, 10);
      await e.waitFor('copied', async () => (await e.g32('g_fs_npool')) === before + 1, 15000);
      const row = await e.gstr('g_fsrows', before * 80);
      ok(row.includes('copied') && row.includes('PFS\\3'), `row: ${row}`);
      await e.click(w, FSD.REMOVE);
      await e.waitFor('removed', async () => (await e.g32('g_fs_npool')) === before);
      await this.answer('Filesystems', FSD.DONE);
      await this.refreshIfDirty();
    });
    await this.step('Partition table: scrolls when more partitions than rows', async () => {
      await this.clickMain(GID.SPLIT);
      const w = await e.waitWin('Split remaining');
      for (let i = 0; i < 4; i++) { await e.click(w, 3); await sleep(150); }   // 4 -> 8
      await e.clickClose(w, 10);
      eq(await this.nparts(), 8, 'partitions');
      const m = await this.main(), g = await e.mem(e.addr('g_plist'), 16);
      await e.clickAt((await e.i16(m + 4)) + g.readInt16BE(0) + g.readInt16BE(4) + 10, (await e.i16(m + 6)) + g.readInt16BE(2) - 13 + 28);
      await e.waitFor('table scrolled', async () => (await e.mem(e.addr('g_plist') + 12, 2)).readInt16BE(0) > 0);
      const top = (await e.mem(e.addr('g_plist') + 12, 2)).readInt16BE(0);
      await this.clickRow('g_plist', top + 1);
      await e.waitFor('scrolled row selected', async () => (await this.sel()) === top + 1);
      await this.idle();
      const edit = await e.gadget(await this.main(), GID.EDIT);
      ok(!(edit.flags & 0x100), 'Edit enabled once a row is selected');
      await e.shot(`${this.dir}/shots/scrolled.png`);
      await this.refreshIfDirty();
    });
    await this.step('Driver load: a file that is not a device is refused', async () => {
      const drv = await e.g32('g_drv_idx');
      await this.clickMain(GID.DRIVER);
      await this.pickFile('Select a device driver', 'FILES:pfs3aio');
      await this.answer('Driver', OK);
      eq(await e.g32('g_drv_idx'), drv, 'driver selection');
    });
    await this.step('Menu: Project/About opens from the menu bar', async () => {
      await this.activateMain();
      await e.pickMenu(0, 0);
      await e.shot(`${this.dir}/shots/about.png`);
      await this.answer('About HDPart', OK);
    });
    await this.step('Menu: right-Amiga shortcut opens Filesystems', async () => {
      await this.activateMain();
      await e.key(RAMIGA, 0x15);          // right-Amiga Y
      await this.answer('Filesystems', FSD.DONE);
      await this.refreshIfDirty();
    });
    await this.step('Keyboard: plain key opens Filesystems', async () => {
      await this.activateMain();
      await e.type('y');
      await this.answer('Filesystems', FSD.DONE);
      await this.refreshIfDirty();
    });

    await this.reboot();

    await this.step('Reboot: PFS volume auto-mounts from the embedded handler', async () => {
      const tasks = JSON.parse(await e.tool('winuae_tasks'));
      ok(tasks.some((t) => t.name === 'DH1'), `DH1 handler running; tasks: ${tasks.map((t) => t.name).join()}`);
      await e.go();
    });
    await this.step('Reboot: table and embedded filesystem are read back', async () => {
      await this.scanTo(' 8 MB');
      eq(await this.nparts(), 3, 'partitions');
      ok((await this.row(1)).includes('PFS\\3'), 'row shows PFS');
      eq(await e.g32('g_fs_npool'), 1, 'pool from RDB');
      await this.clickMain(GID.FS);                // the rows only exist while it is open
      const w = await e.waitWin('Filesystems');
      ok((await e.gstr('g_fsrows')).includes('embedded'), 'pool row source');
      await e.clickClose(w, FSD.DONE);
    });
    await this.step('Reboot: reformatting the auto-mounted volume', async () => {
      const text = await this.format(1, 'PFSBOOT');
      ok(/mounted as/.test(text), `confirm text: ${text}`);
      const r = readRdb(this.diskA);
      ok(partInfo(this.diskA, r, r.parts[1]).hasLabel('PFSBOOT'), 'label on disk');
    });
    await this.step('Quit: asks with unsaved edits, then exits cleanly', async () => {
      await this.selectPart(0);
      await this.clickMain(GID.DELETE);
      await this.answer('Delete Partition', PROCEED);
      await this.activateMain();
      await e.key(RAMIGA, 0x10);          // right-Amiga Q
      await this.answer('Quit HDPart', CANCEL);
      ok(await this.main(), 'still running after Cancel');
      await this.activateMain();
      await e.key(RAMIGA, 0x10);
      await this.answer('Quit HDPart', PROCEED);
      await e.waitFor('process exit', async () => {
        const s = JSON.parse(await e.tool('winuae_loaded_segments', { address: e.process }).catch(() => '{}'));
        return s.command !== 'HDPart';
      }, 20000);
    });
  }

  async boot() {
    const e = this.e;
    await this.step('Boot: HDPart opens a window on the Workbench screen', async () => {
      await e.locate();
      await e.waitFor('main window', async () => (await e.g32('g_win')) && (await e.g32('g_nunits')) === 0, 60000);
      await e.waitFor('picker ready', async () => (await this.status()).includes('Select a driver'), 30000);
      const w = await this.main();
      eq(await e.cstr(await e.u32(w + 32), 30), `HDPart ${ADFVER}`, 'window title');
      ok(await e.g32('g_pub'), 'on the Workbench public screen');
      const execVer = await e.u16((await e.u32(4)) + 20);
      eq(execVer, this.ks.major, 'exec version');
      if (this.ks.major < 39) eq(await e.g32('g_newlook'), 0, 'classic menus below KS3.0');
      await e.shot(`${this.dir}/shots/start.png`);
    });
    await this.step('Scan: finds both test disks', async () => {
      await this.scanTo(' 8 MB');
      eq(await this.nparts(), 0, 'blank disk has an empty table');
      eq(await this.dirty(), 0, 'dirty');
    });
  }

  async reboot() {
    const e = this.e;
    await this.step('Reboot: machine restarts with the written disks', async () => {
      await e.stop();
      await e.start();
      await e.locate();
      await e.waitFor('picker ready', async () => (await this.status()).includes('Select a driver'), 60000);
    });
  }

  async refreshIfDirty() {
    if (await this.dirty()) {
      await this.clickMain(GID.REFRESH);
      await this.answer('Refresh', PROCEED);
      await sleep(1000);
    }
  }
}

async function main() {
  const wanted = process.argv.slice(2).length ? process.argv.slice(2) : Object.keys(KICKSTARTS);
  for (const f of [ELF, HANDLER, ...wanted.map((k) => KICKSTARTS[k].rom)])
    if (!existsSync(f)) { console.error(`Missing ${f}. See tests/target/README.md.`); process.exit(2); }
  const summary = [];
  for (const key of wanted) {
    const s = new Suite(key, KICKSTARTS[key]);
    s.prepare();
    try { await s.run(); } catch (err) { if (!s.results.some((r) => !r.pass)) s.results.push({ name: 'harness', pass: false, err: err.message }); }
    finally { await s.e.stop(); }
    summary.push(s);
  }
  console.log('\n=== Summary ===');
  let bad = 0;
  for (const s of summary) {
    const pass = s.results.filter((r) => r.pass).length, fail = s.results.filter((r) => !r.pass);
    bad += fail.length;
    console.log(`${s.ks.label.padEnd(8)} ${pass} passed, ${fail.length} failed${fail.length ? `: ${fail[0].name}` : ''}`);
  }
  process.exit(bad ? 1 : 0);
}
main().catch((e) => { console.error(e); process.exit(2); });

# de_MaxTransfer, de_Mask, de_NumBuffers, de_BufMemType: evidence from the ROMs and filesystems

Date: 2026-11-02. All addresses are absolute ROM addresses (KS maps at $F80000) or hunk offsets
(third-party handlers, single code hunk relocated to 0).

Labels:

- **OBSERVED**: read in the disassembly.
- **INFERRED**: reasoned from the code, not executed.
- **EMULATED**: ran on WinUAE. This proves driver logic against WinUAE's drive model, not real drive behaviour.
- **UNVERIFIED**: not established by anything here.

## 1. Binaries examined

| Module | Version string | Location | ROM file |
|---|---|---|---|
| FFS | fs 37.26 | $F99112 | amiga-os-204.rom; A3000 2.04 ROM at $F9936A (first 4K of the module byte-identical) |
| FFS | fs 37.28 | $F9D7BE | A600 2.05 ROM (env parsing checked only) |
| FFS | fs 40.1 | $FA825E | amiga-os-310-a1200.rom; A4000 $F9E786 and A3000 $F9DCAE (same size, first 4K identical) |
| FFS | fs 47.4 | $FA6C60 | kicka1200.rom; A4000 $F9F9C4 and A3000 $F9EC00 (same size, first 4K identical) |
| scsi.device (IDE) | scsidisk 37.34 | $F86264 | A600 2.05 ROM |
| scsi.device (IDE) | scsidisk 40.12 | $FB5040 / $F87E34 | A1200 3.1 / A4000 3.1 |
| scsi.device (IDE) | IDE_scsidisk 47.4 | $FB51CC / $F85A5C | A1200 3.2 / A4000 3.2 |
| scsi.device (SCSI) | scsidisk 37.19 / 40.12 / 47.4 | $FB05A4 / $F87AD8 / $F85A6C | A3000 2.04 / 3.1 / 3.2 |
| PFS3 | 19.2 PFS3AIO-VERSION (2.10.2018) | hunk | amiga_hd/pfs3aio (pfs3aio-custom has the same logic, shifted) |
| SFS | SmartFilesystem 1.279 (23.12.2007) | hunk | amiga_hd/Smartfilesystem/AmigaOS3.x/L/SmartFilesystem |

- No ROM module was compressed or unreachable in the parts I needed.
- KS 2.04 A500+ has no scsi.device (OBSERVED: no RomTag). Its FFS is still relevant.
- Not analysed: carddisk.device (PCMCIA), A4000T ROM, IDEfix, Zorro controller ROMs, patched scsi.device 43/44.
- Variants of one module were compared on the first 4K only, plus spot checks of the relevant instructions.
  The "same" claims above are that strong and no stronger.

## 2. The central finding: what actually goes wrong above 256 sectors

**Nothing in the ROMs reads MaxTransfer except the filesystem.** scsi.device never sees it. It takes any
CMD_READ/CMD_WRITE length and splits it itself. The filesystem's MaxTransfer only stops the driver from
reaching the splitting path, and that path differs per ROM.

### scsi.device 37.34 and 40.12: 256-sector chunks, next address read back from the drive

Front end (40.12 A1200 $FB63CE-$FB63DE): chunk = min(remaining blocks, unit limit). The limit is
**65535** blocks (init $FB5382; 37.34 $F865AC). The chunk becomes a SCSI CDB, READ(6) if ≤256 blocks, else READ(10)
($FB6440-$FB6450).

IDE translator (40.12 A1200 $FB6DAE; same logic 37.34 $F87F26 and A4000 $F89BC4, A4000 registers at +2):

```
fb6db6: move.w d2,d0          ; d2 = sectors remaining
        andi.w #-256,d0       ; >= 256?  (low byte now 0)
        beq.s  last
        subi.w #256,d2
fb6dca: move.b d0,8(a4)       ; sector count register = 0, meaning 256
        move.b d3,28(a4)      ; issue ATA command
...
fb6de4: move.b 12(a4),d0      ; READ BACK the drive's sector number register
        addq.b #1,d0          ; next CHS = whatever the drive left there + 1
```

So:

- A 256-sector request is one ATA command with count register 0. No continuation. OBSERVED.
- 257 or more sectors takes the loop. **The next command's address comes from the task file left by the
  previous command.** No software counter and no check. OBSERVED.
- Addressing is CHS only (`divu` at $FB6E62, no LBA bit). OBSERVED.
- Drive selection reads word 47 of IDENTIFY and enables READ/WRITE MULTIPLE with up to 16 sectors ($FB71F0-$FB720C,
  commands $C4/$C5). OBSERVED, and EMULATED in the WinUAE log.

### scsi.device 47.4 (KS 3.2 IDE): fixed in the driver

- Unit limit is **255** blocks at init ($FB557E; A4000 $F85EA6). Each CMD_READ/CMD_WRITE chunk is a new
  CDB whose address is computed in software, so CMD_ transfers never use the continuation loop. OBSERVED.
- The translator's own chunk is 255, not 256 ($FB6D66; A4000 $F8765E), so the count register is never 0.
  Under LBA it continues from a software counter ($FB6D7C, $FB6DA2-$FB6DAC). OBSERVED.
- Only a direct HD_SCSICMD READ(10) of more than 255 sectors on a CHS-only drive still uses the read-back ($FB6DB2). OBSERVED.
- It also has LBA28 and LBA48 ($24/$34/$29/$39 opcodes at $FB6E22-$FB6E42). OBSERVED.

### Empirical confirmation (EMULATED)

- Probe: a small guest program doing plain CMD_READ/CMD_WRITE through ROM scsi.device on WinUAE's A1200 IDE, on a
  disk where every sector carries its own number. Drive configured with multiple mode 16.
- WinUAE has a level `ATA2+S`. In it the emulated drive does not update CHS/LBA after the final block
  (ide.cpp:1051-1054, whose comment says "ATA-2 spec says CHS/LBA does only need to be updated if error condition").
  That is an emulator author's reading of the spec and a third-party statement. I did not read the ATA standards.
  Default level updates the registers.

Read from sector 128 (`bad` = sectors with wrong content, all with io_Error=0 and full io_Actual):

| Sectors | KS 3.1 40.12, default drive | KS 3.1 40.12, ATA2+S | KS 3.2 47.4, default | KS 3.2 47.4, ATA2+S |
|---|---|---|---|---|
| 255 | ok | ok | ok | ok |
| 256 | ok | ok | ok | ok |
| 257 | ok | **bad=1**, index 256 holds sector 369, should be 384 | ok | ok |
| 512 | ok | **bad=256** | ok | ok |
| 1024 | ok | **bad=768** | ok | ok |

Write of 512 sectors to sector 1000 (hdf inspected on the host):

| | KS 3.1 default | KS 3.1 ATA2+S | KS 3.2 default | KS 3.2 ATA2+S |
|---|---|---|---|---|
| Result | correct | **err=0, actual=262144, 271 wrong sectors**: second chunk landed 15 sectors early, overwrote 15 good ones, last 15 never written | correct | correct |

369 = 368 + 1. 368 is the first sector of the last 16-sector block of the first chunk (128+240), the value left in the register
under that drive model. So the predicted failure is exact.

**Failure mode: silent wrong data with no error at all.** It is not "read/write errors" as the folklore doc says.
Writes corrupt neighbouring data and report success.

What this does not establish:

- Which real drives or CF cards leave the registers stale. UNVERIFIED. No real hardware was used.
- Whether a stale-register drive with multiple mode off is safe. INFERRED from WinUAE's model only: with one sector per
  block the register already points at the last sector. The driver enables multiple whenever word 47 offers it, so this
  matters little in practice.
- Any failure at exactly 256. None was reproduced, and PFS3-AiO deliberately allows 256 (section 5). A drive that mishandles a
  count of 0 would break at 256 but not 255; that is the only thing 0x1FE00 buys over 0x20000. UNVERIFIED that such a drive exists.

## 3. FFS

Reads (2.04 $F99E8C-$F99ECC; 3.1 $FAAAEE-$FAAB30; 3.2 $FA994A-$FA9956, $FA9D1A-$FA9D54). OBSERVED:

| Field | 2.04 / 2.05 | 3.1 | 3.2 |
|---|---|---|---|
| de_NumBuffers | ≤0 becomes 1 (signed) | <3 becomes 3 (unsigned) | <5 becomes 5 (unsigned) |
| de_BufMemType | TableSize<12 → 1, else raw | same | same |
| de_MaxTransfer | TableSize<14 → 0x7FFFFFFF, else raw | same | same, **top bit cleared** ($FA9D46) |
| de_Mask | TableSize<14 → 0x00FFFFFE, else raw, stored inverted | same | same |

### MaxTransfer

- Used only in the direct file-data path (read 2.04 $F9AAD0, write $F9AEF0; 3.1 $FA919C, $FA956C; 3.2 $FA7EA0, $FA827C):
  `d5 = min(len, MaxTransfer) >> blockshift`. The loop then merges contiguous data blocks while `--d5 > 0`. OBSERVED.
- Metadata and cached I/O is always one block ($F9A35E, $F9A850, $F9A8F4). OBSERVED.
- MaxTransfer below the block size, including 0: d5 is 0 and the loop exits with one block per request. It works, one block at a time. INFERRED.
- A value is rounded down to whole blocks by the shift. OBSERVED.
- Only contiguous runs produce long transfers, so a fragmented partition may hide a bad value for a long time. INFERRED.
- 3.2 only: if the Direct SCSI flag is set (bit 16 of de_Interleave, $FA992C), clamp to 0xFFFF << blockshift ($FA9E82-$FA9E92). OBSERVED.

**Bug: top bit set on FFS before 3.2** (2.04, 2.05, 3.1):

```
f9aad0: move.l d2,d5          ; requested length
        cmp.l  328(a5),d5     ; SIGNED compare with MaxTransfer
        ble.s  ok
        move.l 328(a5),d5     ; 0xFFFFFFFF is -1, so length > -1, so d5 = 0xFFFFFFFF
ok:     lsr.l  d0,d5          ; becomes 0x7FFFFF blocks, no longer bounded by the request
```

- The run loop then extends to the contiguous end of the file regardless of the requested length.
  The result is an overlong transfer into the caller's buffer, a memory overrun. On write it would take data from past the
  caller's buffer. INFERRED, **not run**.
- Same code on the write side ($F9AEF2) and in 3.1 ($FA919E `ble`). OBSERVED.
- 3.2 masks the top bit and uses unsigned compares ($FA9D46, $FA7EA6 `bls`). OBSERVED.
- So values ≥0x80000000 must be refused. PFS3 and SFS use unsigned compares here and are not affected. OBSERVED.

### Mask

- Test: `addr & ~mask` on the start address of the caller's buffer only ($F9AAC6-$F9AACC, write $F9AEE4). OBSERVED.
- Failure is not an error. It falls to the path at $F9AC12 (write $F9B080), which goes through the FFS buffer cache and CopyMem, one
  block at a time ($F9AC72-$F9AC7A). OBSERVED. Result: correct but slow. INFERRED.
- A buffer starting in range and ending out of range passes. OBSERVED that only the start is tested.
- **FFS never checks its own buffers against Mask.** They come from AllocMem(BufMemType) and are handed straight to the device.
  OBSERVED ($F99E04, $F9A336). So a 24-bit mask does not protect metadata unless BufMemType also lands in reachable memory. INFERRED.
- Write with a null source (a2=0, used for extending) skips the test ($F9AEE6). OBSERVED.

### NumBuffers and BufMemType

- Bytes per buffer: 2.04/3.1 **48 + block size** (560 at 512), 3.2 **76 + block size** (588, 16-byte aligned, $FA9C66-$FA9C86). OBSERVED.
  Actual AllocMem cost rounds up to 8 bytes. INFERRED.
- Allocates NumBuffers+1 with BufMemType|MEMF_CLEAR ($F99F52, $F99E08). OBSERVED.
- No upper limit in code. Stops at the first failed AllocMem and keeps what it has; only zero buffers fails startup ($F99E1A, $F99F5C).
  So a huge value eats all memory of that type instead of failing. INFERRED. The count is a 16-bit `dbf` ($F99DFE), so only the low word counts. OBSERVED.
- BufMemType goes to AllocMem raw. 0 and 1 are both any memory, public or not. OBSERVED that it is unmodified.
- FFS on 2.04/3.1 only issues CMD_READ/CMD_WRITE; I found no TD64/NSD command there (grep only). 3.2 probes NSD/TD64 for partitions
  past 4 GiB ($FA9EEA-$FA9F38). OBSERVED.

## 4. scsi.device: DMA or PIO, and whether Mask matters

| Machine | Transfer | Evidence | Mask needed? |
|---|---|---|---|
| A600, A1200, A4000 IDE (all versions) | **PIO** word loop on the data register | $FB757A, $F8875A; no CachePreDMA call in the module (`jsr -762` count 0) | No |
| Same, odd address | Handled by a separate path combining words and writing bytes | `btst #0,d1` at $FB7572, $F88752, $F8A35C, $FB7D36 | No, odd is safe |
| A3000 SCSI (2.04, 3.1, 3.2) | **DMA** via SDMAC ($DD0000): CachePreDMA, then address written to 12(a4), ST_DMA at 19(a4) | 3.1 $F8A478, $F89DE0, $F89DF2; 2.04 $FB2E2E; 3.2 $F880EE | No for correctness |

A3000 detail, 3.1 $F8A48A-$F8A4AE: if CachePreDMA shortens the region, length is below 256 bytes, the address is odd, or (a flag at
121(a5) is set and) bit 1 of the address is set, the driver falls back to its own programmed I/O. OBSERVED. So misaligned buffers do not
reach the DMA engine. A3000 memory reachable by this DMA: not tested; INFERRED fully 32-bit because no address-range test exists in the path.

A3000 chunk limit: **4095** blocks per command on every version (2.04 $FB0994, 3.1 $F87E22, 3.2 $F85E16), split by the driver.
Lowered to 256 if READ(10) fails ($F8946E). OBSERVED. MaxTransfer is not needed for this driver.

## 5. PFS3-AiO 19.2 (amiga_hd/pfs3aio)

### MaxTransfer: the folklore claim is true, with limits

Own cap at 96(a2) set at $256C-$25A2:

```
256c: move.l #0x7FFFFFFE,96(a2)
2574: tst.w  178(a2)              ; device name == "scsi.device" ($A72-$A86)
      ...FindName(DeviceList,"scsi.device") ($2582-$258A)
2594: move.w 20(a0),d0            ; lib_Version
      addi.w #-36,d0 / cmpi.w #13,d0 ; version 36..49
25a2: move.l #131072,96(a2)       ; cap 0x20000
```

Transfer chunk = min(de_MaxTransfer, cap, remaining), rounded down to the sector size ($B158-$B176 TD32/TD64/NSD; $B000-$B024 Direct SCSI,
also clamped to 65535 sectors). OBSERVED.

- It **enforces its own limit** and does not ignore MaxTransfer: the smaller of the two wins.
- The cap is **0x20000 (256 sectors), not 0x1FE00**, consistent with section 2: 256 never triggers the continuation.
- It applies **only when the device name is exactly "scsi.device" and that device is version 36 to 49**. Any other driver
  name (for example a Buddha, FastATA or other third-party IDE driver) gets no protection and depends on de_MaxTransfer. OBSERVED.
- It does not switch commands to avoid the bug. Access mode at $B73C-$B87E: 1=TD32, 2=Direct SCSI, 3=TD64, 4=NSD.
  Partitions ending below 4 GiB use TD32 (plain CMD_READ/CMD_WRITE) unless the probe fails. Past that, NSD, then TD64, then Direct SCSI. `PDS` dostype
  or de_Interleave bit 16 forces Direct SCSI; bit 18 skips NSD. OBSERVED.
- **de_MaxTransfer below the sector size hangs it.** The chunk rounds to 0, DoIO of length 0 succeeds, the remaining count never drops
  and the loop at $B152-$B1FE repeats. Direct SCSI path ($B02A-$B0B2) likewise. INFERRED, not run.
- No de_TableSize check before reading fields 11-14 at these sites. OBSERVED.

### Mask

- Checked against buffers it allocates; on mismatch it shows the requester "Allocated memory doesn't match memorymask" and carries on
  ($E72-$E86 sector cache, $5E60-$5E72 AllocBufmem, $974E-$975E cache buffers). OBSERVED that the call is made and execution continues after it.
- Data path checks the start and the end of the block-aligned part of the caller's buffer (read $A1F0-$A206, write $A4BA-$A52E).
  On failure it allocates a temporary buffer of the whole transfer ($A2A0-$A2B6, through the BufMemType pool), transfers, then copies. Allocation failure is
  ERROR_NO_FREE_STORE to the caller ($A2BC). OBSERVED.
- Tighter than needed therefore means large temporary allocations and copying, and possible out-of-memory errors, not corruption. INFERRED.

### NumBuffers and BufMemType

- Parsing at $9684-$96A6. OBSERVED:
  - Exactly **30 becomes 150** (`moveq #105,d0; not.b d0`).
  - ≤9 becomes **10**.
  - >600 becomes **600**.
  - The result is **written back** to de_NumBuffers.
- Size per buffer: 32 + reserved block size, from the rootblock field at offset 64 ($96CE-$96D2, $98BC-$98CC). OBSERVED.
  That the field is 1024 on normal partitions: UNVERIFIED (not traced). At 1024: 1056 bytes each, about 155 KB for 150.
- Failure to allocate is a hard failure for the whole cache setup ($973A-$9742). OBSERVED.
- BufMemType is used raw for the memory pool ($9AE, CreatePool puddle 20480/threshold 16384), the sector cache ($E52, 32 sectors) and cache buffers ($971E, |MEMF_CLEAR). OBSERVED.

## 6. SFS 1.279

Worked from the binary. Sources/fs in the package has only headers and objects.

- Parsing at $104FA-$10550. OBSERVED:
  - TableSize ≥12: BufMemType & 0x50707 | 1.
  - TableSize ≥13: MaxTransfer >> blockshift, in blocks.
  - TableSize ≥14: Mask raw.
  - Defaults: 0x100000 blocks, mask 0xFFFFFFFF, memtype 1 ($18E-$19E).
  - MaxTransfer that shifts to 0 becomes 1 block.
- MaxTransfer: chunk = min(remaining, limit) on all transfers ($10ADC-$10AE2, $10864-$1086E). **No own cap and no scsi.device special case**;
  the string "scsi.device" is not in this binary. So SFS relies entirely on de_MaxTransfer. OBSERVED.
- Commands: CMD_READ/WRITE unless the partition passes 4 GiB, then NSD ($4000 query, $C000/$C001), else TD_READ64 24/25, else error ($10554-$1063A). OBSERVED.
  A Direct SCSI branch exists but no store to its flag at 29247(a4) was found ($10882, $10AF2); INFERRED unreachable via that flag, not proven.
- Mask: start address only ($10AA0-$10AAA). On failure it transfers via an internal cache buffer with CopyMem ($10820-$10864, $10998, $109E4). OBSERVED. No warning, no check of its own allocations. OBSERVED that none appears at the allocation sites.
- NumBuffers ($448 passes 44(env) to $E4EC): total below 32 becomes **32**. No upper cap seen. Each is **28 + block size** (540 at 512), AllocMem(BufMemType|CLEAR) ($E55A-$E568). A failed allocation
  backs out and returns error 103, which aborts startup ($E570-$E57A, $454). OBSERVED for the code path; the mount-failure outcome is INFERRED.
- SetCache read-ahead lines are a separate setting and use AllocVec(line+80, BufMemType) ($ED3C-$ED42). OBSERVED.

## 7. Comparison

| | FFS 2.04 / 2.05 / 3.1 | FFS 3.2 | PFS3-AiO 19.2 | SFS 1.279 |
|---|---|---|---|---|
| Honours MaxTransfer | Direct file data only; metadata is single-block | Same | All transfers | All transfers |
| Own limit on scsi.device | None | None (driver is safe) | 0x20000 if name=="scsi.device" and v36-49 | None |
| MaxTransfer < block size | One block at a time | One block at a time | **Hangs** (INFERRED) | One block at a time |
| MaxTransfer ≥ 0x80000000 | **Overrun** (INFERRED) | Top bit cleared | Fine (unsigned) | Fine (unsigned) |
| Mask tests | Start address | Start address | Start and end, plus own allocations | Start address |
| Mask failure | Block-by-block through cache | Same | Temp buffer of whole transfer, may return 103 | Via cache buffer |
| Warns if buffers are out of Mask | No | No | Requester | No |
| NumBuffers min / max | 2.04: 1, 3.1: 3 / RAM | 5 / RAM | 10 / 600; **30 becomes 150** | 32 / none seen |
| Bytes per buffer (512) | 560 | 588 | 32 + reserved block (1056 if 1024) | 540 |
| Too many buffers | Keeps what it got | Keeps what it got | Fails setup | Fails, startup error |
| BufMemType | Raw | Raw | Raw, also pool | & 0x50707 \| 1 |
| Commands | CMD only (below 3.2) | CMD, NSD/TD64 past 4G, optional Direct SCSI | CMD below 4G; NSD/TD64/Direct above | CMD below 4G; NSD/TD64 above |

## 8. Folklore doc, claim by claim

docs/reference/amiga-disk-size-maxtransfer-mask.md

| Claim | Verdict |
|---|---|
| 0x1FE00 "required on every partition with any FS" on onboard IDE | **Too broad.** Needed for FFS and SFS on KS 2.05 and 3.1 scsi.device. Not needed with PFS3-AiO on that device. Not needed by driver logic on KS 3.2 (47.4). Still harmless everywhere, so right as a default. |
| Higher values cause "read/write errors and corruption" | **Half wrong.** EMULATED: no error at all, silent wrong data and silent overwrites. Only on drives leaving stale registers; which real drives do that is UNVERIFIED. |
| Limit is exactly 255 sectors | **Not supported.** 256 sectors was fine in all runs, and PFS3-AiO caps at 256. The boundary is 256/257 by code. 255 is a safe margin, needed only if a drive mishandles count 0 (UNVERIFIED). |
| "No effect on speed" | **Mostly.** Each chunk is another command. Very small values go down to one block per request. Cost at 0x1FE00 not measured. |
| Cause is ATA-1 behaviour vs a change in ATA-2 | **Mechanism confirmed in the driver**: it depends on post-command task file contents. The spec history itself is UNVERIFIED (only a WinUAE comment supports it). |
| Not needed with PFS3-AiO | **True for the name "scsi.device" v36-49 only.** Not a general immunity. |
| Not needed with IDEfix, PCMCIA | Not checked. |
| SCSI uses 0xFFFFFF | A3000 driver splits at 4095 blocks itself, so no limit is needed there. Third-party cards not checked. |
| Mask only matters for DMA | **Confirmed for ROM IDE** (PIO, odd address handled). A3000 DMA driver also self-protects. |
| A1200/A4000 IDE mask 0xFFFFFFFC | **No basis in the ROMs, and slightly harmful.** Driver handles any alignment. On FFS it forces word-aligned buffers through block-by-block copying. |
| Mask/MaxTransfer are "emergency workarounds" | **Misleading.** Mask is a routing choice between direct and copied I/O, and does not cover FFS's own buffers. |
| scsi.device table: "39.x Kickstart 3.1" | **Wrong version**: the 3.1 ROMs hold 40.12. KS 3.2's 47.4 (LBA48, 255 chunks) is missing from the table. |
| "CHS only" for original scsi.device | **Confirmed** for 37.34 and 40.12. |
| Capacity, FS size and other tables | Out of scope, not checked. |

## 9. HDPart's current state

| Where | Issue |
|---|---|
| src/rdb.h:72 `RDB_DEFAULT_MAXTRANSFER 0x7FFFFFFF`, used for new partitions (rdb.c:199, 287, 380) | New partitions get "No limit", while the reference doc and the "IDE ... required!" help say 0x1FE00. On KS 2.05/3.1 IDE with FFS or SFS that is the unsafe case. Split it: 0x1FE00 for new partitions, keep the FFS default only for parsing short tables. |
| kMaxTHelp[0] "required!" | Overstated; see section 8. |
| Custom MaxTransfer field | Accepts ≥0x80000000 (overrun on FFS before 3.2) and below block size (hang on PFS3). Should restrict to block size .. 0x7FFFFFFF. |
| kMaskValues "A1200/A4000" 0xFFFFFFFC | No ROM basis; see section 8. Remove or relabel. |
| kMaskValues "Zorro II DMA" 0x00FFFFFF | FFS's own default is 0x00FFFFFE. The current preset permits odd addresses to a DMA controller. Whether a given card copes is not checked, so the conservative value is safer. |
| rdb.c:551 `env_put(blk, DE_BufMemType, 0)`; no field in the partition struct | Saving **discards any existing BufMemType**. 0 is right for IDE and 32-bit DMA, but breaks a deliberate setup on a Zorro II card in a 32-bit-RAM machine. Should be preserved. |
| num_buffers default 30 | Fine, but it means 150 buffers (about 155 KB, if 1024-byte reserved blocks) on PFS3, and 32 on SFS. Note 31 gives PFS3 far fewer than 30 does. |
| Short tables: rdb.c:910 uses 0x7FFFFFFE | FFS treats a short table as 0x00FFFFFE. HDPart shows a value the filesystem would not use. Cosmetic. |

## 10. Recommended presets (for review)

**MaxTransfer**

| Label | Value | Guidance |
|---|---|---|
| IDE safe (default) | 0x1FE00 | Onboard IDE on KS below 3.2 with FFS or SFS. Safe everywhere. |
| SCSI | 0xFFFFFF | SCSI controllers. A3000 splits long transfers itself. |
| No limit | 0x7FFFFFFF | Only if the driver handles any length, such as KS 3.2 IDE. |
| Custom | | Block size to 0x7FFFFFFF. Higher corrupts memory on FFS before 3.2. |

**Mask**

| Label | Value | Guidance |
|---|---|---|
| Default | 0x7FFFFFFE | IDE, A3000, 32-bit DMA. Not needed by IDE; harmless. |
| Zorro II DMA | 0x00FFFFFE | 24-bit DMA cards. Set Buffer memory so buffers are in reach too. |
| No mask | 0xFFFFFFFF | IDE and A3000 only; odd buffers go straight to the driver. |
| Long-aligned | 0xFFFFFFFC | Only if the controller's documentation asks. Slower on FFS. |
| Custom | | Addresses outside the mask are copied, not refused. |

**NumBuffers**

| Label | Value | Guidance |
|---|---|---|
| Default | 30 | FFS about 17 KB. PFS3 treats 30 as 150 (about 155 KB). SFS uses 32. |
| Low memory | 10 | FFS about 6 KB. PFS3's minimum (about 11 KB). SFS still uses 32. |
| More cache | 100 | FFS about 56 KB, PFS3 about 106 KB, SFS about 54 KB. |
| Custom | | PFS3 clamps to 10-600. FFS takes what memory allows. |

Memory figures assume 512-byte blocks and 1024-byte PFS3 reserved blocks. **No performance measurement was made**, so no value is claimed to be
the fastest. I found nothing in the code that says what is optimal.

**BufMemType**

| Label | Value | Guidance |
|---|---|---|
| Any (default) | 0 | IDE and 32-bit DMA. |
| Chip | 3 | Zorro II DMA with RAM above 16 MB, when 24-bit fast RAM cannot be selected. Slower. |
| Custom | | Passed to AllocMem as is (SFS masks some bits). |

Chip being reachable by every Zorro II DMA card, and whether MEMF_24BITDMA (0x200) is usable because expansion.library tags memory with it: UNVERIFIED. I did
not establish the second from the ROMs.

## 11. Not done

- FFS end to end on emulated IDE with a bad MaxTransfer. The probe calls the device directly; that FFS issues such transfers is from the code.
- The FFS overrun and PFS3 hang were not executed.
- No real hardware. No ATA standard was read.
- pfs3aio-custom and 2.05 FFS: spot checks only.

Probe source and harness: scratchpad/w/exp (t.s, run.mjs). Ran on port 2346; the repo was not modified.

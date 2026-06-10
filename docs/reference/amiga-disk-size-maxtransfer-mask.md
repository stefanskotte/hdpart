# Amiga disk sizes, MaxTransfer & Mask — reference

Captured 2026-06-10 from the EAB "CF / SD and large drives FAQ"
(<https://eab.abime.net/showthread.php?t=61666>), research credited to
Toni Wilen, Thomas Rapp and others. Reproduced here for HDPart development
(MaxTransfer/Mask editor defaults & help text).

## TL;DR for HDPart's audience (A600/1200/4000 onboard IDE + scsi.device)

- **MaxTransfer = 0x1FE00** (130560 bytes, ~127.5 KiB) is **required** on **every
  partition** with **any** file system when using the onboard IDE controller and
  the original `scsi.device`. Higher values cause read/write errors and **data
  corruption** (and fixing the value later does not recover already-corrupted
  data). It limits the *size of one transfer*, **not** speed.
- **Wrong (too-high) MaxTransfer values are repeated all over EAB** — `0x1FE00`
  is the correct one for onboard IDE.
- MaxTransfer limit is **not** needed when using **IDEfix97** (on internal IDE),
  **PFS3 All-in-One** (has a workaround), or **PCMCIA**.
- **Mask** is only for **DMA-capable controllers**; it is **not required** for
  Amiga onboard IDE or PCMCIA. Leave the default unless on a DMA SCSI/Zorro card.

## MaxTransfer

> "First: The max transfer setting limits the max size of one file transfer. It
> has no effect on speed."

- Onboard IDE + `scsi.device`: **`0x1FE00` or lower, on all partitions, any FS.**
  Too-high values → read/write errors and corruption, because of `scsi.device`'s
  outdated **ATA-1** behaviour (incompatible with newer ATA standards). The magic
  `0x1FE00` circumvents an incompatibility between old ATA-1 Amiga drivers and
  ATA-2 drives caused by a change in the ATA spec.
- **SCSI** controllers commonly use **`0xFFFFFF`** (~16 MiB).
- Not required with **IDEfix97** (internal IDE), **PFS3 All-in-One**, or **PCMCIA**.
- Likely affected: third-party IDE drivers made **before ~1994** (before EIDE /
  ATA-2 1996). PFS3-AiO and IDEfix include workarounds.

## Mask

- The Mask is a DMA address mask (which memory addresses the controller may DMA
  to/from). Per the FAQ: **"Mask settings are for DMA capable controllers and not
  required for Amiga IDE or PCMCIA use."**
- Common base values (from EAB/AmiBay discussion, controller-dependent):
  - Zorro II DMA controller: `0xFFFFFF`
  - Zorro III / A3000 / A4000T onboard SCSI: `0xFFFFFFFF`
  - No DMA: `0xFFFFFFFF`
  - A1200 / A4000 internal IDE: `0xFFFFFFFC` (no DMA; mask largely irrelevant)
  - Generic safe default seen widely: `0xFFFFFFFE` / `0x7FFFFFFE`
- Mask & MaxTransfer are **emergency workarounds for driver bugs** — they exist so
  you can at least copy data off / keep using the disk until a fixed driver is
  available.

## Maximum drive-capacity limits

| Limit | Applies to | Reason | Source |
|------|------------|--------|--------|
| **4 GiB** | Any IDE/SCSI drive or CF/SD **without a 64-bit file system** | Max with 32-bit block addressing (`CMD_READ`/`CMD_WRITE` use 32-bit addresses) | Commodore FFS + parts of Workbench |
| **7.87 GiB** | A600/1200/4000 IDE, modern FS but **original** `scsi.device` | CHS artificially capped at 16383/16/63 by ATA spec (CHS obsolete in ATA-6, 2002) | `scsi.device` (CHS only) |
| **128 GiB** | A600/1200/4000 IDE, modern FS + many "modern" `scsi.device` (OS 3.5/3.9) | 28-bit LBA max; 48-bit LBA (ATA-6) needed beyond | `scsi.device` (only v43.45 & v44.2 do 48-bit LBA) |

**Consequence of exceeding 4 GiB without a 64-bit FS:** writes above 4 GiB **wrap
around and destroy data and the RDB**.

### Addressing modes (how to get past 4 GiB)
- All `.device` drivers' `CMD_READ`/`CMD_WRITE` are **32-bit → 4 GiB**.
- **Direct SCSI** (`HD_SCSICMD`) uses block addresses → **2 TiB** (512× more).
- **TD64** (`TD_READ64`/`TD_WRITE64`) → 64-bit → ~16 EiB. **NSD**
  (`NSCMD_TD_READ64`) is functionally identical but incompatible; both need
  driver **and** file-system support.
- Boot partition must be entirely accessible **before** any `scsi.device` patch
  (first 4 GiB w/o patch, 7.87 GiB with directSCSI FS, etc.).
- Keep the **system partition < 2 GiB** — many old installers do 32-bit free-space
  math and fail otherwise.

### Recommendations
- **IDE 4 GB card:** original `scsi.device` + FFS works, but a modern FS (SFS/PFS3)
  is faster/safer (no endless validation).
- **IDE 8 GB card** (< 7.87 GiB): original `scsi.device` + a directSCSI-capable FS
  (PFS3-AiO recommended; PFS3-DS or SFS 1.84 also work).
- **IDE > 8 GiB:** modern FS **and** a patched `scsi.device` (v43.45 recommended;
  v44.2 faster). IDEfix supports up to 128 GiB but dislikes removable CF cards.
- **SCSI:** 4 GiB limit applies; 7.87/128 GiB do **not**. Most SCSI controllers do
  directSCSI, so with PFS3-AiO the limit is 2 TiB; TD64 drivers (e.g. Cyberstorm)
  → up to 16 EiB. Old GVP ROM v1.x/2.x and C= A590/2091 ROM v1–5 cap at 1 GiB (no
  directSCSI).

### CHS drive-size gotcha
If a drive set up on an LBA-capable system has its last partition extended to the
very end, a CHS-only system can't address the last blocks (CHS size is rounded to
multiples of heads × sectors-per-track; LBA is exact). Symptom: read/write errors
or the last partition won't mount. **Fix:** leave **≥ 2 MiB unused at the end** (or
reduce the RDB size definition by ≥ 2 MiB), or patch `scsi.device`. (2 MiB is the
theoretical worst-case CHS/LBA discrepancy; 504 KiB for ATA-compliant 16×63×512.)

### FFS "$FFFFFFFF first block" bug
If the first 4 bytes of a partition's first block are `$FFFFFFFF`, FFS confuses it
with `ID_NO_DISK_PRESENT` (-1) and reports "no disk in drive". Can happen on
factory-fresh cards with random data. Fix: wipe the drive / zero the block.

## Driver / file-system compatibility table

Transcribed from the companion image in the thread (columns: **Rem\*** = supports
removable-type ATA devices; **atapi**; **spd\*\*** = Gayle-IDE speed boost; **2chan**;
**NSD**; **TD64**; **dSCSI** = direct SCSI).

### `scsi.device` — originals (in ROM/OS)

| Version | Ships with | Rem | atapi | spd | 2chan | NSD | TD64 | dSCSI | Max size |
|--------|-----------|-----|-------|-----|-------|-----|------|-------|----------|
| 37.x | Kickstart 2.05 | yes | no | no | no | no | no | yes\*\*\* | 4 GiB; 7.87/128 GiB with directSCSI |
| 39.x | Kickstart 3.1 | yes | no | no | no | no | no | yes\*\*\* | 4 GiB; 7.87/128 GiB with directSCSI |
| 43.34 | OS 3.5 | yes | yes | yes | no | yes | no | yes | 128 GiB |
| 43.35 | OS 3.9 | yes | yes | yes | no | yes | no | yes | 128 GiB |
| 43.43 | OS 3.9 BB2 | yes | yes | yes | no | yes | no | yes | 128 GiB, **but 4 GiB with LBA48 drives** (buggy) |

### `scsi.device` — patches (load via LoadModule)

| Version | Author | Rem | atapi | spd | 2chan | NSD | TD64 | dSCSI | Notes |
|--------|--------|-----|-------|-----|-------|-----|------|-------|-------|
| 43.24 | Heinz Wrobel | yes | no | yes | no | yes | no | yes | Max 128 GiB |
| 43.45 | Hodges, Wilen, Sauer | yes | yes | yes | no | yes | no | yes | **> 128 GiB, recommended** |
| 44.2 | Doobrey | yes | yes | yes | yes | yes | no | yes | > 128 GiB + speed |
| 43.46-? | Cosmos | yes | yes | yes | no | yes | no | yes | experimental |
| 43.46-? | Don Adan | yes | ? | yes | no | yes | no | ? | experimental |

### Third-party IDE / device drivers

| Driver | Ver | Maker | Rem | atapi | spd | 2chan | NSD | TD64 | dSCSI | Notes |
|--------|-----|-------|-----|-------|-----|-------|-----|------|-------|-------|
| IDEfix | 116+ | Elaborate Bytes | no | yes | yes | n/a | yes | yes | yes | use Doobrey's CF-patch |
| Individual C. | | Buddha, X-Surf, … | no | yes | yes | n/a | yes | yes | yes | use Doobrey's CF-patch |
| Elbox | 50/60 | FastATA / 4×EIDE'99 | yes | yes | yes | n/a | yes | yes | yes | > 128 GiB (LBA48), SPLIT mode |
| CFD | 1.32 | compactflash.device | yes | n/a | yes | n/a | no | no | no | A600/1200 PCMCIA |

### File systems

| FS | Ver | Notes | Max partition | Max file | Name len | NSD | TD64 | dSCSI | Requirements |
|----|-----|-------|---------------|----------|----------|-----|------|-------|--------------|
| FFS | 34–40 | originals up to KS3.1 | 2 GiB | 2 G | 32 | no | no | no | 68000+, KS1.3/2.04 |
| FFS | 43/44 | unofficial | 128 GiB? | 2 G | 32 | — | — | — | 68020+, KS2.04+ ? |
| FFS | 45 | OS 3.5/3.9 | 2 TiB | 2 G | 32 | yes | yes | yes | 68000+, KS2.04+ |
| FFS | 50 | OS 4.0 | 2 TiB? | 2 G | 107 | yes | yes | yes | PPC, OS4 |
| SFS | 1.084 | Smart File System | 128 GiB? | 2 G | 107 | no | no | yes | 68020+, KS2.04+ |
| SFS | 1.279 | Smart File System | 128 GiB | 4 G | 107 | yes | no | yes | 68020+, KS2.04+ |
| SFS | 1.279 | dostype `SFS\2` | 1 TiB+ | 2 T | 107 | yes | no | yes | 68020+, KS2.04+ |
| PFS3 | 18.5 | Professional FS | 101 GiB | 2 G | 107 | no | no | yes | 68020+, KS2.04+ \*\*\*\* |
| PFS3 | AiO | All-In-One (T. Wilen) | 101 GiB | 2 G | 107 | yes | no | yes | **68000+, KS1.3+, recommended** |
| FAT95 | 3.18 | Torsten Jäger | 2 TiB (Fat32) | 4 G? | 104 | no | yes | no | 68000+, KS1.3+ |

Footnotes:
- **\*** supports removable-type ATA devices (many CF cards & SD-IDE adapters).
- **\*\*** speed increase for Gayle IDE (based on Piru's SpeedyIDE).
- **\*\*\*** supports up to 7.87 GiB on all drives, but 128 GiB when given "illegal"
  CHS values equalling the full drive size (IDE-SD adapters).
- **\*\*\*\*** PFS3 works on 68000, but v5.3 has no directSCSI version for 68000;
  upgradable from v5.1.

## Notes relevant to HDPart
- HDPart targets exactly the at-risk case (A600/1200/4000 onboard IDE, CF/SD on
  IDE), so **`0x1FE00` should be the default/recommended MaxTransfer** it offers.
- Mask is DMA-only; for HDPart's IDE audience the default (`0x7FFFFFFE`/
  `0xFFFFFFFE`) is fine and rarely needs changing.
- These are RDB DOSEnvVec fields (`de_MaxTransfer`, `de_Mask`) HDPart already
  reads/writes per partition (see `src/rdb.c`).

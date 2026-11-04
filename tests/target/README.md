# On-target test suite

Runs HDPart's real binary in [WinUAE](https://www.winuae.net/) on **Kickstart 2.04,
3.1 and 3.2**, clicking through the GUI like a user and checking the results in guest
memory and in the hardfile from the host. Run it before every release, and extend it
with every feature: the host tests only cover the pure logic, and several bugs here
have only ever shown up on one Kickstart.

```sh
make target-test             # all three Kickstarts
make target-test KS=ks31     # one (ks204, ks31, ks32)
```

Exits non-zero if anything fails. Each Kickstart stops at its first failure, because
later steps build on earlier ones. Logs, disks and screenshots go to
`out/target-test/<ks>/`.

## Requirements

Nothing below is in this repo. ROMs and handlers are not ours to distribute.

| What | Default | Override |
|---|---|---|
| WinUAE with the GDB server (6.1+ or a Unix port) | `~/Devel/winuae-logical/build/winuae` | `WINUAE_PATH` |
| [mcp-winuae-emu](https://github.com/axewater/mcp-winuae-emu), built | `~/Devel/mcp-winuae-emu` | `WINUAE_MCP_DIR` |
| Kickstart 2.04 ROM | `amiga-os-204.rom` | `KS204_ROM` |
| Kickstart 3.1 A1200 ROM | `amiga-os-310-a1200.rom` | `KS31_ROM` |
| Kickstart 3.2 A1200 ROM | `kicka1200.rom` | `KS32_ROM` |
| ROM directory for those defaults | `~/Documents/FS-UAE/Kickstarts` | `KICKSTART_DIR` |
| PFS3 handler ([pfs3aio](https://aminet.net/package/disk/misc/pfs3aio)) | `amiga_hd/pfs3aio` | `HDPART_TEST_FS` |
| `m68k-amiga-elf-objdump` on `PATH` | Bartman toolchain | `AMIGA_OBJDUMP` |

Port 2345 must be free (`WINUAE_GDB_PORT`). An emulator window opens during the run.

## How it works

- `run.mjs` holds the Kickstart definitions and the test steps.
- `emu.mjs` drives the emulator. Window and gadget positions and program state come
  from guest memory via the ELF symbols, and the mouse is steered in a closed loop
  on `Screen->MouseX/Y`, so nothing depends on pixel positions.
- `rdbcheck.mjs` reads the written hardfile on the host and validates every checksum.

The disks start zero-filled, so nothing auto-mounts at boot and Format has to mount
partitions itself, as on real hardware. The second half reboots with the written disks
to check that they mount.

## Coverage

Boot on the Workbench screen, Scan, the Units list, New, Edit (name, filesystem), Delete,
Split, selecting from the disk map and table, table scrolling, Resize, Save (RDB validated on the host, embedded handler byte-exact, out-of-memory
injected at the allocation), Refresh, unsaved-edit prompts on Unit, Driver, Scan, Load
and Refresh, Filesystems (Add, refused file, Remove, assign, Copy from disk), Format
(fresh PFS, fresh FFS, reformat mounted, labels on disk), refused driver file, menu
pick, right-Amiga and plain-key shortcuts, auto-mount after reboot, Quit prompt.

Advanced settings too: MaxTransfer, Mask, Buffers and Buffer memory presets reach the
disk, their help buttons open, and an unsafe MaxTransfer is refused.

**Not covered yet:** the ASL file requester (the boots have no `asl.library`, so file
pickers use the typed path) and Load asl.library, loading a working `.device`, Bootable
and Boot Pri, Resize with Move Start, booting from a formatted partition, and real
hardware. `quick.mjs` below reads and writes files on formatted volumes on emulated IDE.

## Emulated IDE: `quick.mjs`

The suite above uses `uaehf.device`. Quick setup targets the internal IDE port, so `quick.mjs`
puts a card on it (`hardfile2=...,ide0`) and drives HDPart through `scsi.device`. The results and
what they decide are in `docs/reference/quick-setup-tests.md`.

```sh
make target-quick                          # everything whose ROM is set
make target-quick QUICK="used ks31a1200"   # one test on one machine
```

| Test | What it does | Default machines |
|---|---|---|
| `ide` | Boots each machine and checks `g_qs_is_ide`, the answer of `safety_is_internal_ide`. Also runs SetPatch and a LoadModule'd driver | see below |
| `fresh` | Blank card: Split, Save, Format, then `volcheck` 5 MB on each partition | all |
| `used` | Old table auto-mounted, rewritten and formatted without a restart, then checked, restarted and checked again. Reports safe or not | `ks31a1200`, `ks32` |
| `restart` | The same used card, restarted straight after Save and formatted only then | `ks31a1200`, `ks32` |
| `limit` | 4 GB card: fill a 3584 MB and then a 2048 MB partition so 64 MB lands against its end, check it on the host, restart, read back. Runs at `cpu_speed=max`, about 30 minutes per partition and up to 4 GB of disk. A600 ROMs write the filler on a 68020 with the same chipset, then go back to the 68000 | `ks31a1200`, `ks205a` |
| `offset` | The same check on a 2048 MB partition behind a 1760 MB one, so it ends near the top of the card | `ks31a1200`, `ks205a` |
| `run` | Quick setup's run and Finished screen: the table and volumes checked from the host and from inside, Back to HDPart, Restart with nothing loading on the way, a power cut at the reboot, a PC card, an 8 GB card, a card on unit 1 with Scan and Save after it, every change after the last check, unsaved edits, a failure at each step and the run after it, a volume named System elsewhere. `QUICK_CARD_ONLY=run,swap` picks scenarios. `synth` on `ks31a4000` is the host-scan proof on the wizard's own table: about 600 MB written at `cpu_speed=max`. `proof4g` on `ks31a1200` and `ks32`, or any key if named, is the 4 GiB proof on an 8 GB card: all zero from byte 4 GiB to the end | the first eight keys |
| `resume` | Quick setup's restart path and popup: a used card's first run, the Restart offer with nothing loading on the way, the real reboot, the popup, Continue and Cancel, a power cut at the offer, real format failures with the mount live (the answer to `ACTION_FORMAT` forced at `DoPkt`) and the run after each, the marker that can't be cleared, mounts that don't match the table, and the long list of cards that must not bring the popup, each with its hash checked. `QUICK_CARD_ONLY=used,np-blank` picks scenarios; the `np-` ones need a table made first, which the test does itself. On `ks31a1200` the list also runs on `a3000ks31` and `ks31softload` if those are set | the first eight keys |
| `card` | Quick setup's entry and card screen: the panel and menu item, blank, used, PC, 4 GB and 8 GB cards, and every refusal, each proving by hash that the card was not written. Needs `rdbtool` and `xdftool` (amitools) to make the used cards. `QUICK_CARD_ONLY=blank,used` picks scenarios, except on a refused machine, which always runs its one refusal. `synth` on `ks31a4000` proves the driver addresses by offset, with a file at the top of 4 GiB. Screenshots go to `out/target-quick/shots/quick-<key>-<scenario>.png` | the first eight keys, `ks314a4000`, `ks32a4000`, the three A3000 keys, `a500pks204`, `ks31softload`, `a3000softload`, `a3000roma4000`, `a3000roma1200` |

ROMs come from `QUICK_ROM_<KEY>` in upper case, for example `QUICK_ROM_KS31A1200=/path/to.rom`. A key
with no ROM set is skipped with a printed line. Keys: `ks205a`, `ks205b`, `ks31a600` (A600), `ks30`,
`ks31a1200`, `ks314`, `ks32` (A1200), `ks31a4000` (A4000). Paths with odd characters may not reach
the emulator, so copy those ROMs to a plain path first.

`ide` has more keys: `ks314a4000`, `ks32a4000` (A4000), `a3000ks204`, `a3000ks31`, `a3000ks32` (A3000), `a500pks204` (A500+), `a4000tks31`, `a4000tks32`
(A4000T), and `a3000roma4000`, `a3000roma1200` (an A3000 given that other machine's 3.1 ROM). `ks314setpatch` and `ks32setpatch` use the `ks314` and `ks32` ROMs, and need
`QUICK_SETPATCH_314` and `QUICK_SETPATCH_32`. `ks31softload` and `a3000softload` use the `ks31a1200` and `a3000ks31` ROMs, and need `QUICK_LOADMODULE` and `QUICK_MODULE_IDE` or
`QUICK_MODULE_A3000`. Those variables are paths to `C/SetPatch` and `C/LoadModule` and to `DEVS/A1200/scsi.device` (3.1.4) and `DEVS/A3000/scsi.device` (3.2), taken from the OS disks. A variable left unset skips that key with a printed line.

Named keys limit a test to the machines it can run on: `fresh`, `used`, `restart`, `limit`, `offset`, `run` and `resume` take the first eight keys only, and `node tests/target/quick.mjs a3000ks31` runs only `ide` and `card`.

`romsweep.c` checks the classifier on ROM files without an emulator: build it as its header says, then run `out/romsweep <dir>...`, or set `QUICK_ROM_DIRS=dir:dir`. It exits non-zero if an answer disagrees with the hardware base in the module.

A press that seems to go astray: `press.log`, beside each `emulator.log`, has a line per press with its target and the pointer's position.

Exits non-zero only when a test could not finish, or when `limit` or `offset` finish with a problem. "Not safe" from `used` or `restart` is a result, not a failure.
Logs, screenshots and `result.json` go to `out/target-quick/<key>/<test>/`.

`guest/volcheck.s` is the guest tool, built to `out/volcheck`. The boot runs it as `volcheck serve`
beside HDPart, and the harness sends it commands through low memory. It also works from a Shell:
`volcheck <volume> <megabytes> [file] [w|v|i]`. It writes a stamped file through dos.library, reads it
back, compares, and stores the result at `$100`; the layout is at the top of the source. The stamps
let the host find every file sector in the hardfile, which proves where the data really landed.

## Emulator logs

Every emulator log has a 20 MiB limit (`WINUAE_LOG_CAP`); a normal run logs far less. The harness stops the emulator at the limit and says so. For a one-off run by hand use `tests/target/winuae-capped.sh LOG winuae-arguments...`, and never send the emulator's output straight to a file. Don't use `ulimit -f`, because it would also limit the card files.

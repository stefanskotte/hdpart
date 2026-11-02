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
later steps build on earlier ones. Logs, disks and screenshots (`FAIL-*.png`,
`start.png`, `guards.png`, `about.png`) go to `out/target-test/<ks>/`.

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

Boot and own screen, Scan, New, Edit (name, filesystem), Delete, Split, the disk bar,
Resize, Save (RDB validated on the host, embedded handler byte-exact, out-of-memory
injected at the allocation), Refresh, unsaved-edit prompts on Unit, Driver, Scan, Load
and Refresh, Filesystems (Add, refused file, Remove, assign, Copy from disk), Format
(fresh PFS, fresh FFS, reformat mounted, labels on disk), refused driver file, menu
pick, right-Amiga and plain-key shortcuts, auto-mount after reboot, Quit prompt.

**Not covered yet:** the ASL file requester (the boots have no `asl.library`, so file
pickers use the typed path) and Load asl.library, loading a working `.device`, MaxTransfer,
Mask, Bootable and Boot Pri, Resize with Move Start, booting from a formatted partition,
reading and writing files on a formatted volume, and real hardware.

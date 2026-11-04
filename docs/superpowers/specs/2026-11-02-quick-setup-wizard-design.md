# Quick setup wizard for HDPart

Date: 2026-11-02. Status: approved by the user 2026-11-02, built.

## Intent

HDPart assumes you know what cylinders, MaxTransfer and bootable partitions are. A first-time Amiga owner does not, and
gets stuck before they can install Workbench.

Picture the user: an A600, A1200 or A4000 with only a floppy or Gotek drive. They downloaded HDPart to that media. A blank or
used CF or SD card sits on an adapter on the internal IDE port, typically a 4 to 8 GB CF card. They want the card ready to
install Workbench from disks, and they should not have to learn anything to get there.

Success: they click Quick setup, answer a couple of plain questions, and end on a screen telling them what to do next. They
install Workbench from their floppies, and the Amiga then starts from the card.

## Goals

- One guided path from a blank or used card to one that is ready for a Workbench install from disks.
- A bootable first partition, plus optional Work, Games and Demos partitions the user ticks.
- Sensible, safe values chosen for the user, with none of the technical settings on screen.
- Never endanger data on anything the user did not explicitly confirm.

## Non-goals

- Filesystems other than ROM FFS. PFS3 and SFS and anything past the size limits are the advanced route, through the normal UI.
- A500 (no IDE by default), A3000 and other SCSI machines, PCMCIA, and Zorro or third-party IDE controllers. These are experienced-user territory.
- Choosing between several drives. The target user has one card on the internal IDE port.
- Installing Workbench itself. HDPart stops at "ready".
- Custom sizes or more partition choices. More options can come later.

## Anti-goals: failure even if it technically works

- A popup at every start. Advanced users would hate it.
- Erasing anything without showing what is about to be erased, and confirming.
- Technical terms on the wizard's screens: no MaxTransfer, mask, cylinders, RDB or DosType.
- Writing past what the booted Kickstart can address, which risks wrapping onto the start of the disk.
- A silent or vague failure that leaves a beginner stuck. Each failure says what happened and what to do.

## The flow

The approved mockups are in `.superpowers/brainstorm/3891-1793654791/content/wizard-flow.html`, which is not committed. The user
reviewed them and approved them, screen 3 in particular.

1. **Entry.** A "Quick setup..." button in the empty panel of the main window at startup, shown until a driver is picked, plus
   a menu item. Never an automatic popup.
2. **Your card.** Scans `scsi.device`. Shows the drive's model name, its size, how much this setup will use, and what it holds now
   (partition names, or "Empty", or "Not an Amiga disk"). States that everything on the card will be erased. Continue or Cancel.
3. **Partitions.** Tick Work, Games, Demos. System is always there. A map in the Drive Bay style updates live, with one plain line
   explaining the result. Back, Continue, Cancel.
4. **Ready?** A table of device, volume, size. Names the card again and says this cannot be undone. Back, "Erase and set up", Cancel.
5. **Working.** A checklist of steps with progress: table written, table read back, each partition formatted.
6. **Finished.** Says the card is ready and gives numbered steps: swap to the Install disk, Restart, install to System (DH0:), remove the
   floppy and restart. Buttons: Restart, Back to HDPart.

## Kickstart versions

The wizard runs on anything from KS 2.05 up that passes the refusals below, with no version allow-list. Versions outside the test list are accepted as a risk, with the user's real-hardware check as the backstop.

## Refusals

The wizard stops at screen 2 with a plain explanation and changes nothing when:

- `scsi.device` is absent (for example an A500).
- The machine is not an internal-IDE one. If this cannot be established with certainty, refuse. Telling IDE from SCSI is the builder's
  to work out: the A3000 driver has the same device name, and before 3.1.4 the same ID string "scsidisk". Options to investigate
  include Gayle at $DA0000, the A4000 IDE registers at $DD2020, and SCSI INQUIRY data. The ROMs to check are in the local ROM collection.
- No card is found, or more than one drive is found.
- The card is the drive the Amiga booted from. `safety_classify` returns DEV_BOOT and fails safe when the boot device is unknown.

## Values chosen for the user

| Thing | Value | Who decided |
|---|---|---|
| Filesystem | FFS International, DOS\3, on every partition. In ROM from KS 2.0 and needs no files | builder's guess, shown to the user |
| First partition | Device DH0, volume "System", 500 MB, bootable | size and names are my guess, shown in the mockup and not objected to |
| Others | DH1 upward, in the order Work, Games, Demos, with those volume names | as above |
| Sharing | Space left after System, divided evenly between the ticked partitions | user |
| Whole-disk limit | Use at most the first 4 GiB, on every Kickstart | see below |
| Partition limit | 2 GiB each. Space beyond that stays unused and the screen says so | user agreed |
| MaxTransfer | 0x1FE00 | existing default, see `docs/reference/rom-findings-maxtransfer-mask-buffers.md` |
| Mask, buffers, buffer memory | HDPart's defaults: 0x7FFFFFFE, 30, any | existing defaults |

### Size limits

- **4 GiB:** ROM FFS through 40.1 issues only 32-bit CMD_READ/CMD_WRITE (read in the ROMs). The FAQ in
  `docs/reference/amiga-disk-size-maxtransfer-mask.md` says writes past 4 GiB wrap to the start of the disk.
  The last partition ends below the boundary, never on it: a transfer's 32-bit end offset would wrap to 0, so a cylinder size that divides 4 GiB exactly gives up one cylinder.
- **2 GiB:** this comes from the same FAQ, for FFS 34-40. It has not been verified, so verify it by test. If it is wrong the limit can rise,
  but it must stay at or below what is proven.
- **Small cards:** the 500 MB System needs a rule for small cards. Builder's call, keeping System usable and each ticked partition
  worthwhile, with a refusal below a sensible minimum. 4 to 8 GB is the normal case.

## Used Amiga cards: to be settled by test

A used Amiga card auto-mounts at boot even when starting from floppy, so old volumes are live while the wizard rewrites the table. From
reading the code this looks unsafe, but it has not been tested. The user wants this tested before deciding.

From reading `format_partition` (`src/format.c`): any cylinder overlap with a live mount is treated as "reformat the existing volume",
through the old handler, which still has the old bounds. So the restart path below is the expected outcome. Design it first, then test.

The first task of the build is a test on emulated IDE: a card with an old table of different layout, auto-mounted, then rewritten and formatted
with the existing engine. Keep the single-run flow only if that is proven safe. If it is not, or the result is unclear, use the restart path.

**Decision, from the test (2026-11-03): the single-run flow is not safe, so the wizard uses the restart path.** On KS 3.1 and KS 3.2, every Format reported
success while going through old handlers with the old bounds, two partitions were never formatted, a file landed outside its partition, and after the restart
all three volumes gave "Not a DOS disk". The restart path passed the same test on both. Partition limit: 2 GiB stands, proven on KS 3.1 and KS 2.05. A larger
partition also read back correctly, but that does not raise the limit. All of this is emulated. Details in `docs/reference/quick-setup-tests.md`.

### The restart path

1. The wizard writes the new table with a **setup-pending marker**, and the screen says to restart with this same disk.
2. On the next start, HDPart checks for the marker. If it is there, the wizard opens by itself on a short screen: "Finish setting up this card?
   System, Work, Games. Everything on them will be erased." Continue or Cancel.
   By the user's decision on 2026-11-04, the screen also says "This card already has files on it." when a partition's root block is valid and its directory has an entry, which changes only the warning, never whether the screen appears or what Continue and Cancel do.
3. Continue formats every partition and then clears the marker. If a format fails, the marker stays and the next run reformats them all, since
   nothing has been installed yet.
4. Cancel clears the marker, so the popup never comes back. The card is then treated as a used card if Quick setup runs again.

### The popup rule

The user accepted a popup here only if it is certain to appear in this one situation and no other. So:

- The only trigger is the marker. Never infer it from layout, names or an unformatted state.
- Only the wizard ever writes the marker, in a table it has just written itself to a card the user confirmed erasing. It must be something a table from
  another tool or from normal HDPart use cannot contain by accident. Finding the field is the builder's: it must survive a read and rewrite only where
  intended, and saving the table from the normal UI must clear it.
- The check runs only on the card the wizard would use, and only when the same conditions as the refusals list pass. No marker found, or any doubt, means
  no popup and the normal main window.
- A card without the marker, or one that does not match what the marker describes, gets the full flow and the full erase warning.

Test that the popup appears after the restart path, and does not appear on: blank cards, used cards from other tools, HDPart-made cards, finished wizard cards,
after Cancel, or on the machines in the refusals list.

## Failure handling

- Any failed step stops the run, names the step, and says what to do next.
- After the table is written, read it back from the card and compare it before formatting.
- After formatting, confirm each volume is usable.
- Running Quick setup again on a half-finished card must work. With the marker it resumes as above; without it, it is a used card.

## How it gets made

Existing C and GadTools, working on KS 2.05+ within the current window, font and pen rules. Reuse the existing engine: `rdb_*`, `format_partition`
(already non-interactive, takes the volume name), `safety_*`, discovery, the INQUIRY in `device.c`, and the disk map drawing. Put the layout
arithmetic somewhere host-testable, in the same way as `rdb_resize_cyls`.

Version: this is 0.12.

## Testing

The suite's disks use `uaehf.device`, so the wizard needs emulated IDE, on the machines it targets:

- Every IDE driver that can be booted from the collection: 37.34 and 37.55 (KS 2.05 A600), 37.64 (KS 3.0 A1200), 40.x (KS 3.1 A600, A1200, A4000), 45.7 (3.1.4), 47.4 (3.2). The probe in this work already booted most of these ROMs.
- Blank card, used Amiga card, PC-formatted card.
- 4 GB and 8 GB cards, including proof that nothing is written past 4 GiB. Sparse files should do.
- Each refusal, including an A3000 configuration.
- Host tests for the layout arithmetic.

Not yet covered by anything and required here:

- Booting from a partition, and reading and writing files on it.
- End to end: a wizard-made card, the real Install disk run on it in the emulator, then a boot from the card with no floppy. One run per OS version:
  2.05, 3.1, 3.1.4, 3.2. The 2.05 disks are the `Workbench v2.05*.zip` files in the repo root (ignored by git); the rest are in the local Amiga collection.
  All of them stay outside the public repo.

By the user's decision on 2026-11-04, the end-to-end Install disk runs were not done for 0.12, so installing Workbench on a wizard-made card and booting from it with no floppy are untested.

Real hardware is the user's check before release.

## Known and separate

The normal UI has the same 4 GiB exposure today: nothing stops a user partitioning past 4 GiB on KS 3.1. That is out of scope here and needs its own fix.

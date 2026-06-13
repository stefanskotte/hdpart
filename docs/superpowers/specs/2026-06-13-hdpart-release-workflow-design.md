# HDPart release workflow — GitHub Actions

**Date:** 2026-06-13
**Status:** Design approved (brainstorm)
**Scope:** one new file `.github/workflows/release.yml`. No source/Makefile changes.

## Goal

On pushing a version tag `release-x.x`, GitHub Actions builds HDPart with the
exact Bartman toolchain and publishes a GitHub Release containing the bootable
ADF and the hunk executable, both named for the version.

## Trigger & permissions

```yaml
on:
  push:
    tags: ['release-*']
permissions:
  contents: write        # create the Release + upload assets
```
Runner: `ubuntu-latest`. Single job `release`.

## Version derivation + guard

- `VERSION` = tag with the `release-` prefix stripped (`release-0.2` → `0.2`).
- **Guard (fail fast):** read `ADFVER` from the `Makefile` and compare to `VERSION`.
  If they differ, fail the job with a clear message. This prevents shipping a
  build whose embedded version (window title / ADFVER, kept in sync by hand per
  the Makefile comment) disagrees with the release tag.
  ```bash
  MK_VER=$(sed -n 's/^ADFVER = //p' Makefile | tr -d '[:space:]')
  [ "$MK_VER" = "$VERSION" ] || { echo "::error::tag release-$VERSION != Makefile ADFVER $MK_VER"; exit 1; }
  ```
  (Plain `sed` extraction — no new Makefile target required.)

## Toolchain provisioning (pinned Bartman .vsix)

Constant: extension `bartmanabyss.amiga-debug`, version **1.8.2** (gcc 15.1.0).

- `actions/cache` for the extracted toolchain, key `bartman-amiga-debug-1.8.2`,
  path e.g. `~/amiga-toolchain`.
- On cache miss, download + extract:
  ```bash
  curl -L --compressed -o ext.vsix \
    "https://marketplace.visualstudio.com/_apis/public/gallery/publishers/bartmanabyss/vsextensions/amiga-debug/1.8.2/vspackage"
  unzip -q ext.vsix -d ~/amiga-toolchain      # files land under extension/
  chmod -R +x ~/amiga-toolchain/extension/bin/linux   # ZIP loses the exec bit
  ```
- Add to `PATH` (mirrors the local `export`; `AMIGA_BIN = .../bin/linux`):
  ```bash
  BIN=~/amiga-toolchain/extension/bin/linux
  echo "$BIN/opt/bin" >> $GITHUB_PATH    # m68k-amiga-elf-gcc, -objdump
  echo "$BIN"         >> $GITHUB_PATH    # elf2hunk
  ```
- `pip install amitools` (provides `xdftool` for `make adf`).

## Build & gate

1. `./tests/run-host-tests.sh` — engine tests (system `cc`, fast). Fail the
   release if they fail.
2. `make` → `out/HDPart.exe` (the Makefile's entry-order guard runs via the
   toolchain `objdump`; must print `entry-order OK`).
3. `make adf ADFVER="$VERSION"` → `out/HDPart-$VERSION.adf`.

## Release artifacts

- Copy the hunk exe to a versioned name: `cp out/HDPart.exe out/HDPart-$VERSION.exe`.
- Publish with `softprops/action-gh-release@v2`:
  - `name: HDPart $VERSION`, created from the pushed tag, auto-generated notes.
  - `files:` `out/HDPart-$VERSION.adf` and `out/HDPart-$VERSION.exe`.

## Out of scope / notes

- Does NOT run `make hd` (FS-UAE staging is irrelevant to a release).
- Does NOT bump the version in `gui.c`/`Makefile` — that stays a manual pre-tag
  step; the guard enforces consistency rather than mutating sources.
- Toolchain availability depends on the marketplace keeping v1.8.2 downloadable;
  the cache mitigates per-run dependence.

## Testing approach

GitHub Actions can't run locally, so before relying on the workflow we validate
the mechanical steps with the **local** Linux-equivalent toolchain logic:
- Confirm the `.vsix` URL downloads and `unzip` yields `extension/bin/linux/...`.
- Confirm `make` + `make adf ADFVER=0.2` succeed with that toolchain on PATH and
  produce `out/HDPart-0.2.adf` + a hunk exe.
- Lint the YAML (valid workflow syntax).
After merge, the first real `release-x.x` tag is the end-to-end test.

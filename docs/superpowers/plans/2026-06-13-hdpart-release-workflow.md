# HDPart Release Workflow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a GitHub Actions workflow that, on a `release-x.x` tag, builds HDPart with the pinned Bartman toolchain and publishes a GitHub Release with the versioned ADF + hunk exe.

**Architecture:** Single workflow file `.github/workflows/release.yml`. It derives the version from the tag, guards it against the Makefile's `ADFVER`, fetches+caches the exact Bartman `amiga-debug` v1.8.2 toolchain from the VS Code marketplace `.vsix`, installs `amitools` for `xdftool`, runs the engine host-tests, builds `make` + `make adf`, and uploads two versioned assets via `softprops/action-gh-release`.

**Tech Stack:** GitHub Actions (ubuntu-latest), Bartman m68k-amiga-elf gcc 15.1.0 + elf2hunk (from .vsix), Python amitools (xdftool), softprops/action-gh-release@v2.

**Design spec:** `docs/superpowers/specs/2026-06-13-hdpart-release-workflow-design.md`

---

## File Structure

- **Create:** `.github/workflows/release.yml` — the entire deliverable. No source/Makefile changes.

## Testing reality

A workflow can't be unit-tested or run locally end-to-end. Verification is layered:
- **Task 1** validates the mechanically-risky, host-independent parts locally (the `.vsix` URL, the unzip layout, the exact paths the YAML hard-codes). The macOS box cannot run the Linux toolchain binaries, but `make`/`make adf` are already proven this session via the darwin build, and the Makefile resolves the SDK relative to `which m68k-amiga-elf-gcc` identically on either OS.
- **Task 2** validates YAML well-formedness.
- The **first real `release-x.x` tag push** is the end-to-end test (only place the Linux binaries actually execute). The plan ends with how to do that safely.

---

### Task 1: Validate the toolchain fetch + layout locally (de-risk before writing YAML)

**Files:** none (investigation only).

- [ ] **Step 1: Confirm the marketplace .vsix downloads and is a zip**

```bash
cd /tmp
curl -L --compressed -o bartman.vsix \
  "https://marketplace.visualstudio.com/_apis/public/gallery/publishers/bartmanabyss/vsextensions/amiga-debug/1.8.2/vspackage"
file bartman.vsix          # expect: Zip archive data
unzip -l bartman.vsix | grep -E "bin/linux/(elf2hunk|opt/bin/m68k-amiga-elf-gcc)$"
```
Expected: `file` reports a Zip archive; the grep shows BOTH `extension/bin/linux/elf2hunk` and `extension/bin/linux/opt/bin/m68k-amiga-elf-gcc` exist inside.

- [ ] **Step 2: Confirm the extracted layout matches the PATH the YAML will set**

```bash
cd /tmp && rm -rf amiga-tc && mkdir amiga-tc
unzip -q bartman.vsix -d amiga-tc
ls amiga-tc/extension/bin/linux/opt/bin/m68k-amiga-elf-gcc \
   amiga-tc/extension/bin/linux/opt/bin/m68k-amiga-elf-objdump \
   amiga-tc/extension/bin/linux/elf2hunk \
   -d amiga-tc/extension/bin/linux/opt/m68k-amiga-elf/sys-include
```
Expected: all four paths exist. (These are exactly `$BIN/opt/bin`, `$BIN` for elf2hunk, and the SDK dir the Makefile derives via `which`.) If the layout differs, STOP and adjust the spec/YAML paths to match what's actually in the archive.

- [ ] **Step 3: Confirm the build + ADF commands themselves work (darwin toolchain, already on this machine)**

```bash
cd /Users/sfs/Devel/party
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
./tests/run-host-tests.sh | tail -1
make >/dev/null && echo "make OK"
make adf ADFVER=0.2 2>&1 | tail -1
sed -n 's/^ADFVER = //p' Makefile | tr -d '[:space:]'   # expect: 0.2 (guard reference)
```
Expected: host tests pass, `make OK`, ADF built, `ADFVER` extraction prints `0.2`. This proves the build/guard logic the workflow runs (only the toolchain *origin* differs on CI).

- [ ] **Step 4: Record findings**

No commit (investigation). If Steps 1–2 revealed a different URL behavior or archive layout, note the corrected values for Task 2; otherwise proceed with the YAML exactly as written below.

---

### Task 2: Write `.github/workflows/release.yml`

**Files:**
- Create: `.github/workflows/release.yml`

- [ ] **Step 1: Create the workflow file**

```yaml
name: Release

on:
  push:
    tags:
      - 'release-*'

permissions:
  contents: write

jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Derive version from tag
        id: ver
        run: |
          VERSION="${GITHUB_REF_NAME#release-}"
          echo "version=$VERSION" >> "$GITHUB_OUTPUT"
          echo "Building HDPart release version: $VERSION"

      - name: Guard - tag version must match Makefile ADFVER
        run: |
          VERSION="${{ steps.ver.outputs.version }}"
          MK_VER=$(sed -n 's/^ADFVER = //p' Makefile | tr -d '[:space:]')
          if [ "$MK_VER" != "$VERSION" ]; then
            echo "::error::tag release-$VERSION does not match Makefile ADFVER=$MK_VER. Bump the gui.c window title + Makefile ADFVER to match the tag before releasing."
            exit 1
          fi
          echo "Version guard OK: $VERSION"

      - name: Cache Amiga toolchain
        id: toolcache
        uses: actions/cache@v4
        with:
          path: ~/amiga-toolchain
          key: bartman-amiga-debug-1.8.2

      - name: Download + extract Bartman toolchain (.vsix)
        if: steps.toolcache.outputs.cache-hit != 'true'
        run: |
          curl -L --compressed -o ext.vsix \
            "https://marketplace.visualstudio.com/_apis/public/gallery/publishers/bartmanabyss/vsextensions/amiga-debug/1.8.2/vspackage"
          file ext.vsix
          mkdir -p ~/amiga-toolchain
          unzip -q ext.vsix -d ~/amiga-toolchain
          chmod -R +x ~/amiga-toolchain/extension/bin/linux

      - name: Put toolchain on PATH
        run: |
          BIN="$HOME/amiga-toolchain/extension/bin/linux"
          echo "$BIN/opt/bin" >> "$GITHUB_PATH"
          echo "$BIN" >> "$GITHUB_PATH"

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: '3.x'

      - name: Install amitools (xdftool)
        run: pip install amitools

      - name: Verify tooling
        run: |
          m68k-amiga-elf-gcc --version | head -1
          command -v elf2hunk
          command -v xdftool

      - name: Engine host tests
        run: ./tests/run-host-tests.sh

      - name: Build executable
        run: make

      - name: Build ADF
        run: make adf ADFVER="${{ steps.ver.outputs.version }}"

      - name: Collect versioned artifacts
        run: |
          VERSION="${{ steps.ver.outputs.version }}"
          cp out/HDPart.exe "out/HDPart-$VERSION.exe"
          ls -la "out/HDPart-$VERSION.adf" "out/HDPart-$VERSION.exe"

      - name: Publish GitHub Release
        uses: softprops/action-gh-release@v2
        with:
          name: HDPart ${{ steps.ver.outputs.version }}
          generate_release_notes: true
          files: |
            out/HDPart-${{ steps.ver.outputs.version }}.adf
            out/HDPart-${{ steps.ver.outputs.version }}.exe
```

- [ ] **Step 2: Validate YAML well-formedness**

```bash
cd /Users/sfs/Devel/party
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/release.yml')); print('YAML OK')"
```
Expected: `YAML OK` (no parse error). If `yaml` module is missing, use `pip install pyyaml` first, or `ruby -ryaml -e 'YAML.load_file(".github/workflows/release.yml"); puts "YAML OK"'`.

- [ ] **Step 3: Commit + push**

```bash
cd /Users/sfs/Devel/party
git add .github/workflows/release.yml
git commit -m "ci: release workflow — build ADF+exe from release-* tags via pinned Bartman toolchain"
git push origin master
```

---

## Final verification

- [ ] **Step 1:** Confirm the workflow appears in the repo's Actions tab (GitHub parses it on push). `gh workflow list` should show "Release".
- [ ] **Step 2 (end-to-end, user-driven):** The real test is the first tag. Because `gui.c`/`ADFVER` are at `0.2`, the safe first release is:
  ```bash
  git tag release-0.2
  git push origin release-0.2
  ```
  Then watch `gh run watch` (or the Actions tab). Confirm: the guard passes, the toolchain downloads + caches, host tests pass, `make`/`make adf` succeed, and a "HDPart 0.2" release appears with `HDPart-0.2.adf` + `HDPart-0.2.exe` attached. Download the ADF and boot it in FS-UAE as the final proof.
- [ ] **Step 3:** If the Linux toolchain fails to execute on the runner (e.g. glibc/permission issue not visible from macOS), capture the failing step's log and iterate on the "Download + extract" / PATH steps — that is the one class of issue Task 1 could not catch locally.

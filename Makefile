# HDPart - AmigaOS application Makefile
# Supports Bartman's m68k-amiga-elf-gcc + elf2hunk toolchain as well as
# AmigaPorts m68k-amigaos-gcc (on PATH or under /opt/amiga*/bin).
# Builds a normal AmigaDOS hunk executable (NOT a bare-metal demo).

ifdef OS
	WINDOWS = 1
	SHELL = cmd.exe
endif

ifndef WINDOWS
ifdef AMIGA_TOOLCHAIN_BIN
export PATH := $(AMIGA_TOOLCHAIN_BIN):$(PATH)
DEFAULT_CC := $(AMIGA_TOOLCHAIN_BIN)/m68k-amigaos-gcc
else ifneq ($(shell command -v m68k-amiga-elf-gcc 2>/dev/null),)
DEFAULT_CC := m68k-amiga-elf-gcc
else ifneq ($(shell command -v m68k-amigaos-gcc 2>/dev/null),)
DEFAULT_CC := m68k-amigaos-gcc
else
OPT_AMIGA_CC := $(lastword $(sort $(wildcard /opt/amiga/bin/m68k-amigaos-gcc /opt/amiga-*/bin/m68k-amigaos-gcc)))
ifneq ($(OPT_AMIGA_CC),)
export PATH := $(patsubst %/,%,$(dir $(OPT_AMIGA_CC))):$(PATH)
DEFAULT_CC := $(OPT_AMIGA_CC)
else
DEFAULT_CC := m68k-amiga-elf-gcc
endif
endif
else
DEFAULT_CC := m68k-amiga-elf-gcc
endif

CC      = $(DEFAULT_CC)
TOOLCHAIN_PREFIX = $(patsubst %-gcc,%-,$(CC))
AS      = $(TOOLCHAIN_PREFIX)as
LD      = $(TOOLCHAIN_PREFIX)ld
OBJDUMP = $(TOOLCHAIN_PREFIX)objdump
STRIP   = $(TOOLCHAIN_PREFIX)strip

ifneq ($(findstring amigaos,$(CC)),)
AMIGAOS_GCC = 1
endif

# `program` is overridable by the VSCode build task (passes program=${config:amiga.program}).
# The Bartman amiga-debug extension runs <program>.exe and reads <program>.elf for symbols.
program ?= out/HDPart
OUT   = $(program)

# Only compile our own sources under src/. Place startup.c first so _start is
# at byte 0 of .text under both m68k-amigaos-ld and m68k-amiga-elf-ld.
c_sources := src/startup.c $(filter-out src/startup.c,$(sort $(wildcard src/*.c)))
c_objects := $(addprefix obj/,$(patsubst src/%.c,%.o,$(c_sources)))

# Ensure build output dirs exist on a fresh checkout (CI / new clone) so -MMD can
# write obj/*.d and the linker can write out/. Parse-time; Unix shells only.
ifndef WINDOWS
$(shell mkdir -p obj out)
endif

ifdef AMIGAOS_GCC
	SDKDIR = $(abspath $(dir $(shell command -v $(CC) 2>/dev/null))../m68k-amigaos/ndk-include)
else ifdef WINDOWS
	SDKDIR = $(abspath $(dir $(shell where $(CC)))..\m68k-amiga-elf\sys-include)
else
	SDKDIR = $(abspath $(dir $(shell which $(CC)))../m68k-amiga-elf/sys-include)
endif

# OS app: freestanding (no libc available) but a normal relocatable executable.
# No -Ttext=0 (that is for bare-metal). Provide our own _start entry.
CCFLAGS = -g -MP -MMD -m68000 -Os -nostdlib -ffreestanding -fomit-frame-pointer \
          -Wall -Wextra -Wno-unused-function \
          -Wno-pointer-sign -Wno-volatile-register-var \
          -ffunction-sections -fdata-sections -Isrc -DHDPART_AMIGA

ifdef AMIGAOS_GCC
LDFLAGS = -nostdlib -Wl,-e,_start,--gc-sections,-Map=$(OUT).map
else
LDFLAGS = -nostdlib -Wl,-e,_start,--emit-relocs,--gc-sections,-Map=$(OUT).map
endif

# AmigaDOS LoadSeg() ignores any entry symbol and runs the first byte of the
# code hunk. This guard fails the build if offset 0 of .text is not _start
# (see src/startup.c). Skipped on Windows (cmd.exe shell).
ifdef WINDOWS
ENTRY_CHECK = @echo (entry-order check skipped on Windows)
else ifdef AMIGAOS_GCC
ENTRY_CHECK = @addr=`$(OBJDUMP) -t $(OUT).debug | awk '$$NF == "_start" {print $$1; exit}'`; \
	if [ "$$addr" != "00000000" ]; then \
		echo "FATAL: hunk entry _start is at '$${addr:-missing}', expected 00000000 (startup not first in .text)"; \
		exit 1; \
	fi; \
	echo "entry-order OK: _start is at 00000000 in .text"
else
ENTRY_CHECK = @first=`$(OBJDUMP) -d $(OUT).elf | awk '/^[0-9a-f]+ </{print $$2; exit}'`; \
	if [ "$$first" != "<_start>:" ]; then \
		echo "FATAL: hunk entry is $$first, expected <_start>: (startup not first in .text)"; \
		exit 1; \
	fi; \
	echo "entry-order OK: _start is first in .text"
endif

all: $(OUT).exe

ifdef AMIGAOS_GCC
$(OUT).exe: $(OUT).debug
	$(info Stripping $@)
	@$(STRIP) -o $(OUT).exe $(OUT).debug
	$(ENTRY_CHECK)

$(OUT).debug: $(c_objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(c_objects) -o $@
else
$(OUT).exe: $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(OUT).exe
	$(ENTRY_CHECK)

$(OUT).elf: $(c_objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(c_objects) -o $@
endif

obj/%.o : src/%.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $(CURDIR)/$<

# --- FS-UAE test helpers (dev convenience, macOS/Linux) -----------------------
# `make hd`            stage the built binary into amiga_hd/ (mounted as HDPart:)
# `make install-fsuae` copy the HDPart launcher config into FS-UAE
FSUAE_CFG_DIR = $(HOME)/Documents/FS-UAE/Configurations

hd: $(OUT).exe
	@mkdir -p amiga_hd amiga_boot/s
	@cp $(OUT).exe amiga_hd/HDPart
	@cp tools/HDPart.info amiga_hd/HDPart.info
	@cp $(OUT).exe amiga_boot/HDPart
	@printf 'HDPart\n' > amiga_boot/s/startup-sequence
	$(info Staged -> amiga_hd/HDPart (+ HDPart.info icon) (volume HDPart:, run: HDPart:HDPart))
	$(info Staged -> amiga_boot/ (bootable dir; startup-sequence auto-runs HDPart for the no-Workbench own-screen test))

install-fsuae:
	@mkdir -p "$(FSUAE_CFG_DIR)"
	@cp tools/fsuae/*.fs-uae "$(FSUAE_CFG_DIR)/"
	$(info Installed FS-UAE configs -> $(FSUAE_CFG_DIR)/  (every tools/fsuae/*.fs-uae))

# `make adf`            build a shareable 880K OFS floppy with the program + icons
#                       (out/HDPart-$(ADFVER).adf). Primary use: mount under an
#                       existing Workbench and double-click HDPart; it is also
#                       bootable (auto-runs HDPart on the Workbench screen). Needs
#                       amitools' xdftool (pip3 install amitools).
# ADFVER: keep in sync with the window title in src/gui.c ("HDPart 0.13").
ADFVER = 0.13
ADF    = out/HDPart-$(ADFVER).adf

adf: $(OUT).exe tools/HDPart.info tools/Disk.info tools/system-configuration
	@command -v xdftool >/dev/null 2>&1 || { echo "ERROR: xdftool not found - install amitools (pip3 install amitools)"; exit 1; }
	@mkdir -p out
	@printf 'HDPart\n' > out/.adf-startup-sequence
	@# Without this file a floppy boot gets topaz 9 as the system font.
	@xdftool -f $(ADF) create + format "HDPart" ofs + boot install \
	    + makedir s \
	    + makedir devs \
	    + write $(OUT).exe HDPart \
	    + write out/.adf-startup-sequence s/startup-sequence \
	    + write tools/system-configuration devs/system-configuration \
	    + write tools/HDPart.info HDPart.info \
	    + write tools/Disk.info Disk.info
	@rm -f out/.adf-startup-sequence
	@# asl.library is NOT shipped in this repo (it is Commodore-copyrighted) and
	@# is not ROM-resident, so the stock floppy has no file requester: the pickers
	@# fall back to typed paths, and Project/Load asl.library... can bring in a
	@# requester from any disk that has one. If you drop your own copy at
	@# tools/asl.library (V37+, gitignored) it gets bundled into Libs/ here, and
	@# HDPart late-assigns LIBS: to PROGDIR:Libs to find it.
	@if [ -f tools/asl.library ]; then \
	    xdftool $(ADF) makedir Libs + write tools/asl.library Libs/asl.library ; \
	    echo "Built -> $(ADF)  (+ local Libs/asl.library)" ; \
	else \
	    echo "Built -> $(ADF)  (no asl.library; typed-path fallback + Load asl.library...)" ; \
	fi

# `make target-test`    run the full on-target suite in WinUAE on KS 2.04, 3.1 and 3.2.
#                       Needs WinUAE, mcp-winuae-emu and ROMs: see tests/target/README.md.
target-test: $(OUT).exe
	@node tests/target/run.mjs $(KS)

# `make target-quick`   the emulated-IDE tests behind Quick setup (see tests/target/README.md).
#                       QUICK picks what to run, e.g. QUICK="used ks31a1200"; default is everything.
VOLCHECK = out/volcheck

$(VOLCHECK): tests/target/guest/volcheck.s
	$(info Assembling $<)
	@$(AS) -m68000 -o obj/volcheck.o $<
ifdef AMIGAOS_GCC
	@$(LD) -e start -s -o $@ obj/volcheck.o
else
	@$(LD) -e start --emit-relocs -o out/volcheck.elf obj/volcheck.o
	@elf2hunk out/volcheck.elf $@
endif

target-quick: $(OUT).exe $(VOLCHECK)
	@node tests/target/quick.mjs $(QUICK)

clean:
	$(info Cleaning...)
ifdef WINDOWS
	@del /q obj\* out\*
else
	@$(RM) obj/* out/*
endif

-include $(c_objects:.o=.d)


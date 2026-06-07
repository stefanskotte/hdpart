# HDPart - AmigaOS application Makefile (Bartman m68k-amiga-elf toolchain)
# Builds a normal AmigaDOS hunk executable (NOT a bare-metal demo).

ifdef OS
	WINDOWS = 1
	SHELL = cmd.exe
endif

CC    = m68k-amiga-elf-gcc
# `program` is overridable by the VSCode build task (passes program=${config:amiga.program}).
# The Bartman amiga-debug extension runs <program>.exe and reads <program>.elf for symbols.
program ?= out/HDPart
OUT   = $(program)

# Only compile our own sources under src/.
c_sources := $(wildcard src/*.c)
c_objects := $(addprefix obj/,$(patsubst src/%.c,%.o,$(c_sources)))

ifdef WINDOWS
	SDKDIR = $(abspath $(dir $(shell where $(CC)))..\m68k-amiga-elf\sys-include)
else
	SDKDIR = $(abspath $(dir $(shell which $(CC)))../m68k-amiga-elf/sys-include)
endif

# OS app: freestanding (no libc available) but a normal relocatable executable.
# No -Ttext=0 (that is for bare-metal). Provide our own _start entry.
CCFLAGS = -g -MP -MMD -m68000 -Os -nostdlib -ffreestanding -fomit-frame-pointer \
          -Wall -Wextra -Wno-unused-function \
          -ffunction-sections -fdata-sections -Isrc
LDFLAGS = -nostdlib -Wl,-e,_start,--emit-relocs,--gc-sections,-Map=$(OUT).map

# elf2hunk ignores the ELF entry symbol and runs the first byte of the code
# hunk. This guard fails the build if the lowest-addressed function is not
# _start (see src/startup.c). Skipped on Windows (cmd.exe shell).
ifdef WINDOWS
ENTRY_CHECK = @echo (entry-order check skipped on Windows)
else
ENTRY_CHECK = @first=`m68k-amiga-elf-objdump -d $(OUT).elf | awk '/^[0-9a-f]+ </{print $$2; exit}'`; \
	if [ "$$first" != "<_start>:" ]; then \
		echo "FATAL: hunk entry is $$first, expected <_start>: (startup not first in .text)"; \
		exit 1; \
	fi; \
	echo "entry-order OK: _start is first in .text"
endif

all: $(OUT).exe

$(OUT).exe: $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(OUT).exe
	$(ENTRY_CHECK)

$(OUT).elf: $(c_objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(c_objects) -o $@

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
	@cp $(OUT).exe amiga_boot/HDPart
	@printf 'HDPart\n' > amiga_boot/s/startup-sequence
	$(info Staged -> amiga_hd/HDPart (volume HDPart:, run: HDPart:HDPart))
	$(info Staged -> amiga_boot/ (bootable dir; startup-sequence auto-runs HDPart for the no-Workbench own-screen test))

install-fsuae:
	@mkdir -p "$(FSUAE_CFG_DIR)"
	@cp tools/fsuae/*.fs-uae "$(FSUAE_CFG_DIR)/"
	$(info Installed FS-UAE configs -> $(FSUAE_CFG_DIR)/  (HDPart, HDPart-204, HDPart-204-ownscreen))

clean:
	$(info Cleaning...)
ifdef WINDOWS
	@del /q obj\* out\*
else
	@$(RM) obj/* out/*
endif

-include $(c_objects:.o=.d)


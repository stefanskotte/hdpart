# HDPart - AmigaOS application Makefile (Bartman m68k-amiga-elf toolchain)
# Builds a normal AmigaDOS hunk executable (NOT a bare-metal demo).

ifdef OS
	WINDOWS = 1
	SHELL = cmd.exe
endif

CC    = m68k-amiga-elf-gcc
OUT   = out/HDPart

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
CCFLAGS = -g -MP -MMD -m68000 -Os -nostdlib -fomit-frame-pointer \
          -Wall -Wextra -Wno-unused-function \
          -ffunction-sections -fdata-sections -Isrc
LDFLAGS = -nostdlib -Wl,-e,_start,--emit-relocs,--gc-sections,-Map=$(OUT).map

all: $(OUT)

$(OUT): $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(OUT)

$(OUT).elf: $(c_objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(c_objects) -o $@

obj/%.o : src/%.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $(CURDIR)/$<

clean:
	$(info Cleaning...)
ifdef WINDOWS
	@del /q obj\* out\*
else
	@$(RM) obj/* out/*
endif

-include $(c_objects:.o=.d)

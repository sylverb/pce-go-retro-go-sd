# PC Engine / PC Engine CD — standalone Retro-Go SD core
# (firmware main: pce-go + Core/Src/porting/pce, adapted as dynamic CORE).
#
#   make              # → pce.bin + pce.xip  (HuCard + SGX + CD cue/OGG + CHD)
#   make host         # → ./pce_host
#   make docker
#
# pce.xip is a QSPI sidecar (cold OGG/LZMA-enc text + leftover rodata),
# same pattern as gba.xip — copy both files to /cores/.
#
# Sources copied from ori/game-and-watch-retro-go-sd (firmware PCE before
# external-core split).

#######################################
# Project identity
#######################################
PROJECT_KIND ?= core

CORE_NAME      := pce
PACKED_BIN     := pce.bin
XIP_BIN        := $(CORE_NAME).xip
BUILD_DIR      := build/$(PROJECT_KIND)
CORE_PACK_NAME := PCE-GO
CORE_ENTRY     := app_main_pce

# Both disc backends are always linked; runtime picks by ACTIVE_FILE->ext
# (.chd → libchdr heap/LUT8, .cue → OGG arena). Heaps never coexist.
PCE_CD_OGG := 1
PCE_CD_CHD := 1

CORE_C_SOURCES := \
src/pce-go/gfx.c \
src/pce-go/h6280.c \
src/pce-go/pce.c \
src/porting/sound_pce.c \
src/porting/pce_cd.c \
src/porting/pce_scsi.c \
src/porting/pce_adpcm.c \
src/main_pce.c \
src/porting/pce_cd_ogg.c \
src/porting/pce_cd_chd.c \
src/porting/pce_cd_chd_alloc.c

CORE_C_INCLUDES := \
-Isrc/porting \
-Isrc/pce-go \
-Isrc/porting/third_party/stb_vorbis

# stb_vorbis needs sin/cos/log/exp/ldexp from libm (core links -nostdlib).
CORE_LDLIBS := -lm

CHDR := src/porting/third_party/libchdr
LZMA := $(CHDR)/deps/lzma-24.05
ZLIB := $(CHDR)/deps/zlib-1.3.1
CORE_C_SOURCES += \
$(CHDR)/src/libchdr_chd.c \
$(CHDR)/src/libchdr_cdrom.c \
$(CHDR)/src/libchdr_flac.c \
$(CHDR)/src/libchdr_bitstream.c \
$(CHDR)/src/libchdr_huffman.c \
$(LZMA)/src/Alloc.c \
$(LZMA)/src/CpuArch.c \
$(LZMA)/src/LzmaDec.c \
$(LZMA)/src/LzmaEnc.c \
$(LZMA)/src/LzFind.c \
$(LZMA)/src/Sort.c \
$(LZMA)/src/Delta.c \
$(ZLIB)/adler32.c \
$(ZLIB)/crc32.c \
$(ZLIB)/inffast.c \
$(ZLIB)/inflate.c \
$(ZLIB)/inftrees.c \
$(ZLIB)/zutil.c
CORE_C_INCLUDES += \
-I$(CHDR)/include \
-I$(CHDR)/include/libchdr \
-I$(LZMA)/include \
-I$(ZLIB)

GNW_CORE_SDK ?= sdk

# Match firmware release -O2 for the interpreter / CD path.
OPT = -O2
PCE_ALIGN_CFLAGS := -falign-functions -falign-jumps -fmerge-all-constants

CORE_LDSCRIPT := ld/pce_core.ld
CORE_EXTRA_SEGMENTS := itcm:core_itcm

#######################################
# Kind-specific compile defs + packing
#######################################
ifeq ($(PROJECT_KIND),core)
CORE_C_DEFS := \
-DPROJECT_KIND_CORE=1 \
-DCOVERFLOW=1 \
-DCHEAT_CODES=1 \
-DMAX_CHEAT_CODES=13 \
-DGNW_DISABLE_COMPRESSION \
-DTARGET_GNW \
-DPCE_CD_OGG=1 \
-DPCE_CD_CHD=1 \
-DZ7_ST \
-DZ_SOLO

PAD_LOGO     := src/assets/pad.bmp
HEADER_LOGO  := src/assets/header.bmp
HEADER_CD    := src/assets/header_cd.bmp
HEADER_SGX   := src/assets/header_sgx.png

else
$(error PROJECT_KIND must be 'core' (got '$(PROJECT_KIND)'))
endif

include $(GNW_CORE_SDK)/Makefile

CFLAGS += $(PCE_ALIGN_CFLAGS)
# Vendored zlib/lzma are noisy under -Wall.
CFLAGS += -Wno-unused -Wno-unused-parameter -Wno-unused-function

PACK_CORE := $(GNW_CORE_SDK)/tools/pack_core.py

CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

#######################################
# Pack
#######################################
.PHONY: pack

# Two CD systems share dirname=pcecd: cue needs parse=cdrom (firmware folder
# collapse); CHD must use parse=rom + ext=chd (parse=cdrom ignores extensions).
PACK_SYSTEM_ARGS := \
	--system name="PC Engine",dirname=pce,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_LOGO),ext=pce,parse=rom,cheat_ext=pceplus \
	--system name="SuperGrafx",dirname=sgx,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_SGX),ext="sgx pce",parse=rom,cheat_ext=pceplus \
	--system name="PC Engine CD",dirname=pcecd,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_CD),ext="cue chd",parse=cdrom,cheat_ext=pceplus

$(XIP_BIN): $(TARGET_ELF)
	$(V)$(ECHO) [ XIP ] $(XIP_BIN)
	$(V)$(CP) -O binary --only-section=.xip_pce --only-section=.rodata_pce $< $@

pack: $(TARGET_BIN) $(BUILD_DIR)/$(CORE_NAME)_core_itcm.bin $(XIP_BIN) $(PAD_LOGO) $(HEADER_LOGO) $(HEADER_CD) $(HEADER_SGX)
	$(V)$(ECHO) [ PACK CORE ] $(PACKED_BIN) + $(XIP_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_CORE) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		$(PACK_SYSTEM_ARGS) \
		--logo-invert \
		--segment itcm:__ITCM_CORE_START__:__CORE_ITCM_CODE_END__:__CORE_ITCM_BSS_END__:$(BUILD_DIR)/$(CORE_NAME)_core_itcm.bin \
		--core-name "$(CORE_PACK_NAME)" \
		--version "$(CORE_VERSION)" \
		--out $(PACKED_BIN)

all: pack

.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-XIP_BIN print-CORE_VERSION
print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)
print-TARGET_ELF:
	@echo $(TARGET_ELF)
print-TARGET_MAP:
	@echo $(BUILD_DIR)/$(CORE_NAME)_core.map
print-XIP_BIN:
	@echo $(XIP_BIN)
print-CORE_VERSION:
	@echo $(CORE_VERSION)

clean::
	$(V)rm -f pce.bin pcechd.bin $(XIP_BIN)

#######################################
# Docker
#######################################
.PHONY: docker docker_pull docker_shell

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker:
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE)
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc) PROJECT_KIND=$(PROJECT_KIND)

docker_pull:
	$(V)$(ECHO) "[ PULL ]" $(DOCKER_IMAGE)
	$(V)docker pull $(DOCKER_IMAGE)

docker_shell:
	$(DOCKER_RUN) bash

#######################################
# Host SDL
#######################################
include host/Makefile.host

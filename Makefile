# PC Engine / PC Engine CD — standalone Retro-Go SD core
# (firmware main: pce-go + Core/Src/porting/pce, adapted as dynamic CORE).
#
#   make                  — build + pack → pce.bin
#   make host             — Linux/macOS SDL binary
#   make docker           — Docker builder image
#
# Sources copied from ori/game-and-watch-retro-go-sd (firmware PCE before
# external-core split). BUILD_DIR=build/core must match ld/pce_core.ld.

#######################################
# Project identity
#######################################
PROJECT_KIND ?= core

CORE_NAME  := pce
CORE_ENTRY := app_main_pce

CORE_C_SOURCES := \
src/pce-go/gfx.c \
src/pce-go/h6280.c \
src/pce-go/pce.c \
src/porting/sound_pce.c \
src/porting/pce_cd.c \
src/porting/pce_scsi.c \
src/porting/pce_adpcm.c \
src/main_pce.c

CORE_C_INCLUDES := \
-Isrc/porting \
-Isrc/pce-go

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build/$(PROJECT_KIND)

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
-DTARGET_GNW

PACKED_BIN   := $(CORE_NAME).bin
PAD_LOGO     := src/assets/pad.bmp
HEADER_LOGO  := src/assets/header.bmp
HEADER_CD    := src/assets/header_cd.bmp
HEADER_SGX   := src/assets/header_sgx.png

else
$(error PROJECT_KIND must be 'core' (got '$(PROJECT_KIND)'))
endif

include $(GNW_CORE_SDK)/Makefile

CFLAGS += $(PCE_ALIGN_CFLAGS)

PACK_CORE := $(GNW_CORE_SDK)/tools/pack_core.py

CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

#######################################
# Pack
#######################################
.PHONY: pack

pack: $(TARGET_BIN) $(BUILD_DIR)/pce_core_itcm.bin $(PAD_LOGO) $(HEADER_LOGO) $(HEADER_CD) $(HEADER_SGX)
	$(V)$(ECHO) [ PACK CORE ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_CORE) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--system name="PC Engine",dirname=pce,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_LOGO),ext=pce,parse=rom,cheat_ext=pceplus \
		--system name="SuperGrafx",dirname=sgx,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_SGX),ext="sgx pce",parse=rom,cheat_ext=pceplus \
		--system name="PC Engine CD",dirname=pcecd,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_CD),ext=cue,parse=cdrom,cheat_ext=pceplus \
		--logo-invert \
		--segment itcm:__ITCM_CORE_START__:__CORE_ITCM_CODE_END__:__CORE_ITCM_BSS_END__:$(BUILD_DIR)/pce_core_itcm.bin \
		--core-name "PCE-GO" \
		--version "$(CORE_VERSION)" \
		--out $(PACKED_BIN)

all: pack

.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-CORE_VERSION
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
print-CORE_VERSION:
	@echo $(CORE_VERSION)

clean::
	$(V)rm -f $(PACKED_BIN)

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
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE) "PROJECT_KIND=$(PROJECT_KIND)"
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

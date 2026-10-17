/*
 * Host stand-in for gw_core_bridge.h macros / linker symbols.
 * Include instead of gw_core_bridge.h when building with -DHOST_BUILD.
 */
#pragma once

#include <stdint.h>
#include "rom_manager.h"
#include "gw_malloc.h"
#include "common.h"
#include "gw_firmware_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same names the device linker script exports. */
extern uint32_t __CORE_BSS_END__;
extern uint32_t __CORE_CODE_END__;

void gw_core_bridge_init(void);

/* Base of the host RAM_EMU bump pool (same role as device ram_start). */
void *host_ram_pool_base(void);

/* Optional: path passed on the CLI / HOST_ROM for core ROM load. */
void host_set_rom_path(const char *path);
int host_poll_events(void); /* returns 0 if the window should quit */

/* Map firmware SD absolute path ("/bios/pce/syscard3.pce") via $HOST_SD. */
int host_map_sd_path(const char *sd_path, char *out, size_t out_sz);
uint8_t *host_load_sd_file(const char *sd_path, uint32_t *size_out);

#ifdef __cplusplus
}
#endif

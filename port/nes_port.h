#pragma once

#include <stddef.h>
#include <stdint.h>

#include "module_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void nes_port_set_host(const module_host_api_v1 *host);
const module_host_api_v1 *nes_port_host(void);
void *nes_port_malloc(size_t size, uint32_t caps);
void *nes_port_calloc(size_t count, size_t size, uint32_t caps);
void nes_port_free(void *ptr);
uint32_t nes_port_millis(void);
uint64_t nes_port_micros(void);
void nes_port_delay(uint32_t ms);
uint32_t nes_port_random(void);
/**
 * @brief Convert a function pointer stored in ELF data sections to executable IROM space when needed.
 */
void *nes_port_exec_ptr(const void *ptr);
void nes_port_log(const char *text);
void nes_port_logf(const char *fmt, ...);
int nes_port_sd_open(const char *path, const char *mode, void **out_file);
uint32_t nes_port_file_read(void *file, void *buf, uint32_t len);
uint32_t nes_port_file_write(void *file, const void *buf, uint32_t len);
int nes_port_file_seek(void *file, int64_t offset, uint32_t mode);
uint64_t nes_port_file_position(void *file);
uint64_t nes_port_file_size(void *file);
int nes_port_file_available(void *file);
int nes_port_file_is_directory(void *file);
void nes_port_file_flush(void *file);
void nes_port_file_close(void *file);

#ifdef __cplusplus
}
#endif

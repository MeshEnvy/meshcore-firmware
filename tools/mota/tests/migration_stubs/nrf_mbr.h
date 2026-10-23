#pragma once
#include <stdint.h>

#define SD_MBR_COMMAND         0x18
#define SD_MBR_COMMAND_COPY_BL 1u

struct sd_mbr_command_t {
  uint32_t command;
  union {
    struct {
      uint32_t *bl_src;
      uint32_t bl_len;
    } copy_bl;
  } params;
};

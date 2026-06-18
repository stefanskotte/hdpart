#include "fsload.h"
#include <string.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

int fsload_is_hunk_file(const uint8_t *buf, uint32_t len)
{
    if (!buf || len < 4) return 0;
    return be32(buf) == HUNK_HEADER ? 1 : 0;
}

const char *fsl_err_text(int rc)
{
    switch (rc) {
    case FSL_OK:           return "OK";
    case FSL_EOPEN:        return "Could not open the file.";
    case FSL_EREAD:        return "Error reading the file.";
    case FSL_ETOOBIG:      return "File is too large for an RDB filesystem.";
    case FSL_ENOTLOADFILE: return "Not an AmigaDOS filesystem (no HUNK header).";
    case FSL_ENOMEM:       return "Out of memory.";
    case FSL_EFULL:        return "Filesystem list is full.";
    case FSL_ENOFS:        return "That disk has no embedded filesystem.";
    default:               return "Unknown error.";
    }
}

#ifndef HDPART_RDB_H
#define HDPART_RDB_H
#include <stdint.h>

#define RDB_BLOCK_BYTES 512
#define RDB_MAX_PARTS   32
#define RDB_NAME_LEN    32

typedef int (*BlockIO)(void *ctx, uint32_t block, uint8_t *buf, int write);

#endif

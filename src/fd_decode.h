#ifndef _UGRPC_FD_DECODE_H
#define _UGRPC_FD_DECODE_H

#include "pb_decode.h"

#include <stddef.h>
#include <stdint.h>

extern const struct decode_ops fd_decode_ops;
extern const struct decode_ops fds_decode_ops;
// the following can be freed using free()
void *new_fd_decode_ctx(void);
void *new_fds_decode_ctx(void);

int decode_fd_bytes(const uint8_t *bytes, size_t len);

#ifdef CLEANUP_SYMBOL_DB
void cleanup_fd_symbols(void);
#endif

#endif /* _UGRPC_FD_DECODE_H */

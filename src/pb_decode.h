#ifndef _UGRPC_PB_DECODE_H
#define _UGRPC_PB_DECODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define INIT_MAX_NEST 4
// somewhat arbitrary but apparently enforced by other deserializers
#define MAX_NEST_LIMIT 100

// from https://protobuf.dev/programming-guides/encoding/
enum protobuf_wire_type {
    PB_WIRE_TYPE_VARINT = 0,
    PB_WIRE_TYPE_I64 = 1,
    PB_WIRE_TYPE_LEN = 2,
    PB_WIRE_TYPE_SGROUP = 3,
    PB_WIRE_TYPE_EGROUP = 4,
    PB_WIRE_TYPE_I32 = 5
};

enum pb_field_action {
    // numbers less that 0 are errors
    FIELD_ACTION_CONSUME = 0,
    FIELD_ACTION_SKIP = 1,
    FIELD_ACTION_PARSE_BYTES = 2,
    FIELD_ACTION_PARSE_REPEATED_VARINT = 3,
    FIELD_ACTION_PARSE_REPEATED_I64 = 4,
    FIELD_ACTION_PARSE_REPEATED_I32 = 5,
    FIELD_ACTION_PARSE_MESSAGE = 6
};

struct decode_ops {
    int (*field)(int nr, enum protobuf_wire_type wire_type,
            uint64_t value, void *ctx);
    int (*ldv_bytes)(const uint8_t *buf, int len, bool last, void *ctx);
    int (*end_message)(void *ctx);
};

struct pb_decode_info *new_parse_info(int msg_len);
void free_parse_info(struct pb_decode_info *info);
int pb_decode_chunk(struct pb_decode_info *info, const uint8_t *buf,
        size_t len, const struct decode_ops *ops, void *ctx);

#endif /* _UGRPC_PB_DECODE_H */

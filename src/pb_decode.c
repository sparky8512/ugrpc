//
// support for decoding raw protocol buffer messages using provided callbacks
//

#include "error.h"
#include "pb_decode.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum parse_state {
    PARSE_NONE = 0,
    PARSE_SKIP = FIELD_ACTION_SKIP,
    PARSE_BYTES = FIELD_ACTION_PARSE_BYTES,
    PARSE_TAG_VARINT = PARSE_BYTES + 1,
    PARSE_VALUE_OR_LENGTH_VARINT = PARSE_TAG_VARINT + 1,
    PARSE_FIXED_INT = PARSE_VALUE_OR_LENGTH_VARINT + 1
};

// state info for the wire-level protobuf decoder
struct pb_decode_info {
    enum parse_state state;
    int remain_bytes;   // bytes left in message
    uint64_t value;     // vint or fint
    int field_nr;
    enum protobuf_wire_type wire_type;
    int skip_field_nr;  // perf hint: skip unpacked repeated field
    int pending_bytes;  // for skip ldv or get bytes
    int repeated_bytes; // for parse repeated vint/fint
    int value_bytes;    // max for vint, tot for fint
    int nest_level;
    int max_nest_level;
    int *nest_stack;    // stack of remain_bytes values
};

struct pb_decode_info *new_parse_info(int msg_len)
{
    struct pb_decode_info *info = malloc(sizeof(struct pb_decode_info));
    if (!info) {
        return NULL;
    }

    memset(info, 0, sizeof(struct pb_decode_info));
    info->max_nest_level = INIT_MAX_NEST;
    info->nest_stack = malloc(sizeof(int) * INIT_MAX_NEST);
    info->remain_bytes = msg_len;

    if (!info->nest_stack) {
        free(info);
        return NULL;
    }

    return info;
}

void free_parse_info(struct pb_decode_info *info)
{
    free(info->nest_stack);
    free(info);
}

static inline void start_fixed_int(struct pb_decode_info *info)
{
    info->value = 0;
    info->value_bytes = info->wire_type == PB_WIRE_TYPE_I64 ? 8 : 4;
}

static int get_fixed_int(struct pb_decode_info *info, const uint8_t **pbuf, const uint8_t *buf_end)
{
    const uint8_t *buf = *pbuf;
    int max_bytes = info->wire_type == PB_WIRE_TYPE_I64 ? 8 : 4;
    while (true) {
        if (buf >= buf_end) {
            *pbuf = buf;
            return 1;
        }

        uint8_t b = *buf++;
        info->value = info->value | (uint64_t)b << 8 * (max_bytes - info->value_bytes);
        info->value_bytes--;
        if (info->value_bytes == 0) {
            *pbuf = buf;
            return 0;
        }
    }
}

static inline void start_varint(struct pb_decode_info *info)
{
    info->value = 0;
    info->value_bytes = 10;
}

static int get_varint(struct pb_decode_info *info, const uint8_t **pbuf, const uint8_t *buf_end)
{
    const uint8_t *buf = *pbuf;
    while (true) {
        if (buf >= buf_end) {
            *pbuf = buf;
            return 1;
        }

        uint8_t b = *buf++;
        info->value = info->value | (uint64_t)(b & 127) << 7 * (10 - info->value_bytes);
        if ((b & 128) == 0) {
            *pbuf = buf;
            return 0;
        }
        info->value_bytes--;

        if (info->value_bytes == 0) {
            // max varint size overflow
            *pbuf = buf;
            return ERROR_DECODE;
        }
    }
}

int pb_decode_chunk(struct pb_decode_info *info, const uint8_t *buf, size_t len, const struct decode_ops *ops, void *ctx)
{
    int rv;
    int field_rv;
    int bytes_len;
    const uint8_t *orig_buf = buf;
    const uint8_t *chunk_end = buf + len;
    const uint8_t *message_end = buf + (info->repeated_bytes ? info->repeated_bytes : info->remain_bytes);
    message_end = message_end > chunk_end ? chunk_end : message_end;

    switch (info->state) {
        case PARSE_NONE:
            break;
        case PARSE_TAG_VARINT:
            goto continue_tag_varint;
        case PARSE_VALUE_OR_LENGTH_VARINT:
            goto continue_value_or_length_varint;
        case PARSE_FIXED_INT:
            goto continue_fixed_int;
        case PARSE_SKIP:
        case PARSE_BYTES:
            bytes_len = info->pending_bytes;
            field_rv = (int)info->state;
            goto continue_parse_ldv;
    }

    while (buf < chunk_end) {
        if (!info->repeated_bytes) {
            // recompute, in case just finished parsing repeated
            message_end = buf + info->remain_bytes;
            message_end = message_end > chunk_end ? chunk_end : message_end;

            start_varint(info);
continue_tag_varint:
            rv = get_varint(info, &buf, message_end);
            if (rv < 0) {
                return rv;
            }
            if (rv) {
                if (buf != chunk_end) {
                    // message length overflow
                    return ERROR_DECODE;
                }
                info->state = PARSE_TAG_VARINT;
                info->remain_bytes -= buf - orig_buf;
                return 0;
            }

            uint64_t tag = info->value;
            if (tag >> 32 != 0) {
                // field number out of range
                return ERROR_DECODE;
            }
            info->wire_type = (enum protobuf_wire_type)(tag & 7);
            info->field_nr = (int)(((uint32_t)tag) >> 3);
            if (info->field_nr == 0) {
                return ERROR_DECODE;
            }
        }

        switch (info->wire_type) {
            case PB_WIRE_TYPE_VARINT:
            case PB_WIRE_TYPE_LEN:
                start_varint(info);
continue_value_or_length_varint:
                rv = get_varint(info, &buf, message_end);
                if (rv < 0) {
                    return rv;
                }
                if (info->repeated_bytes) {
                    info->repeated_bytes -= buf - orig_buf;
                }
                if (rv) {
                    if (buf != chunk_end) {
                        return ERROR_DECODE;
                    }
                    info->state = PARSE_VALUE_OR_LENGTH_VARINT;
                    info->remain_bytes -= buf - orig_buf;
                    return 0;
                }
                if (info->wire_type == PB_WIRE_TYPE_LEN) {
                    if (info->value > INT32_MAX) {
                        return ERROR_DECODE;
                    }
                    if ((int)info->value > info->remain_bytes - (buf - orig_buf)) {
                        return ERROR_DECODE;
                    }
                }
                break;
            case PB_WIRE_TYPE_SGROUP:
            case PB_WIRE_TYPE_EGROUP:
                // nothing else to do for group tags
                info->value = 0;
                break;
            case PB_WIRE_TYPE_I64:
            case PB_WIRE_TYPE_I32:
                start_fixed_int(info);
continue_fixed_int:
                rv = get_fixed_int(info, &buf, message_end);
                if (rv < 0) {
                    return rv;
                }
                if (info->repeated_bytes) {
                    info->repeated_bytes -= buf - orig_buf;
                }
                if (rv) {
                    if (buf != chunk_end) {
                        return ERROR_DECODE;
                    }
                    info->state = PARSE_FIXED_INT;
                    info->remain_bytes -= buf - orig_buf;
                    return 0;
                }
                break;
            default:
                // invalid wire type
                return ERROR_DECODE;
        }
        if (info->field_nr == info->skip_field_nr) {
            field_rv = FIELD_ACTION_SKIP;
        } else {
            field_rv = ops->field(info->field_nr, info->wire_type, info->value, ctx);
            if (field_rv == FIELD_ACTION_SKIP) {
                info->skip_field_nr = info->field_nr;
            }
        }
        if (field_rv < 0) {
            return field_rv;
        }
        if (info->wire_type == PB_WIRE_TYPE_LEN && field_rv == FIELD_ACTION_PARSE_MESSAGE) {
            if (info->nest_level >= info->max_nest_level) {
                if (info->nest_level > MAX_NEST_LIMIT) {
                    fprintf(stderr, "Max message nest depth exceeded\n");
                    return ERROR_REPORTED;
                }
                info->max_nest_level += INIT_MAX_NEST;
                int *new_stack = realloc(info->nest_stack, sizeof(int) * info->max_nest_level);
                if (new_stack) {
                    info->nest_stack = new_stack;
                } else {
                    return ERROR_ALLOCATION;
                }
            }
            int message_len = (int)info->value;
            info->nest_stack[info->nest_level++] = info->remain_bytes - (buf - orig_buf) - message_len;
            orig_buf = buf;
            info->remain_bytes = message_len;
            // it's fine to lose this, it's just a perf hint:
            info->skip_field_nr = 0;
            message_end = buf + message_len;
            message_end = message_end > chunk_end ? chunk_end : message_end;
        } else if (info->wire_type == PB_WIRE_TYPE_LEN) {
            bytes_len = (int)info->value;
            // for packed scalar, set appropriate wire type and use
            // repeated_bytes to track how much left to parse
            if (field_rv == FIELD_ACTION_PARSE_REPEATED_VARINT) {
                info->wire_type = PB_WIRE_TYPE_VARINT;
                info->repeated_bytes = bytes_len;
            } else if (field_rv == FIELD_ACTION_PARSE_REPEATED_I64) {
                info->wire_type = PB_WIRE_TYPE_I64;
                info->repeated_bytes = bytes_len;
            } else if (field_rv == FIELD_ACTION_PARSE_REPEATED_I32) {
                info->wire_type = PB_WIRE_TYPE_I32;
                info->repeated_bytes = bytes_len;
            } else {
            // for other lvd types, use pending_bytes to track how
            // much is left to restart in next chunk
                int bytes_avail;
continue_parse_ldv:
                bytes_avail = message_end - buf;
                if (bytes_len > bytes_avail) {
                    info->pending_bytes = bytes_len - bytes_avail;
                    bytes_len = bytes_avail;
                } else {
                    info->pending_bytes = 0;
                }
                switch (field_rv) {
                    case FIELD_ACTION_SKIP:
                    default:
                        info->state = PARSE_SKIP;
                        break;
                    case FIELD_ACTION_PARSE_BYTES:
                        if (bytes_len || info->pending_bytes == 0) {
                            rv = ops->ldv_bytes(buf, bytes_len, info->pending_bytes == 0, ctx);
                            if (rv < 0) {
                                return rv;
                            }
                        }
                        info->state = PARSE_BYTES;
                        break;
                }
                buf += bytes_len;
                if (info->pending_bytes) {
                    info->remain_bytes -= buf - orig_buf;
                    return 0;
                }
                info->state = PARSE_NONE;
            }
        }
        info->remain_bytes -= buf - orig_buf;
        orig_buf = buf;
        while (info->remain_bytes == 0) {
            rv = ops->end_message(ctx);
            if (rv < 0) {
                return rv;
            }
            info->skip_field_nr = 0;
            if (info->nest_level == 0) {
                break;
            }
            info->remain_bytes = info->nest_stack[--info->nest_level];
            message_end = buf + info->remain_bytes;
            message_end = message_end > chunk_end ? chunk_end : message_end;
        }
    }
    info->state = PARSE_NONE;
    return 0;
}

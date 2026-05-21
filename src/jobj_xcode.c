//
// support for decoding serialized protocol buffer messages into JSON
// strings and vice versa
//

#include "error.h"
#include "jobj_xcode.h"
#include "fd_decode.h"
#include "pb_decode.h"
#include "symbol_db.h"

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libubox/avl.h>
#include <json-c/json_object.h>
#include <json-c/json_object_iterator.h>
#include <json-c/json_tokener.h>
#include <json-c/json_util.h>  // json_type_to_name

//
// base64 is reimplemented here based on RFC 4648 because the implementation
// in libubox needs a full nul-terminated string for decode, always does
// multiples of 4 chars to/from 3 bytes at a time and does not allow either
// base64url or unpadded input for decode.
//
static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_";

static inline void reset_outbuf(struct output_buffer *outbuf)
{
    outbuf->current = (uint8_t *)outbuf + sizeof(struct output_buffer);
}

struct output_buffer *new_outbuf(size_t size, flush_fn flush, void *flush_arg)
{
    struct output_buffer *outbuf;

    outbuf = malloc(sizeof(struct output_buffer) + size);
    if (outbuf) {
        reset_outbuf(outbuf);
        outbuf->end = outbuf->current + size;
        outbuf->flush = flush;
        outbuf->flush_arg = flush_arg;
    }

    return outbuf;
}

static int flush_outbuf(struct output_buffer *outbuf)
{
    int rv = outbuf->flush(outbuf, outbuf->flush_arg);

    if (!rv) {
        reset_outbuf(outbuf);
    }

    return rv;
}

// make sure at least 1 byte available
static inline int check_outbuf(struct output_buffer *outbuf)
{
    if (outbuf->current >= outbuf->end) {
        return flush_outbuf(outbuf);
    }
    return 0;
}

//
// NOTE: As of v0.18, json-c does not set errno to ERANGE when it caps to
// INT_MIN / INT_MAX (and similar) values. Later releases will, but for now,
// work around by getting both int64 and uint64 and doing separate range
// check.
//

static int get_json_u64(struct json_object *jobj, uint64_t *value)
{
    int64_t svalue = json_object_get_int64(jobj);
    if (svalue < 0) {
        return 1;
    }

    *value = json_object_get_uint64(jobj);

    return 0;
}

static int get_json_s64(struct json_object *jobj, uint64_t *value)
{
    uint64_t uvalue = json_object_get_uint64(jobj);
    if (uvalue > INT64_MAX) {
        return 1;
    }

    *value = (uint64_t)json_object_get_int64(jobj);

    return 0;
}

static int get_json_u32(struct json_object *jobj, uint64_t *value)
{
    int64_t svalue = json_object_get_int64(jobj);
    if (svalue < 0) {
        return 1;
    }

    *value = json_object_get_uint64(jobj);

    return *value > UINT32_MAX ? 1 : 0;
}

static int get_json_s32(struct json_object *jobj, uint64_t *value)
{
    uint64_t uvalue = json_object_get_uint64(jobj);
    if (uvalue > INT32_MAX) {
        return 1;
    }

    *value = (uint64_t)json_object_get_int64(jobj);

    return (int64_t)*value < INT32_MIN ? 1 : 0;
}

static int get_json_bool(struct json_object *jobj, uint64_t *value)
{
    json_bool value_bool = json_object_get_boolean(jobj);

    *value = value_bool ? 1 : 0;
    return 0;
}

static int get_json_double(struct json_object *jobj, uint64_t *value)
{
    errno = 0;
    double value_fp = json_object_get_double(jobj);

    memcpy(value, &value_fp, sizeof(*value));
    return errno ? 1 : 0;
}

static int get_json_float(struct json_object *jobj, uint64_t *value)
{
    errno = 0;
    double dvalue = json_object_get_double(jobj);
    if (errno || dvalue > FLT_MAX || dvalue < -FLT_MAX) {
        return 1;
    }

    float fvalue = (float)dvalue;
    uint32_t value_int;
    memcpy(&value_int, &fvalue, sizeof(value_int));
    *value = (uint64_t)value_int;

    return 0;
}

static void xform_none(uint64_t *value)
{
}

static void xform_zigzag64(uint64_t *value)
{
    int64_t svalue = (int64_t)*value;
    if (svalue < 0) {
        *value = (uint64_t)((-(svalue + 1) << 1) + 1);
    } else {
        *value = (uint64_t)(svalue << 1);
    }
}

static void xform_zigzag32(uint64_t *value)
{
    int32_t svalue = (int32_t)*value;
    if (svalue < 0) {
        *value = (uint64_t)((-(svalue + 1) << 1) + 1);
    } else {
        *value = (uint64_t)(svalue << 1);
    }
}

static void un_xform_zigzag64(uint64_t *value)
{
    uint64_t uvalue = *value;
    if (uvalue & 1) {
        int64_t svalue = -(int64_t)(uvalue >> 1) - 1;
        *value = (uint64_t)svalue;
    } else {
        *value = uvalue >> 1;
    }
}

static void un_xform_zigzag32(uint64_t *value)
{
    uint32_t uvalue = (uint32_t)*value;
    if (uvalue & 1) {
        int64_t svalue = -(int64_t)(uvalue >> 1) - 1;
        *value = (uint64_t)svalue;
    } else {
        *value = (uint64_t)(uvalue >> 1);
    }
}

// This function writes to len if outbuf is NULL, else it writes data to
// outbuf. The reason this is not 2 separate functions is to cut back on
// code duplication in the calling code
static int encode_varint(uint64_t value, int *len, struct output_buffer *outbuf)
{
    if (outbuf) {
        do {
            int rv = check_outbuf(outbuf);
            if (rv) {
                return rv;
            }

            uint8_t b = ((uint8_t)value) & 0x7f;
            value >>= 7;
            if (value) {
                b |= 0x80;
            }
            *outbuf->current++ = b;
        } while (value);
    } else {
#if defined(__has_builtin) && __has_builtin(__builtin_clz)
        *len = value ? (int)(64 - __builtin_clzll(value) + 6) / 7 : 1;
#else
        int count = 1;
        while (value >>= 7) {
            count++;
        }
        *len = count;
#endif
    }

    return 0;
}

static int encode_fixed_int(uint64_t value, int len, struct output_buffer *outbuf)
{
    if (outbuf) {
        while (len--) {
            int rv = check_outbuf(outbuf);
            if (rv) {
                return rv;
            }

            *outbuf->current++ = (uint8_t)value;
            value >>= 8;
        }
    }

    return 0;
}

static size_t decode_string_len(int src_len)
{
    return src_len;
}

static int encode_string_len(const char *src, int src_len)
{
    return src_len;
}

static int encode_string(const char *src, int src_len, struct output_buffer *outbuf)
{
    while (src_len) {
        int rv = check_outbuf(outbuf);
        if (rv) {
            return rv;
        }

        int len = outbuf->end - outbuf->current;
        if (len > src_len) {
            len = src_len;
        }
        memcpy(outbuf->current, src, (size_t)len);
        outbuf->current += len;
        src += len;
        src_len -= len;
    }

    return 0;
}

static size_t decode_bytes_len(int src_len)
{
    return (src_len + 2) / 3 * 4;
}

static int encode_bytes_len(const char *src, int src_len)
{
    int bin_len = src_len / 4 * 3;
    int partial_len = src_len % 4;

    if (partial_len) {
        // unpadded partial block, errors will be caught on encode
        bin_len += partial_len - 1;
    } else if (src_len && src[src_len-1] == '=') {
        // padded block
        bin_len--;
        if (src[src_len-2] == '=') {
            bin_len--;
        }
    }

    return bin_len;
}

static int b64_val(char c)
{
    const char *ind = strchr(b64_chars, c);
    if (!ind) {
        return -1;
    }

    int val = (int)(ind - b64_chars);
    return val < 64 ? val : val - 2;
}

static int encode_bytes(const char *src, int src_len, struct output_buffer *outbuf)
{
    int bits = 0;
    int bpos = 0;
    while (*src != '=' && src_len) {
        int val = b64_val(*src);
        if (val < 0) {
            fprintf(stderr, "Invalid base64 char '%c'\n", *src);
            return ERROR_REPORTED;
        }
        src++;
        bits = bits << 6 | val;
        bpos += 6;
        if (bpos >= 8) {
            int rv = check_outbuf(outbuf);
            if (rv) {
                return rv;
            }

            bpos -= 8;
            *outbuf->current++ = (uint8_t)(bits >> bpos);
        }
        src_len--;
    }

    // fail on incorrect padding or extra non-0 bits
    if (src_len > 2 || ((bits << (6-bpos)) & 0x3f)) {
        fprintf(stderr, "Bad base64 padding\n");
        return ERROR_REPORTED;
    }

    while (src_len) {
        if (*src++ != '=') {
            fprintf(stderr, "Bad base64 padding\n");
            return ERROR_REPORTED;
        }
        src_len--;
    }

    return 0;
}

static int encode_tag(int fld_number, int wire_type, struct output_buffer *outbuf)
{
    uint64_t tag = fld_number << 3 | wire_type;
    int tag_len = 0;
    int rv = encode_varint(tag, &tag_len, outbuf);
    if (rv) {
        return rv;
    }

    return tag_len;
}

static int encode_grpc_prefix(int size, struct output_buffer *outbuf)
{
    uint8_t b = 0;  // grpc prefix flags byte
    for (int i = 0; i < 5; i++) {
        int rv = check_outbuf(outbuf);
        if (rv) {
            return rv;
        }
        *outbuf->current++ = b;

        // grpc prefix size is big endian
        b = (uint8_t)(size >> 24);
        size <<= 8;
    }

    return 0;
}

struct field_xcode_ops {
    union {
        struct {
            int (*get_value)(struct json_object *jobj, uint64_t *value);
            void (*xform)(uint64_t *value);
            void (*un_xform)(uint64_t *value);
        } varint;
        struct {
            int (*get_value)(struct json_object *jobj, uint64_t *value);
        } fixed_int;
        struct {
            size_t (*decode_len)(int src_len);
            int (*encode_len)(const char *src, int src_len);
            int (*encode)(const char *src, int src_len, struct output_buffer *outbuf);
        } ldv;
    };
    enum json_type json_type;
    enum protobuf_wire_type wire_type;
    bool is_signed;
    bool is_32bit;
};

static const struct field_xcode_ops xcode_ops[] = {
    [PB_TYPE_RESERVED] = { { { NULL } }, json_type_null, 0, false, false },
    [PB_TYPE_DOUBLE] = { .fixed_int = { get_json_double }, json_type_double /* or int or string */, PB_WIRE_TYPE_I64, true, false },
    [PB_TYPE_FLOAT] = { .fixed_int = { get_json_float }, json_type_double /* or int or string */, PB_WIRE_TYPE_I32, true, true },
    [PB_TYPE_INT64] = { .varint = { get_json_s64, xform_none, xform_none }, json_type_int, PB_WIRE_TYPE_VARINT, true, false },
    [PB_TYPE_UINT64] = { .varint = { get_json_u64, xform_none, xform_none }, json_type_int, PB_WIRE_TYPE_VARINT, false, false },
    [PB_TYPE_INT32] = { .varint = { get_json_s32, xform_none, xform_none }, json_type_int, PB_WIRE_TYPE_VARINT, true, true },
    [PB_TYPE_FIXED64] = { .fixed_int = { get_json_u64 }, json_type_int, PB_WIRE_TYPE_I64, false, false },
    [PB_TYPE_FIXED32] = { .fixed_int = { get_json_u32 }, json_type_int, PB_WIRE_TYPE_I32, false, true },
    [PB_TYPE_BOOL] = { .varint = { get_json_bool, xform_none, xform_none }, json_type_boolean, PB_WIRE_TYPE_VARINT, false, true },
    [PB_TYPE_STRING] = { .ldv = { decode_string_len, encode_string_len, encode_string }, json_type_string, PB_WIRE_TYPE_LEN, false, false },
    [PB_TYPE_GROUP] ={ { { NULL } }, json_type_null, PB_WIRE_TYPE_LEN, false, false },
    [PB_TYPE_MESSAGE] = { { { NULL } }, json_type_object, PB_WIRE_TYPE_LEN, false, false },
    [PB_TYPE_BYTES] = { .ldv = { decode_bytes_len, encode_bytes_len, encode_bytes }, json_type_string, PB_WIRE_TYPE_LEN, false, false },
    [PB_TYPE_UINT32] = { .varint = { get_json_u32, xform_none, xform_none }, json_type_int, PB_WIRE_TYPE_VARINT, false, true },
    [PB_TYPE_ENUM] = { .varint = { get_json_s32, xform_none, xform_none }, json_type_string /* or int */, PB_WIRE_TYPE_VARINT, true, true },
    [PB_TYPE_SFIXED32] = { .fixed_int = { get_json_s32 }, json_type_int, PB_WIRE_TYPE_I32, true, true },
    [PB_TYPE_SFIXED64] = { .fixed_int = { get_json_s64 }, json_type_int, PB_WIRE_TYPE_I64, true, false },
    [PB_TYPE_SINT32] = { .varint = { get_json_s32, xform_zigzag32, un_xform_zigzag32 }, json_type_int, PB_WIRE_TYPE_VARINT, true, true },
    [PB_TYPE_SINT64] = { .varint = { get_json_s64, xform_zigzag64, un_xform_zigzag64 }, json_type_int, PB_WIRE_TYPE_VARINT, true, false },
};

static int encode_field(struct json_object *jobj, struct field_type *fld_type, struct message_type *msg_type, struct output_buffer *outbuf)
{
    const struct field_xcode_ops *ops = &xcode_ops[fld_type->type];

    int payload_len = 0;
    switch (ops->wire_type) {
        case PB_WIRE_TYPE_VARINT:
            {
                uint64_t value;
                if (fld_type->type == PB_TYPE_ENUM &&
                        json_object_is_type(jobj, json_type_string)) {
                    struct enum_type *enm_type = resolve_enm_type(&fld_type->type_name,
                            msg_type->name);
                    if (!enm_type) {
                        return ERROR_REPORTED;
                    }

                    const char *str = json_object_get_string(jobj);
                    struct enum_value_type *val_type;
                    val_type = avl_find_element(&enm_type->values_by_name, str,
                            val_type, name_node);
                    if (!val_type) {
                        fprintf(stderr, "Unknown enum value '%s' on field %s of %s\n",
                                str, fld_type->name, msg_type->name);
                        return ERROR_REPORTED;
                    }
                    value = (uint64_t)val_type->number;
                } else {
                    if (ops->varint.get_value(jobj, &value)) {
                        fprintf(stderr, "Numeric overflow or invalid number on field %s of %s\n",
                                fld_type->name, msg_type->name);
                        return ERROR_REPORTED;
                    }
                }
                ops->varint.xform(&value);
                int rv = encode_varint(value, &payload_len, outbuf);
                if (rv) {
                    return rv;
                }
            }
            break;

        case PB_WIRE_TYPE_I64:
        case PB_WIRE_TYPE_I32:
            {
                uint64_t value;
                if (ops->fixed_int.get_value(jobj, &value)) {
                    fprintf(stderr, "Numeric overflow or invalid number on field %s of %s\n",
                            fld_type->name, msg_type->name);
                    return ERROR_REPORTED;
                }
                payload_len = ops->wire_type == PB_WIRE_TYPE_I64 ? 8 : 4;
                int rv = encode_fixed_int(value, payload_len, outbuf);
                if (rv) {
                    return rv;
                }
            }
            break;

        case PB_WIRE_TYPE_LEN:
            {
                const char *str = json_object_get_string(jobj);
                int value_len = json_object_get_string_len(jobj);
                int xform_len = ops->ldv.encode_len(str, value_len);
                int length_len = 0;
                int rv = encode_varint((uint64_t)xform_len, &length_len, outbuf);
                if (rv) {
                    return rv;
                }
                if (outbuf) {
                    rv = ops->ldv.encode(str, value_len, outbuf);
                    if (rv) {
                        return rv;
                    }
                }
                payload_len = length_len + xform_len;
            }
            break;

        default:
            // shouldn't happen, bad type info
            return ERROR_INTERNAL(4);
    }

    return payload_len;
}

struct encode_stack_data {
    struct json_object_iterator iter;
    struct json_object_iterator end;
    struct message_type *msg_type;
    struct json_object *jobj;
    struct json_object *arr_jobj;
    int arr_idx;
    int arr_start;
    int size;
};

int jobj_encode_msg(struct json_object *top_jobj,
        struct message_type *msg_type, struct output_buffer *outbuf)
{
    if (!json_object_is_type(top_jobj, json_type_object)) {
        return ERROR_DECODE;
    }

    int max_nest_level = INIT_MAX_NEST;
    struct encode_stack_data *nest_stack = malloc(
            sizeof(struct encode_stack_data) * INIT_MAX_NEST);
    if (!nest_stack) {
        return ERROR_ALLOCATION;
    }

    int size = 0;
    bool output = false;
    int rv;

    // outer loop is executed twice: first with !output, then with output
    // first pass is to compute size, second for actual output
    do {
        struct json_object *jobj = top_jobj;
        struct json_object *arr_jobj = NULL;
        int arr_idx = 0;
        int arr_start = 0;
        int nest_level = 0;

        if (output) {
            encode_grpc_prefix(size, outbuf);
            // note that size is no longer used after this point, so the
            // following code only keeps it correct for !output case
        }

        struct output_buffer *poutbuf = output ? outbuf : NULL;

        struct json_object_iterator iter = json_object_iter_begin(jobj);
        struct json_object_iterator end = json_object_iter_end(jobj);
        while (!json_object_iter_equal(&iter, &end)) {
            const char *name = json_object_iter_peek_name(&iter);
            struct field_type *fld_type;
            fld_type = avl_find_element(&msg_type->fields_by_name, name, fld_type, name_node);
            if (!fld_type) {
                fprintf(stderr, "No field named \"%s\" in %s\n", name, msg_type->name);
                rv = ERROR_REPORTED;
                goto out_free;
            }

            struct json_object *value_jobj = json_object_iter_peek_value(&iter);
            const struct field_xcode_ops *ops = &xcode_ops[fld_type->type];
            bool skip_tag = false;

            if (fld_type->repeated) {
                // TODO: also check not unpacked
                bool packed_scalar = ops->wire_type != PB_WIRE_TYPE_LEN;

                if (!arr_jobj) {
                    if (!json_object_is_type(value_jobj, json_type_array)) {
                        fprintf(stderr, "Field %s of %s expected JSON type %s, got %s\n",
                                name, msg_type->name, "array",
                                json_type_to_name(json_object_get_type(value_jobj)));
                        rv = ERROR_REPORTED;
                        goto out_free;
                    }

                    // skip over empty arrays
                    if (!json_object_array_length(value_jobj)) {
                        json_object_iter_next(&iter);
                        goto done_repeated_field;
                    }

                    if (packed_scalar) {
                        int tag_len = encode_tag(fld_type->number, PB_WIRE_TYPE_LEN, poutbuf);
                        if (tag_len < 0) {
                            rv = tag_len;
                            goto out_free;
                        }

                        size += tag_len;

                        if (poutbuf) {
                            int arr_len = (int)(long)json_object_get_userdata(value_jobj);
                            rv = encode_varint((uint64_t)arr_len, NULL, poutbuf);
                            if (rv) {
                                goto out_free;
                            }
                        }
                    }

                    arr_jobj = value_jobj;
                    arr_idx = 0;
                    arr_start = size;
                }

                value_jobj = json_object_array_get_idx(arr_jobj, arr_idx++);
                if (!value_jobj) {
                    // end of array, similar to end of message
                    if (packed_scalar && !poutbuf) {
                        json_object_set_userdata(arr_jobj, (void *)(long)(size - arr_start), NULL);
                        int length_len = 0;
                        rv = encode_varint((uint64_t)size, &length_len, NULL);
                        if (rv) {
                            goto out_free;
                        }
                        size += length_len;
                    }

                    arr_jobj = NULL;
                    json_object_iter_next(&iter);
                    goto done_repeated_field;
                }

                if (packed_scalar) {
                    skip_tag = true;
                }
            }

            if (!(json_object_is_type(value_jobj, ops->json_type) ||
                    (fld_type->type == PB_TYPE_ENUM &&
                     json_object_is_type(value_jobj, json_type_int)) ||
                    (ops->json_type == json_type_double &&
                     (json_object_is_type(value_jobj, json_type_int) ||
                      json_object_is_type(value_jobj, json_type_string))))) {
                fprintf(stderr, "Field %s of %s expected JSON type %s, got %s\n",
                        name, msg_type->name, json_type_to_name(ops->json_type),
                        json_type_to_name(json_object_get_type(value_jobj)));
                rv = ERROR_REPORTED;
                goto out_free;
            }

            if (!skip_tag) {
                int tag_len = encode_tag(fld_type->number, ops->wire_type, poutbuf);
                if (tag_len < 0) {
                    rv = tag_len;
                    goto out_free;
                }

                size += tag_len;
            }

            if (fld_type->type == PB_TYPE_MESSAGE) {
                if (poutbuf) {
                    int msg_len = (int)(long)json_object_get_userdata(value_jobj);
                    rv = encode_varint((uint64_t)msg_len, NULL, poutbuf);
                    if (rv) {
                        goto out_free;
                    }
                }
                if (nest_level >= max_nest_level) {
                    if (nest_level > MAX_NEST_LIMIT) {
                        fprintf(stderr, "Max message nest depth exceeded\n");
                        rv = ERROR_REPORTED;
                        goto out_free;
                    }
                    max_nest_level += INIT_MAX_NEST;
                    struct encode_stack_data *new_stack = realloc(nest_stack,
                            sizeof(struct encode_stack_data) * max_nest_level);
                    if (new_stack) {
                        nest_stack = new_stack;
                    } else {
                        rv = ERROR_ALLOCATION;
                        goto out_free;
                    }
                }
                nest_stack[nest_level].iter = iter;
                nest_stack[nest_level].end = end;
                nest_stack[nest_level].msg_type = msg_type;
                nest_stack[nest_level].jobj = jobj;
                nest_stack[nest_level].arr_jobj = arr_jobj;
                nest_stack[nest_level].arr_idx = arr_idx;
                nest_stack[nest_level].arr_start = arr_start;
                nest_stack[nest_level].size = size;
                nest_level++;
                jobj = value_jobj;
                size = 0;
                arr_jobj = NULL;
                msg_type = resolve_msg_type(&fld_type->type_name, msg_type->name);
                if (!msg_type) {
                    rv = ERROR_REPORTED;
                    goto out_free;
                }

                iter = json_object_iter_begin(jobj);
                end = json_object_iter_end(jobj);
            } else {
                int payload_len = encode_field(value_jobj, fld_type, msg_type, poutbuf);
                if (payload_len < 0) {
                    rv = payload_len;
                    goto out_free;
                }

                size += payload_len;

                if (!arr_jobj) {
                    json_object_iter_next(&iter);
                }
            }

done_repeated_field:
            while (nest_level && json_object_iter_equal(&iter, &end)) {
                if (!poutbuf) {
                    json_object_set_userdata(jobj, (void *)(long)size, NULL);
                    int length_len = 0;
                    rv = encode_varint((uint64_t)size, &length_len, NULL);
                    if (rv) {
                        goto out_free;
                    }
                    size += length_len;
                }

                nest_level--;

                iter = nest_stack[nest_level].iter;
                end = nest_stack[nest_level].end;
                msg_type = nest_stack[nest_level].msg_type;
                jobj = nest_stack[nest_level].jobj;
                arr_jobj = nest_stack[nest_level].arr_jobj;
                arr_idx = nest_stack[nest_level].arr_idx;
                arr_start = nest_stack[nest_level].arr_start;
                size += nest_stack[nest_level].size;

                if (!arr_jobj) {
                    json_object_iter_next(&iter);
                }
            }
        }

        output = !output;
    } while (output);

    rv = flush_outbuf(outbuf);

out_free:
    free(nest_stack);

    return rv;
}

int jstr_encode_msg(const char* json_str, struct message_type *msg_type,
        struct output_buffer *outbuf)
{
    struct json_tokener *json_tok = json_tokener_new();
    if (!json_tok) {
        return ERROR_ALLOCATION;
    }

    json_tokener_set_flags(json_tok, JSON_TOKENER_STRICT |
            JSON_TOKENER_ALLOW_TRAILING_CHARS | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *jobj = json_tokener_parse_ex(json_tok, json_str, strlen(json_str) + 1);
    enum json_tokener_error json_err = json_tokener_get_error(json_tok);
    size_t parsed = json_tokener_get_parse_end(json_tok);
    json_tokener_free(json_tok);

    if (json_err != json_tokener_success) {
        const char *err_msg = json_err == json_tokener_continue ?
            "incomplete JSON object" : json_tokener_error_desc(json_err);
        fprintf(stderr, "parsing of request JSON failed at offset %d: %s\n", (int)parsed, err_msg);
        goto out_free_jobj;
    }

    int rv = jobj_encode_msg(jobj, msg_type, outbuf);

out_free_jobj:
    json_object_put(jobj);

    return rv;
}

////////////////////////////////////////////////////////////////////////////////

struct jobj_decode_nest_state {
    struct json_object *jobj;
    struct message_type *msg_type;
    // following used only by jstream_decode
    bool decoding_array;
    int prior_field_nr;
};

struct jobj_decode_state {
    struct jobj_decode_nest_state current;
    char *current_string;
    struct json_object *string_jobj;
    size_t string_offset;
    bool ldv_is_bytes;
    uint16_t b64_bits;
    int b64_bpos;
};

static void *_new_jx_decode_ctx(size_t ctx_size)
{
    void *ctx = malloc(ctx_size);
    if (ctx) {
        memset(ctx, 0, ctx_size);
    }

    return ctx;
}

void *new_jobj_decode_ctx()
{
    return _new_jx_decode_ctx(sizeof(struct jobj_decode_state));
}

void free_jobj_decode_ctx(void *ctx)
{
    free(ctx);
}

void init_jx_decode_ctx(void *ctx, struct json_object *top_jobj,
        struct message_type *msg_type)
{
    struct jobj_decode_state *state = (struct jobj_decode_state *)ctx;
    state->current.jobj = top_jobj;
    state->current.msg_type = msg_type;
}

// shared logic between jobj_decode and jstream_decode

static int jx_decode_field(int nr, enum protobuf_wire_type wire_type,
        uint64_t value, struct jobj_decode_state *state,
        struct field_type **pfld_type, struct json_object **pfld_jobj)
{
    struct message_type *msg_type = state->current.msg_type;

    struct field_type *fld_type;
    fld_type = avl_find_element(&msg_type->fields_by_number, (void *)(long)nr,
            fld_type, number_node);
    if (!fld_type) {
        // skip unknown field
        return FIELD_ACTION_SKIP;
    }

    if (fld_type->type_name && !fld_type->type) {
        // type is allowed to be unset if type name is set, resolve to figure
        // out what it is
        if (resolve_msg_type_noerr(&fld_type->type_name, msg_type->name)) {
            fld_type->type = PB_TYPE_MESSAGE;
        } else if (resolve_enm_type_noerr(&fld_type->type_name, msg_type->name)) {
            fld_type->type = PB_TYPE_ENUM;
        } else {
            fprintf(stderr, "Unknown type '%s' within '%s'\n",
                    fld_type->type_name, msg_type->name);
            return FIELD_ACTION_SKIP;
        }
    }

    const struct field_xcode_ops *ops = &xcode_ops[fld_type->type];

    // Look up enum value, switch to int if necessary
    const char *enum_val_str = NULL;
    if (fld_type->type == PB_TYPE_ENUM) {
        struct enum_type *enm_type = resolve_enm_type(&fld_type->type_name,
                msg_type->name);
        if (enm_type) {
            struct enum_value_type *val_type;
            val_type = avl_find_element(&enm_type->values_by_number,
                    (void *)(long)value, val_type, number_node);
            if (val_type) {
                enum_val_str = val_type->name;
            }
        }
        if (!enum_val_str) {
            ops = &xcode_ops[PB_TYPE_INT32];
        }
    }

    if (wire_type != ops->wire_type) {
        if (wire_type == PB_WIRE_TYPE_LEN) {
            // packed repeated scalar
            switch (ops->wire_type) {
                case PB_WIRE_TYPE_VARINT:
                    return FIELD_ACTION_PARSE_REPEATED_VARINT;
                case PB_WIRE_TYPE_I64:
                    return FIELD_ACTION_PARSE_REPEATED_I64;
                case PB_WIRE_TYPE_I32:
                    return FIELD_ACTION_PARSE_REPEATED_I32;
                default:
                    break;
            }
        }
        // type mismatch: unexpected wire type
        return ERROR_DECODE;
    }

    if (ops->wire_type == PB_WIRE_TYPE_VARINT) {
        ops->varint.un_xform(&value);
    }

    int rv = FIELD_ACTION_CONSUME;
    struct json_object *fld_jobj;
    switch (ops->json_type) {
        case json_type_boolean:
            fld_jobj = json_object_new_boolean((json_bool)value);
            break;
        case json_type_double:
            {
                double dvalue;
                if (fld_type->type == PB_TYPE_FLOAT) {
                    uint32_t value32 = (uint32_t)value;
                    float fvalue;
                    memcpy(&fvalue, &value32, sizeof(float));
                    dvalue = (double)fvalue;
                } else {
                    memcpy(&dvalue, &value, sizeof(double));
                }
                fld_jobj = json_object_new_double(dvalue);
            }
            break;
        case json_type_int:
            // Per proto 3 Language Guide, 32 bit values that are out of
            // range get truncated. zigzag decoding is truncated before
            // decoding, that is handled elsewhere.
            if (ops->is_signed) {
                int64_t svalue = (int64_t)value;
                if (ops->is_32bit) {
                    svalue = (int64_t)(int32_t)svalue;
                }
                fld_jobj = json_object_new_int64(svalue);
            } else {
                if (ops->is_32bit) {
                    value = (uint64_t)(uint32_t)value;
                }
                fld_jobj = json_object_new_uint64(value);
            }
            break;
        case json_type_object:
            fld_jobj = json_object_new_object();
            break;
        case json_type_string:
            if (fld_type->type == PB_TYPE_ENUM) {
                fld_jobj = json_object_new_string(enum_val_str);
            } else {
                state->current_string = malloc(ops->ldv.decode_len((int)value));
                if (!state->current_string) {
                    return ERROR_ALLOCATION;
                }

                fld_jobj = json_object_new_string("");
                state->string_jobj = fld_jobj;
                state->string_offset = 0;
                state->b64_bits = 0;
                state->b64_bpos = 0;
                state->ldv_is_bytes = fld_type->type == PB_TYPE_BYTES;

                rv = FIELD_ACTION_PARSE_BYTES;
            }
            break;
        default:
            // null or array, which should not happen
            return ERROR_INTERNAL(5);
    }

    if (!fld_jobj) {
        return ERROR_ALLOCATION;
    }

    *pfld_type = fld_type;
    *pfld_jobj = fld_jobj;
    return rv;
}

static int jx_nest_message(struct json_object *fld_jobj,
        struct field_type *fld_type, struct jobj_decode_state *state)
{
    struct jobj_decode_nest_state *nest = malloc(sizeof(struct jobj_decode_nest_state));
    if (!nest) {
        return ERROR_ALLOCATION;
    }

    *nest = state->current;
    json_object_set_userdata(fld_jobj, nest, json_object_free_userdata);

    struct message_type *msg_type = state->current.msg_type;
    struct message_type *fld_msg_type = resolve_msg_type(&fld_type->type_name, msg_type->name);
    if (!fld_msg_type) {
        return ERROR_REPORTED;
    }
    state->current.jobj = fld_jobj;
    state->current.msg_type = fld_msg_type;

    return FIELD_ACTION_PARSE_MESSAGE;
}

static int jobj_decode_field(int nr, enum protobuf_wire_type wire_type,
        uint64_t value, void *ctx)
{
    struct jobj_decode_state *state = (struct jobj_decode_state *)ctx;

    struct field_type *fld_type;
    struct json_object *fld_jobj;
    int rv = jx_decode_field(nr, wire_type, value, state, &fld_type, &fld_jobj);
    if (rv != FIELD_ACTION_CONSUME && rv != FIELD_ACTION_PARSE_BYTES) {
        return rv;
    }

    int json_rv;
    struct json_object *jobj = state->current.jobj;
    if (fld_type->repeated) {
        struct json_object *arr_jobj;
        if (!json_object_object_get_ex(jobj, fld_type->name, &arr_jobj)) {
            arr_jobj = json_object_new_array();
            if (!arr_jobj) {
                json_object_put(fld_jobj);
                return ERROR_ALLOCATION;
            }
            json_rv = json_object_object_add_ex(jobj, fld_type->name, arr_jobj,
                    JSON_C_OBJECT_ADD_CONSTANT_KEY | JSON_C_OBJECT_ADD_KEY_IS_NEW);
            if (json_rv) {
                json_object_put(arr_jobj);
                json_object_put(fld_jobj);
                return ERROR_ALLOCATION;
            }
        }
        if (json_object_array_add(arr_jobj, fld_jobj)) {
            json_object_put(fld_jobj);
            return ERROR_ALLOCATION;
        }
    } else {
        json_rv = json_object_object_add_ex(jobj, fld_type->name, fld_jobj,
                JSON_C_OBJECT_ADD_CONSTANT_KEY);
        if (json_rv) {
            json_object_put(fld_jobj);
            return ERROR_ALLOCATION;
        }
    }

    if (fld_type->type == PB_TYPE_MESSAGE) {
        rv = jx_nest_message(fld_jobj, fld_type, state);
    }

    return rv;
}

static char b64_char(int val)
{
    return b64_chars[val];
}

static int jobj_decode_ldv_bytes(const uint8_t *buf, int len, bool last, void *ctx)
{
    struct jobj_decode_state *state = (struct jobj_decode_state *)ctx;

    if (state->ldv_is_bytes) {
        int off = state->string_offset;
        uint16_t bits = state->b64_bits;
        int bpos = state->b64_bpos;
        while (len--) {
            bits |= *buf++ << (8 - bpos);
            bpos += 8;
            do {
                state->current_string[off++] = b64_char((int)(bits >> 10));
                bits <<= 6;
                bpos -= 6;
            } while (bpos >= 6);
        }
        if (last) {
            if (bpos) {
                state->current_string[off++] = b64_char((int)(bits >> 10));
            }
            while (off % 4) {
                state->current_string[off++] = '=';
            }
        }
        state->b64_bits = bits;
        state->b64_bpos = bpos;
        state->string_offset = off;
    } else {
        memcpy(state->current_string + state->string_offset, buf, len);
        state->string_offset += len;
    }

    if (last) {
        json_object_set_string_len(state->string_jobj, state->current_string, state->string_offset);
        free(state->current_string);
        state->string_jobj = NULL;
    }

    return 0;
}

static int jobj_decode_end_message(void *ctx)
{
    struct jobj_decode_state *state = (struct jobj_decode_state *)ctx;
    struct json_object *jobj = state->current.jobj;

    struct jobj_decode_nest_state *nest = json_object_get_userdata(jobj);
    if (nest) {
        state->current = *nest;
        json_object_set_userdata(jobj, NULL, NULL);
    }

    return 0;
}

const struct decode_ops jobj_decode_ops = {
    jobj_decode_field,
    jobj_decode_ldv_bytes,
    jobj_decode_end_message
};

////////////////////////////////////////

struct jstream_decode_state {
    struct jobj_decode_state jobj_state;    // must be first
    struct json_object *deferred_jobj;
    bool multi_field;
    bool mishandled_duplicate;
};

void *new_jstream_decode_ctx()
{
    return _new_jx_decode_ctx(sizeof(struct jstream_decode_state));
}

void free_jstream_decode_ctx(void *ctx)
{
    free(ctx);
}

bool jstream_mishandled_duplicate(void *ctx)
{
    struct jstream_decode_state *state = (struct jstream_decode_state *)ctx;
    return state->mishandled_duplicate;
}

// write JSON string from jobj, then unconditionally deref it
static int write_jobj_str(struct json_object *jobj)
{
    int rv;
    size_t len;
    const char *str = json_object_to_json_string_length(jobj,
            JSON_C_TO_STRING_NOSLASHESCAPE, &len);
    if (str) {
        fwrite(str, 1, len, stdout);
        rv = 0;
    } else {
        rv = ERROR_ALLOCATION;
    }
    json_object_put(jobj);
    return rv;
}

static int jstream_decode_field(int nr, enum protobuf_wire_type wire_type,
        uint64_t value, void *ctx)
{
    struct jstream_decode_state *state = (struct jstream_decode_state *)ctx;

    struct field_type *fld_type;
    struct json_object *fld_jobj;
    int rv = jx_decode_field(nr, wire_type, value,&state->jobj_state,
            &fld_type, &fld_jobj);
    if (rv != FIELD_ACTION_CONSUME && rv != FIELD_ACTION_PARSE_BYTES) {
        return rv;
    }

    bool duplicate_field = false;
    struct json_object *jobj = state->jobj_state.current.jobj;
    if (!fld_type->repeated) {
        struct json_object *old_jobj;
        if (json_object_object_get_ex(jobj, fld_type->name, &old_jobj)) {
            duplicate_field = true;
        } else {
            int json_rv = json_object_object_add_ex(jobj, fld_type->name, NULL,
                    JSON_C_OBJECT_ADD_CONSTANT_KEY | JSON_C_OBJECT_ADD_KEY_IS_NEW);
            if (json_rv) {
                return ERROR_ALLOCATION;
            }
        }
    }

    bool mishandled_duplicate = duplicate_field &&
        (nr != state->jobj_state.current.prior_field_nr || !state->deferred_jobj);

    // improperly repeated fields will typically be contiguous, in which
    // case, just drop prior one if possible and keep last one as per wire
    // format documentation...
    if (state->deferred_jobj) {
        if (nr != state->jobj_state.current.prior_field_nr) {
            int write_rv = write_jobj_str(state->deferred_jobj);
            if (write_rv) {
                return write_rv;
            }
        } else {
            json_object_put(state->deferred_jobj);
        }
        state->deferred_jobj = NULL;
    }

    // ... but if not, drop subsequent ones and mark as incorrectly handled
    if (mishandled_duplicate) {
        state->mishandled_duplicate = true;

        // clean up state set up by jx_decode
        if (rv == FIELD_ACTION_PARSE_BYTES) {
            free(state->jobj_state.current_string);
            state->jobj_state.string_jobj = NULL;
        }

        json_object_put(fld_jobj);
        return FIELD_ACTION_SKIP;
    }

    if (state->jobj_state.current.decoding_array &&
            nr != state->jobj_state.current.prior_field_nr) {
        fprintf(stdout, "]");
        state->jobj_state.current.decoding_array = false;
    }
    state->jobj_state.current.prior_field_nr = nr;

    bool comma = state->multi_field;
    if (!state->multi_field) {
        fprintf(stdout, "{");
        state->multi_field = true;
    }
    bool bracket = false;
    const char *field_name = fld_type->name;

    if (fld_type->repeated) {
        if (state->jobj_state.current.decoding_array) {
            comma = true;
            field_name = NULL;
        } else {
            bracket = true;
            state->jobj_state.current.decoding_array = true;
        }
    } else {
        if (fld_type->type != PB_TYPE_MESSAGE) {
            // defer emitting value in case duplicate immediately follows
            state->deferred_jobj = fld_jobj;
        }
        if (duplicate_field) {
            comma = false;
            field_name = NULL;
        }
    }

    if (comma) {
        fprintf(stdout, ",");
    }

    if (field_name) {
        fprintf(stdout, "\"%s\":", field_name);
    }

    if (bracket) {
        fprintf(stdout, "[");
    }

    if (fld_type->type == PB_TYPE_MESSAGE) {
        rv = jx_nest_message(fld_jobj, fld_type, &state->jobj_state);

        state->multi_field = false;
        state->jobj_state.current.decoding_array = false;
        state->jobj_state.current.prior_field_nr = 0;
    } else if (fld_type->repeated && rv != FIELD_ACTION_PARSE_BYTES) {
        int write_rv = write_jobj_str(fld_jobj);
        if (write_rv) {
            return write_rv;
        }
    }

    return rv;
}

static int jstream_decode_ldv_bytes(const uint8_t *buf, int len, bool last, void *ctx)
{
    struct jstream_decode_state *state = (struct jstream_decode_state *)ctx;
    struct json_object *string_jobj = state->jobj_state.string_jobj;

    int rv = jobj_decode_ldv_bytes(buf, len, last, &state->jobj_state);

    bool emit_value = last && string_jobj != state->deferred_jobj;
    if (!rv && emit_value) {
        rv = write_jobj_str(string_jobj);
    } else if (rv) {
        json_object_put(string_jobj);
        free(state->jobj_state.current_string);
        state->jobj_state.string_jobj = NULL;
    }

    return rv;
}

static int jstream_decode_end_message(void *ctx)
{
    int rv;
    struct jstream_decode_state *state = (struct jstream_decode_state *)ctx;

    if (state->deferred_jobj) {
        rv = write_jobj_str(state->deferred_jobj);
        if (rv) {
            return rv;
        }
        state->deferred_jobj = NULL;
    }

    if (state->jobj_state.current.decoding_array) {
        fprintf(stdout, "]");
        state->jobj_state.current.decoding_array = false;
    }

    struct json_object *jobj = state->jobj_state.current.jobj;
    if (!json_object_get_userdata(jobj)) {
        // top-level message needs to keep reference alive
        jobj = NULL;
    }
    // this will pop the message nest state
    rv = jobj_decode_end_message(&state->jobj_state);
    if (jobj) {
        json_object_put(jobj);
    }

    if (!rv) {
        if (!state->multi_field) {
            fprintf(stdout, "{");
        }
        fprintf(stdout, "}");
        state->multi_field = true;
    }

    return rv;
}

const struct decode_ops jstream_decode_ops = {
    jstream_decode_field,
    jstream_decode_ldv_bytes,
    jstream_decode_end_message
};

////////////////////////////////////////

struct rr_decode_state {
    struct jobj_decode_state jobj_state;    // must be first
    struct pb_decode_info *fd_info;
    void *fd_ctx;
    struct output_buffer *fds_outbuf;
    bool decoding_fd;
};

void *new_rr_decode_ctx(struct output_buffer *fds_outbuf)
{
    struct rr_decode_state *state =
        _new_jx_decode_ctx(sizeof(struct rr_decode_state));
    if (state) {
        state->fds_outbuf = fds_outbuf;
    }

    return state;
}

void free_rr_decode_ctx(void *ctx)
{
    struct rr_decode_state *state = (struct rr_decode_state *)ctx;

    free(state->fd_info);
    free(state->fd_ctx);
    free(state);
}

static int rr_decode_field(int nr, enum protobuf_wire_type wire_type,
        uint64_t value, void *ctx)
{
    struct rr_decode_state *state = (struct rr_decode_state *)ctx;

    if (nr == 1 &&  // FileDescriptorResponse.file_descriptor_proto
            wire_type == PB_WIRE_TYPE_LEN && !strcmp(
                "@reflection_v1.FileDescriptorResponse",
                state->jobj_state.current.msg_type->name)) {
        // value is the bytes len, and has already been bounds checked
        state->fd_info = new_parse_info((int)value);
        if (!state->fd_info) {
            return ERROR_ALLOCATION;
        }

        state->fd_ctx = new_fd_decode_ctx();
        if (!state->fd_ctx) {
            free_parse_info(state->fd_info);
            state->fd_info = NULL;
            return ERROR_ALLOCATION;
        }

        if (state->fds_outbuf) {
            int rv = encode_tag(1 /* FileDescriptorProto.file */,
                    PB_WIRE_TYPE_LEN, state->fds_outbuf);

            if (!rv) {
                rv = encode_varint(value, NULL, state->fds_outbuf);
            }

            if (rv) {
                free(state->fd_ctx);
                state->fd_ctx = NULL;
                free_parse_info(state->fd_info);
                state->fd_info = NULL;
                return rv;
            }
        }

        state->decoding_fd = true;

        return FIELD_ACTION_PARSE_BYTES;
    }

    return jobj_decode_field(nr, wire_type, value, &state->jobj_state);
}

static int rr_decode_ldv_bytes(const uint8_t *buf, int len, bool last, void *ctx)
{
    struct rr_decode_state *state = (struct rr_decode_state *)ctx;

    if (state->decoding_fd) {
        int rv = 0;
        if (state->fds_outbuf) {
            rv = encode_string((const char *)buf, len, state->fds_outbuf);
            if (rv) {
                fprintf(stderr, "Error writing FileDescptorSet file\n");
            }
        }

        if (!rv) {
            rv = pb_decode_chunk(state->fd_info, buf, len, &fd_decode_ops,
                    state->fd_ctx);
            if (rv) {
                fprintf(stderr, "FD message decode error\n");
            }
        }

        if (last || rv) {
            free_parse_info(state->fd_info);
            state->fd_info = NULL;
            free(state->fd_ctx);
            state->fd_ctx = NULL;
            state->decoding_fd = false;
            if (state->fds_outbuf) {
                flush_outbuf(state->fds_outbuf);
            }
        }

        return rv;
    }

    return jobj_decode_ldv_bytes(buf, len, last, &state->jobj_state);
}

const struct decode_ops rr_decode_ops = {
    rr_decode_field,
    rr_decode_ldv_bytes,
    jobj_decode_end_message
};


#ifndef _UGRPC_JOBJ_XCODE_H
#define _UGRPC_JOBJ_XCODE_H

#include "pb_decode.h"
#include <stdbool.h>
#include <stddef.h>

struct message_type;
struct json_object;

struct output_buffer;
typedef int (*flush_fn)(struct output_buffer *outbuf, void *arg);

struct output_buffer {
    uint8_t *current;
    uint8_t *end;
    flush_fn flush;
    void *flush_arg;
};

// for use with pb_decode_chunk
extern const struct decode_ops jobj_decode_ops;
extern const struct decode_ops jstream_decode_ops;
extern const struct decode_ops rr_decode_ops;

// for these, must call both new func and init_jx func
void *new_jobj_decode_ctx();
void free_jobj_decode_ctx(void *ctx);
void *new_jstream_decode_ctx();
void free_jstream_decode_ctx(void *ctx);
void *new_rr_decode_ctx(struct output_buffer *fds_outbuf);
void free_rr_decode_ctx(void *ctx);
void init_jx_decode_ctx(void *ctx, struct json_object *top_jobj,
        struct message_type *msg_type);
bool jstream_mishandled_duplicate(void *ctx);

struct output_buffer *new_outbuf(size_t size, flush_fn flush, void *flush_arg);
int jobj_encode_msg(struct json_object *top_jobj,
        struct message_type *msg_type, struct output_buffer *outbuf);
int jstr_encode_msg(const char* json_str, struct message_type *msg_type,
        struct output_buffer *outbuf);

#endif /* _UGRPC_JOBJ_XCODE_H */

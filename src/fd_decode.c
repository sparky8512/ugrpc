//
// support for decoding serialized FileDescriptorProto and FileDescriptorSet
// messages into internal ugrpc protocol symbol types
//

#include "error.h"
#include "fd_decode.h"
#include "pb_decode.h"
#include "symbol_db.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/list.h>
#include <libubox/avl.h>
#include <libubox/avl-cmp.h>

struct service_type {
    struct list_head list;

    struct list_head methods;
    char *name;
};

struct fd_decode_state {
    struct list_head messages;
    struct list_head enums;
    struct list_head services;
    struct message_type *current_message;
    struct field_type *current_field;
    struct enum_type *current_enum;
    struct enum_value_type *current_enum_value;
    struct service_type *current_service;
    struct method_type *current_method;
    char *current_string;
    char *package_name;
};

static void *_new_fd_decode_ctx(size_t ctx_size)
{
    // fd_decode_state is always at start of ctx struct 
    struct fd_decode_state *state = malloc(ctx_size);
    if (state) {
        memset(state, 0, ctx_size);
        INIT_LIST_HEAD(&state->messages);
        INIT_LIST_HEAD(&state->enums);
        INIT_LIST_HEAD(&state->services);
    }

    return state;
}

void *new_fd_decode_ctx(void)
{
    return _new_fd_decode_ctx(sizeof(struct fd_decode_state));
}

static int cmp_number(const void *k1, const void *k2, void *ptr)
{
    int nr_1 = (int)(long)k1;
    int nr_2 = (int)(long)k2;

    return nr_1 > nr_2 ? 1 : nr_2 > nr_1 ? -1 : 0;
}

static int new_string(char **pstr, char **pstr2, enum protobuf_wire_type wire_type,
        size_t len)
{
    if (wire_type != PB_WIRE_TYPE_LEN) {
        return ERROR_DECODE;
    }

    char *str = *pstr;
    free(str);
    str = malloc(len + 1);
    *pstr = str;
    if (!str) {
        return ERROR_ALLOCATION;
    }

    *pstr2 = str;
    return FIELD_ACTION_PARSE_BYTES;
}

static int new_symbol_struct(void** pptr, enum protobuf_wire_type wire_type,
        size_t size)
{
    if (wire_type != PB_WIRE_TYPE_LEN) {
        return ERROR_DECODE;
    }

    void *ptr = malloc(size);
    if (!ptr) {
        return ERROR_ALLOCATION;
    }
    memset(ptr, 0, size);

    *pptr = ptr;
    return FIELD_ACTION_PARSE_MESSAGE;
}

static int new_message(struct message_type **pmsg, struct message_type *parent,
        enum protobuf_wire_type wire_type)
{
    int rv = new_symbol_struct((void **)pmsg, wire_type, sizeof(struct message_type));

    if (rv >= 0) {
        struct message_type *msg = *pmsg;
        avl_init(&msg->fields_by_name, avl_strcmp, false, NULL);
        avl_init(&msg->fields_by_number, cmp_number, false, NULL);
        msg->parent = parent;
    }

    return rv;
}

static int new_field(struct field_type **pfld, enum protobuf_wire_type wire_type)
{
    return new_symbol_struct((void **)pfld, wire_type, sizeof(struct field_type));
}

static int new_enum(struct enum_type **penm, struct message_type *parent,
        enum protobuf_wire_type wire_type)
{
    int rv = new_symbol_struct((void **)penm, wire_type, sizeof(struct enum_type));

    if (rv >= 0) {
        struct enum_type *enm = *penm;
        avl_init(&enm->values_by_name, avl_strcmp, false, NULL);
        avl_init(&enm->values_by_number, cmp_number, false, NULL);
        enm->parent = parent;
    }

    return rv;
}

static int new_enum_value(struct enum_value_type **pval, enum protobuf_wire_type wire_type)
{
    return new_symbol_struct((void **)pval, wire_type, sizeof(struct enum_value_type));
}

static int new_service(struct service_type **psvc, enum protobuf_wire_type wire_type)
{
    int rv = new_symbol_struct((void **)psvc, wire_type, sizeof(struct service_type));

    if (rv >= 0) {
        struct service_type *svc = *psvc;
        INIT_LIST_HEAD(&svc->methods);
    }

    return rv;
}

static int new_method(struct method_type **pmth, enum protobuf_wire_type wire_type)
{
    return new_symbol_struct((void **)pmth, wire_type, sizeof(struct method_type));
}

static int fd_decode_field(int nr, enum protobuf_wire_type wire_type, uint64_t value, void *ctx)
{
    struct fd_decode_state *state = (struct fd_decode_state *)ctx;

    struct enum_value_type *cur_val = state->current_enum_value;
    if (cur_val) {
        switch (nr) {
            case 1: // EnumValueDescriptorProto.name
                return new_string(&cur_val->name, &state->current_string, wire_type, (size_t)value);
            case 2: // EnumValueDescriptorProto.number
                {
                    int64_t svalue = (int64_t)value;
                    if (svalue < INT32_MIN || svalue > INT32_MAX) {
                        // mark invalid, not a int32
                        cur_val->number_node.key = (void *)1;
                    } else {
                        // just in case duplicate and prior one invalid
                        cur_val->number_node.key = NULL;
                        cur_val->number = (int)svalue;
                    }
                }
                return FIELD_ACTION_CONSUME;
            default:
                return FIELD_ACTION_SKIP;
        }
    }

    struct enum_type *cur_enm = state->current_enum;
    if (cur_enm) {
        switch (nr) {
            case 1: // EnumDescriptorProto.name
                return new_string(&cur_enm->name, &state->current_string, wire_type, (size_t)value);
            case 2: // EnumDescriptorProto.value
                return new_enum_value(&state->current_enum_value, wire_type);
            default:
                return FIELD_ACTION_SKIP;
        }
    }

    struct field_type *cur_fld = state->current_field;
    if (cur_fld) {
        switch (nr) {
            case 1: // FieldDescriptorProto.name
                return new_string(&cur_fld->name, &state->current_string, wire_type, (size_t)value);
            case 3: // FieldDescriptorProto.number
                if (value < 1 || value > 0x1fffffff) {
                    // invalid field number
                    cur_fld->number = 0;
                } else {
                    cur_fld->number = (int)value;
                }
                return FIELD_ACTION_CONSUME;
            case 5: // FieldDescriptorProto.type
                if (value >= (uint64_t)PB_TYPE_UNKNOWN) {
                    // this will be handled in end_of_message
                    cur_fld->type = PB_TYPE_UNKNOWN;
                } else {
                    cur_fld->type = (enum protobuf_field_type)value;
                }
                cur_fld->type = (enum protobuf_field_type)value;
                return FIELD_ACTION_CONSUME;
            case 6: // FieldDescriptorProto.type_name
                return new_string(&cur_fld->type_name, &state->current_string, wire_type, (size_t)value);
            case 4: // FieldDescriptorProto.label
                cur_fld->repeated = value == 3; // LABEL_REPEATED
                return FIELD_ACTION_CONSUME;
            default:
                return FIELD_ACTION_SKIP;
        }
    }

    struct message_type *cur_msg = state->current_message;
    if (cur_msg) {
        switch (nr) {
            case 1: // DescriptorProto.name
                return new_string(&cur_msg->name, &state->current_string, wire_type, (size_t)value);
            case 2: // DescriptorProto.field
                return new_field(&state->current_field, wire_type);
            case 3: // DescriptorProto.nested_type
                return new_message(&state->current_message, cur_msg, wire_type);
            case 4: // DescriptorProto.enum_type
                return new_enum(&state->current_enum, cur_msg, wire_type);
            case 8: // DescriptorProto.oneof_decl needed?
            default:
                return FIELD_ACTION_SKIP;
        }
    }

    struct method_type *cur_mth = state->current_method;
    if (cur_mth) {
        switch (nr) {
            case 1: // MethodDescriptorProto.name
                return new_string(&cur_mth->name, &state->current_string, wire_type, (size_t)value);
            case 2: // MethodDescriptorProto.input_type
                return new_string(&cur_mth->request_type_name, &state->current_string, wire_type, (size_t)value);
            case 3: // MethodDescriptorProto.output_type
                return new_string(&cur_mth->response_type_name, &state->current_string, wire_type, (size_t)value);
            case 5: // MethodDescriptorProto.client_streaming
                cur_mth->request_stream = value != 0;
                return FIELD_ACTION_CONSUME;
            case 6: // MethodDescriptorProto.server_streaming
                cur_mth->response_stream = value != 0;
                return FIELD_ACTION_CONSUME;
            default:
                return FIELD_ACTION_SKIP;
        }
    }

    struct service_type *cur_svc = state->current_service;
    if (cur_svc) {
        switch (nr) {
            case 1: // ServiceDescriptorProto.name
                return new_string(&cur_svc->name, &state->current_string, wire_type, (size_t)value);
            case 2: // ServiceDescriptorProto.method
                return new_method(&state->current_method, wire_type);
            default:
                return FIELD_ACTION_SKIP;
        }
    }

    switch (nr) {
        case 2: // FileDescriptorProto.package
            return new_string(&state->package_name, &state->current_string, wire_type, (size_t)value);
        case 4: // FileDescriptorProto.message_type
            return new_message(&state->current_message, NULL, wire_type);
        case 5: // FileDescriptorProto.enum_type
            return new_enum(&state->current_enum, NULL, wire_type);
        case 6: // FileDescriptorProto.service
            return new_service(&state->current_service, wire_type);
        default:
            return FIELD_ACTION_SKIP;
    }
}

static int fd_decode_ldv_bytes(const uint8_t *buf, int len, bool last, void *ctx)
{
    struct fd_decode_state *state = (struct fd_decode_state *)ctx;

    char *cur_str = state->current_string;
    if (!cur_str) {
        return ERROR_INTERNAL(3);
    }

    memcpy(cur_str, buf, (size_t)len);
    cur_str += len;
    if (last) {
        *cur_str = 0;
        cur_str = NULL;
    }

    state->current_string = cur_str;
    return 0;
}

static void free_enum_value(struct enum_value_type *val)
{
    free(val->name);
    free(val);
}

static void free_enum(struct enum_type *enm)
{
    free(enm->name);
    struct enum_value_type *val, *next_val;
    list_for_each_entry_safe(val, next_val, &enm->values_by_number.list_head, number_node.list) {
        list_del(&val->number_node.list);
        free_enum_value(val);
    }
    free(enm);
}

static void free_field(struct field_type *fld)
{
    free(fld->name);
    free(fld->type_name);
    free(fld);
}

static void free_message(struct message_type *msg)
{
    free(msg->name);
    struct field_type *fld, *next_fld;
    list_for_each_entry_safe(fld, next_fld, &msg->fields_by_number.list_head, number_node.list) {
        list_del(&fld->number_node.list);
        free_field(fld);
    }
    free(msg);
}

static void free_method(struct method_type *mth)
{
    free(mth->name);
    free(mth->request_type_name);
    free(mth->response_type_name);
    free(mth);
}

static void free_service(struct service_type *svc)
{
    free(svc->name);
    struct method_type *mth, *next_mth;
    list_for_each_entry_safe(mth, next_mth, &svc->methods, list) {
        list_del(&mth->list);
        free_method(mth);
    }
    free(svc);
}

static int extend_name(const char *str1, char **pstr2)
{
    char *str2 = *pstr2;
    char *new_name = malloc((str1 ? strlen(str1) + 1 : 0) + strlen(str2) + 1);
    if (!new_name) {
        return ERROR_ALLOCATION;
    }

    char *str = new_name;
    if (str1) {
        str = stpcpy(str, str1);
        *str++ = '.';
    }
    strcpy(str, str2);
    free(str2);

    *pstr2 = new_name;
    return 0;
}

static struct enum_type* move_enum_to_avl(struct enum_type *enm)
{
    struct enum_type *next_enm = list_next_entry(enm, list);
    list_del(&enm->list);

    struct enum_type *old_enm = add_enum(enm);
    if (old_enm) {
        free_enum(old_enm);
    }

    return next_enm;
}

static struct message_type* move_message_to_avl(struct message_type *msg)
{
    struct message_type *next_msg = list_next_entry(msg, list);
    list_del(&msg->list);

    struct message_type *old_msg = add_message(msg);
    if (old_msg) {
        free_message(old_msg);
    }

    return next_msg;
}

static struct method_type* move_method_to_avl(struct method_type *mth)
{
    struct method_type *next_mth = list_next_entry(mth, list);
    list_del(&mth->list);

    struct method_type *old_mth = add_method(mth);
    if (old_mth) {
        free_method(old_mth);
    }

    return next_mth;
}

static int fd_decode_end_message(void *ctx)
{
    struct fd_decode_state *state = (struct fd_decode_state *)ctx;

    struct enum_value_type *cur_val = state->current_enum_value;
    struct enum_type *cur_enm = state->current_enum;

    if (cur_val) {
        if (!cur_val->name || !cur_val->name[0] || cur_val->number_node.key) {
            // reject invalid value definition
            free_enum_value(cur_val);
            return ERROR_DECODE;
        }

        cur_val->name_node.key = cur_val->name;
        if (avl_insert(&cur_enm->values_by_name, &cur_val->name_node)) {
            // reject if duplicate name
            free_enum_value(cur_val);
            return ERROR_DECODE;
        }

        cur_val->number_node.key = (void *)(long)cur_val->number;
        // duplicate numbers are ok, so ignore failure, keep first:
        avl_insert(&cur_enm->values_by_number, &cur_val->number_node);

        if (!cur_enm->default_value) {
            cur_enm->default_value = cur_val;
        }

        state->current_enum_value = NULL;
        return 0;
    }

    if (cur_enm) {
        if (!cur_enm->default_value || !cur_enm->name || !cur_enm->name[0]) {
            // reject empty or nameless enum types
            free_enum(cur_enm);
            return ERROR_DECODE;
        }

        list_add_tail(&cur_enm->list, &state->enums);

        state->current_enum = NULL;
        return 0;
    }

    struct field_type *cur_fld = state->current_field;
    struct message_type *cur_msg = state->current_message;

    if (cur_fld) {
        if (!cur_fld->number || !cur_fld->name || !cur_fld->name[0] ||
                cur_fld->type == PB_TYPE_RESERVED ||
                (!cur_fld->type && (!cur_fld->type_name || !cur_fld->type_name[0]))) {
            // reject invalid field definition
            free_field(cur_fld);
            return ERROR_DECODE;
        }

        if (cur_fld->type == PB_TYPE_UNKNOWN) {
            // some new type we don't know how to handle?
            // ignore it, but don't flag as failure
            free_field(cur_fld);
            state->current_field = NULL;
            return 0;
        }

        cur_fld->name_node.key = cur_fld->name;
        if (avl_insert(&cur_msg->fields_by_name, &cur_fld->name_node)) {
            // reject if duplicate number or name
            free_field(cur_fld);
            return ERROR_DECODE;
        }

        cur_fld->number_node.key = (void *)(long)cur_fld->number;
        if (avl_insert(&cur_msg->fields_by_number, &cur_fld->number_node)) {
            avl_delete(&cur_msg->fields_by_name, &cur_fld->name_node);
            free_field(cur_fld);
            return ERROR_DECODE;
        }

        state->current_field = NULL;
        return 0;
    }

    if (cur_msg) {
        if (!cur_msg->name || !cur_msg->name[0]) {
            // reject nameless message types
            free_message(cur_msg);
            return ERROR_DECODE;
        }

        list_add_tail(&cur_msg->list, &state->messages);

        state->current_message = cur_msg->parent;
        return 0;
    }

    struct method_type *cur_mth = state->current_method;
    struct service_type *cur_svc = state->current_service;

    if (cur_mth) {
        if (!cur_mth->name || !cur_mth->name[0] ||
            !cur_mth->request_type_name || !cur_mth->request_type_name[0] ||
            !cur_mth->response_type_name || !cur_mth->response_type_name[0]) {
            // reject nameless or incomplete method types
            free_method(cur_mth);
            return ERROR_DECODE;
        }

        list_add_tail(&cur_mth->list, &cur_svc->methods);

        state->current_method = NULL;
        return 0;
    }

    if (cur_svc) {
        if (!cur_svc->name || !cur_svc->name[0]) {
            // reject nameless service types
            free_service(cur_svc);
            return ERROR_DECODE;
        }

        list_add_tail(&cur_svc->list, &state->services);

        state->current_service = NULL;
        return 0;
    }

    struct message_type *msg = list_first_entry(&state->messages, struct message_type, list);
    while (!list_entry_is_h(msg, &state->messages, list)) {
        const char* base_name = msg->parent ? msg->parent->name : state->package_name;
        int rv = extend_name(base_name, &msg->name);
        if (rv) {
            return rv;
        }

        msg = move_message_to_avl(msg);
    }

    struct enum_type *enm = list_first_entry(&state->enums, struct enum_type, list);
    while (!list_entry_is_h(enm, &state->enums, list)) {
        const char* base_name = enm->parent ? enm->parent->name : state->package_name;
        int rv = extend_name(base_name, &enm->name);
        if (rv) {
            return rv;
        }

        enm = move_enum_to_avl(enm);
    }

    struct service_type *svc, *next_svc;
    list_for_each_entry_safe(svc, next_svc, &state->services, list) {
        int rv = extend_name(state->package_name, &svc->name);
        if (rv) {
            return rv;
        }

        struct method_type *mth = list_first_entry(&svc->methods, struct method_type, list);
        while (!list_entry_is_h(mth, &svc->methods, list)) {
            rv = extend_name(svc->name, &mth->name);
            if (rv) {
                return rv;
            }

            mth = move_method_to_avl(mth);
        }

        list_del(&svc->list);
        free_service(svc);
    }

    free(state->package_name);
    state->package_name = NULL;

    return 0;
}

const struct decode_ops fd_decode_ops = {
    fd_decode_field,
    fd_decode_ldv_bytes,
    fd_decode_end_message
};

//
// FileDescriptorSet is just a repeated FileDescriptorProto, so the following
// mostly just wraps fd_decode_ops
//

struct fds_decode_state {
    struct fd_decode_state fd_state;
    bool decoding_fd;
};

void *new_fds_decode_ctx(void)
{
    return _new_fd_decode_ctx(sizeof(struct fds_decode_state));
}

static int fds_decode_field(int nr, enum protobuf_wire_type wire_type,
        uint64_t value, void *ctx)
{
    struct fds_decode_state *state = (struct fds_decode_state *)ctx;

    if (state->decoding_fd) {
        return fd_decode_field(nr, wire_type, value, ctx);
    }

    if (nr == 1 && wire_type == PB_WIRE_TYPE_LEN) {  // FileDescriptorSet.file
        state->decoding_fd = true;
        return FIELD_ACTION_PARSE_MESSAGE;
    }

    return FIELD_ACTION_SKIP;
}

static int fds_decode_end_message(void *ctx)
{
    struct fds_decode_state *state = (struct fds_decode_state *)ctx;

    if (state->decoding_fd) {
        if (!state->fd_state.current_message &&
                !state->fd_state.current_enum &&
                !state->fd_state.current_service) {
            state->decoding_fd = false;
        }
        return fd_decode_end_message(ctx);
    }

    return 0;
}

const struct decode_ops fds_decode_ops = {
    fds_decode_field,
    fd_decode_ldv_bytes,
    fds_decode_end_message
};

int decode_fd_bytes(const uint8_t *bytes, size_t len)
{
    struct pb_decode_info *info = new_parse_info((int)len);
    if (!info) {
        return ERROR_ALLOCATION;
    }

    void *ctx = new_fd_decode_ctx();
    if (!ctx) {
        goto out_free_info;
    }

    int rv = pb_decode_chunk(info, bytes, len, &fd_decode_ops, ctx);
    free(ctx);

out_free_info:
    free_parse_info(info);

    return rv;
}

#ifdef CLEANUP_SYMBOL_DB
void cleanup_fd_symbols(void)
{
    cleanup_symbols(free_message, free_enum, free_method);
}
#endif

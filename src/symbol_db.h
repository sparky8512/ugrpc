#ifndef _UGRPC_SYMBOL_DB_H
#define _UGRPC_SYMBOL_DB_H

#include <stdbool.h>
#include <libubox/list.h>
#include <libubox/avl.h>

// as specified in google/protobuf/descriptor.proto
enum protobuf_field_type {
    PB_TYPE_RESERVED = 0,
    PB_TYPE_DOUBLE = 1,
    PB_TYPE_FLOAT = 2,
    PB_TYPE_INT64 = 3,
    PB_TYPE_UINT64 = 4,
    PB_TYPE_INT32 = 5,
    PB_TYPE_FIXED64 = 6,
    PB_TYPE_FIXED32 = 7,
    PB_TYPE_BOOL = 8,
    PB_TYPE_STRING = 9,
    PB_TYPE_GROUP = 10,
    PB_TYPE_MESSAGE = 11,
    PB_TYPE_BYTES = 12,
    PB_TYPE_UINT32 = 13,
    PB_TYPE_ENUM = 14,
    PB_TYPE_SFIXED32 = 15,
    PB_TYPE_SFIXED64 = 16,
    PB_TYPE_SINT32 = 17,
    PB_TYPE_SINT64 = 18,
    PB_TYPE_UNKNOWN = 19
};

struct field_type {
    struct avl_node name_node;
    struct avl_node number_node;

    char *name;
    int number;
    bool repeated;
    enum protobuf_field_type type;
    char *type_name;
};

struct message_type {
    union {
        struct {
            struct list_head list;
            struct message_type *parent;
        };
        struct avl_node node;
    };

    char *name;
    struct avl_tree fields_by_name;
    struct avl_tree fields_by_number;
};

struct enum_value_type {
    struct avl_node name_node;
    struct avl_node number_node;

    char *name;
    int number;
};

struct enum_type {
    union {
        struct {
            struct list_head list;
            struct message_type *parent;
        };
        struct avl_node node;
    };

    char *name;
    struct avl_tree values_by_name;
    struct avl_tree values_by_number;
    struct enum_value_type *default_value;
};

struct method_type {
    union {
        struct list_head list;
        struct avl_node node;
    };

    char *name;
    char *request_type_name;
    char *response_type_name;
    bool request_stream;
    bool response_stream;
};

struct message_type* add_message(struct message_type *msg);
struct enum_type* add_enum(struct enum_type *enm);
struct method_type* add_method(struct method_type *mth);
struct message_type *resolve_fq_msg_type(const char *name);
struct message_type *resolve_msg_type_noerr(char **pname, const char *parent);
struct message_type *resolve_msg_type(char **pname, const char *parent);
struct enum_type *resolve_fq_enm_type(const char *name);
struct enum_type *resolve_enm_type_noerr(char **pname, const char *parent);
struct enum_type *resolve_enm_type(char **pname, const char *parent);
struct method_type* resolve_method_type(const char *name);

#ifdef CLEANUP_SYMBOL_DB
void cleanup_symbols(void (*free_message)(struct message_type *),
        void (*free_enum)(struct enum_type *),
        void (*free_method)(struct method_type *));
#endif

#endif /* _UGRPC_SYMBOL_DB_H */

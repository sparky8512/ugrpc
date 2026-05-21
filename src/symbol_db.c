//
// support for storing internal protocol symbol type data and looking them
// up by name
//

#include "symbol_db.h"

#include <float.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
 
#include <libubox/list.h>
#include <libubox/avl.h>
#include <libubox/avl-cmp.h>

static AVL_TREE(messages, avl_strcmp, false, NULL);
static AVL_TREE(enums, avl_strcmp, false, NULL);
static AVL_TREE(methods, avl_strcmp, false, NULL);

// returns prior message with same name, if any. caller is responsible for
// freeing it
struct message_type* add_message(struct message_type *msg)
{
    struct message_type *old_msg = avl_find_element(&messages, msg->name,
            (struct message_type *)NULL, node);
    if (old_msg) {
        avl_delete(&messages, &old_msg->node);
    }
    msg->node.key = msg->name;
    avl_insert(&messages, &msg->node);

    return old_msg;
}

// returns prior enum with same name, if any. caller is responsible for
// freeing it
struct enum_type* add_enum(struct enum_type *enm)
{
    struct enum_type *old_enm = avl_find_element(&enums, enm->name,
            (struct enum_type *)NULL, node);
    if (old_enm) {
        avl_delete(&enums, &old_enm->node);
    }
    enm->node.key = enm->name;
    avl_insert(&enums, &enm->node);

    return old_enm;
}

// returns prior method with same name, if any. caller is responsible for
// freeing it
struct method_type* add_method(struct method_type *mth)
{
    struct method_type *old_mth = avl_find_element(&methods, mth->name,
            (struct method_type *)NULL, node);
    if (old_mth) {
        avl_delete(&methods, &old_mth->node);
    }
    mth->node.key = mth->name;
    avl_insert(&methods, &mth->node);

    return old_mth;
}

typedef void * (*resolve_fn)(const char *name);

static void *resolve_non_fq_type(char **pname, const char *parent,
        resolve_fn resolve)
{
    char *name = *pname;
    size_t parent_len = strlen(parent) + 1;
    size_t name_len = strlen(name);
    char *strbuf = malloc(parent_len + name_len + 2);
    if (strbuf) {
        strbuf[0] = '.';
        char *parent_end = stpcpy(strbuf + 1, parent);
        *parent_end++ = '.';
        while (true) {
            strcpy(parent_end, name);
            void *resolved_type = resolve(strbuf + 1);
            if (resolved_type) {
                free(name);
                *pname = realloc(strbuf, strlen(strbuf) + 1);
                return *pname ? resolved_type : NULL;
            }
            strbuf[parent_len] = 0;
            parent_end = strrchr(strbuf, '.');
            if (!parent_end) {
                break;
            }
            parent_len = parent_end - strbuf;
            parent_end++;
        }
        free(strbuf);
    }

    return NULL;
}

static void *resolve_fq_msg_type_noerr(const char *name)
{
    struct message_type *msg_type;
    msg_type = avl_find_element(&messages, name, msg_type, node);
    return msg_type;
}

struct message_type *resolve_fq_msg_type(const char *name)
{
    struct message_type *msg_type =
        (struct message_type *)resolve_fq_msg_type_noerr(name);
    if (!msg_type) {
        fprintf(stderr, "Failed to resolve message type '%s'\n", name);
    }
    return msg_type;
}

struct message_type *resolve_msg_type_noerr(char **pname, const char *parent)
{
    const char *name = *pname;
    if (name[0] == '.') {
        return resolve_fq_msg_type_noerr(name + 1);
    }

    return (struct message_type *)
        resolve_non_fq_type(pname, parent, resolve_fq_msg_type_noerr);
}

struct message_type *resolve_msg_type(char **pname, const char *parent)
{
    struct message_type *msg = resolve_msg_type_noerr(pname, parent);
    if (!msg) {
        const char *name = *pname;
        if (name[0] == '.') {
            fprintf(stderr, "Failed to resolve message type '%s'\n", name);
        } else {
            fprintf(stderr, "Failed to resolve message type '%s' within '%s'\n",
                    name, parent);
        }
    }

    return msg;
}

static void *resolve_fq_enm_type_noerr(const char *name)
{
    struct enum_type *enm_type;
    enm_type = avl_find_element(&enums, name, enm_type, node);
    return enm_type;
}

struct enum_type *resolve_fq_enm_type(const char *name)
{
    struct enum_type *enm_type =
        (struct enum_type *)resolve_fq_enm_type_noerr(name);
    if (!enm_type) {
        fprintf(stderr, "Failed to resolve enum type '%s'\n", name);
    }
    return enm_type;
}

struct enum_type *resolve_enm_type_noerr(char **pname, const char *parent)
{
    const char *name = *pname;
    if (name[0] == '.') {
        return resolve_fq_enm_type_noerr(name + 1);
    }

    return (struct enum_type *)
        resolve_non_fq_type(pname, parent, resolve_fq_enm_type_noerr);
}

struct enum_type *resolve_enm_type(char **pname, const char *parent)
{
    struct enum_type *enm = resolve_enm_type_noerr(pname, parent);
    if (!enm) {
        const char *name = *pname;
        if (name[0] == '.') {
            fprintf(stderr, "Failed to resolve enum type '%s'\n", name);
        } else {
            fprintf(stderr, "Failed to resolve enum type '%s' within '%s'\n", name, parent);
        }
    }

    return enm;
}

struct method_type* resolve_method_type(const char *name)
{
    struct method_type *mth_type;
    mth_type = avl_find_element(&methods, name, mth_type, node);
    return mth_type;
}

#ifdef CLEANUP_SYMBOL_DB
void cleanup_symbols(void (*free_message)(struct message_type *),
        void (*free_enum)(struct enum_type *),
        void (*free_method)(struct method_type *))
{
    struct method_type *mth, *next_mth;
    list_for_each_entry_safe(mth, next_mth, &methods.list_head, node.list) {
        list_del(&mth->node.list);
        free_method(mth);
    }

    struct enum_type *enm, *next_enm;
    list_for_each_entry_safe(enm, next_enm, &enums.list_head, node.list) {
        list_del(&enm->node.list);
        free_enum(enm);
    }

    struct message_type *msg, *next_msg;
    list_for_each_entry_safe(msg, next_msg, &messages.list_head, node.list) {
        list_del(&msg->node.list);
        free_message(msg);
    }
}
#endif

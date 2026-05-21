#ifndef _UGRPC_DECODE_COMMON_H
#define _UGRPC_DECODE_COMMON_H

#include <stdbool.h>
#include <stdint.h>

struct json_object;
struct ugrpc_options;

// miscellaneous defs and funcs shared between decode and service commands

#define MAX_READ 2048
// spec max message size is 2GB-1, impose a lower max to avoid OOM
#define MAX_MESSAGE_SIZE (64 * 1024 * 1024)
#define MAX_TRAILERS_LEN 4096

int32_t get_grpc_status(char *status, char *message, bool suppress_err);
// returns grpc status code, or -1 if none specified
int32_t parse_trailers(char *trailers_str, bool suppress_err);
int check_reflection_response(struct json_object *jobj);
int print_jobj_result(struct json_object *jobj, struct ugrpc_options *opts);
void check_jstream_mishandled(void *ctx);

#endif /* _UGRPC_DECODE_COMMON_H */

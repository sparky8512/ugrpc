#include "error.h"
#include "jobj_xcode.h"
#include "options.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <json-c/json_object.h>

int32_t get_grpc_status(char *status, char *message, bool suppress_err)
{
    errno = 0;
    long code = strtoul(status, NULL, 10);
    if (errno || code < 0 || code > INT32_MAX) {
        return -1;
    }

    if (code && !suppress_err) {
        if (message) {
            fprintf(stderr, "GRPC error (%d): %s\n", (int)code, message);
        } else {
            fprintf(stderr, "GRPC error code %d\n", (int)code);
        }
    }

    return (int32_t)code;
}

int32_t parse_trailers(char *trailers_str, bool suppress_err)
{
    char *status_val = NULL;
    char *message_val = NULL;
    char *line = strtok(trailers_str, "\r\n");
    while (line) {
        char *colon = strchr(line, ':');
        if (colon) {
            size_t name_len = colon - line;
            char *value = colon + 1;
            while (isspace(*value)) {
                value++;
            }

            if (!strncasecmp("grpc-status", line, name_len)) {
                status_val = value;
            } else if (!strncasecmp("grpc-message", line, name_len)) {
                message_val = value;
            }
        }
        line = strtok(NULL, "\r\n");
    }

    return get_grpc_status(status_val, message_val, suppress_err);
}

int check_reflection_response(struct json_object *jobj)
{
    struct json_object *err_jobj = json_object_object_get(jobj, "error_response");
    if (err_jobj) {
        struct json_object *msg_jobj = json_object_object_get(err_jobj, "error_message");
        const char *str = json_object_get_string(msg_jobj ? msg_jobj : err_jobj);
        fprintf(stderr, "Reflection service error: %s\n", str);
        return ERROR_REPORTED;
    }

    return 0;
}

int print_jobj_result(struct json_object *jobj, struct ugrpc_options *opts)
{
    if (opts->light) {
        // for low-mem algo, it's already been printed
        fprintf(stdout, "\n");
    } else {
        int flags = opts->pretty ?
#ifdef USE_PRETTY_COLOR
            JSON_C_TO_STRING_COLOR |
#endif
            JSON_C_TO_STRING_NOSLASHESCAPE | JSON_C_TO_STRING_PRETTY :
            JSON_C_TO_STRING_NOSLASHESCAPE;
        size_t len;
        const char *str = json_object_to_json_string_length(jobj, flags, &len);
        if (str) {
            fwrite(str, 1, len, stdout);
            fprintf(stdout, "\n");
        } else {
            // a bit of an assumption, but that the only real reason the
            // json call would fail
            return ERROR_ALLOCATION;
        }
    }

    return 0;
}

void check_jstream_mishandled(void *ctx)
{
    if (jstream_mishandled_duplicate(ctx)) {
        fprintf(stderr, "\nWARNING: duplicate non-repeated field detected "
                "that was incorrectly decoded.\nRepeat without '-l' option "
                "to decode correctly.\n");
    }
}

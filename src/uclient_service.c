//
// support for the "service" and "reflect" commands using libuclient
//

#include "error.h"
#include "decode_common.h"
#include "jobj_xcode.h"
#include "options.h"
#include "pb_decode.h"
#include "reflection_v1_fd.h"
#include "symbol_db.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blob.h>
#include <libubox/blobmsg.h>
#include <libubox/uclient.h>
#include <libubox/uloop.h>
#include <json-c/json_object.h>

#define MAX_REDIRECTS 20

struct service_state {
    struct json_object *jobj;
    struct message_type *response_type;
    struct pb_decode_info *pb_info;
    const struct decode_ops *decode_ops;
    void *decode_ctx;
    struct ugrpc_options *opts;
    const char *reflect_sym;
    const char *method_name;
    const char *host;
    const char *json_str;
    char *trailer_str;
    int trailer_off;
    int redirects;
    int error;
    int32_t grpc_status;
    int prefix_bytes_pending;
    int trailer_bytes_pending;
    int message_bytes_pending;
    uint8_t prefix_bytes[5];
    bool reflect_v1_failed;
};

static void ucl_finished(struct uclient *ucl)
{
    uclient_disconnect(ucl);
    uloop_end();
}

static const struct blobmsg_policy status_policy[2] = {
    { "grpc-status", BLOBMSG_TYPE_STRING },
    { "grpc-message", BLOBMSG_TYPE_STRING },
};

static void ucl_set_grpc_status(struct service_state *state, char *status,
        char *message)
{
    bool noerr = state->reflect_sym && !state->reflect_v1_failed;
    state->grpc_status = get_grpc_status(status, message, noerr);
}

static void ucl_parse_trailers(struct service_state *state)
{
    bool noerr = state->reflect_sym && !state->reflect_v1_failed;
    state->grpc_status = parse_trailers(state->trailer_str, noerr);
}

static void ucl_response_headers(struct uclient *ucl)
{
    struct service_state *state = (struct service_state *)ucl->priv;

    if (uclient_http_status_redirect(ucl)) {
        if (++state->redirects > MAX_REDIRECTS) {
            fprintf(stderr, "Too many HTTP redirects\n");
            goto err_out;
        }

        int rv = uclient_http_redirect(ucl);
        if (rv < 0) {
            fprintf(stderr, "HTTP redirect error\n");
            goto err_out;
        }

        if (rv)  {
            return;
        }

        // else didn't actually redirect, below will catch as error
    }

    // anything other than 200 is unexpected, but spec says to expect
    // brokenness, so be a little more flexible than that
    if (ucl->status_code != 200 && ucl->status_code != 204 &&
            ucl->status_code != 201) {
        fprintf(stderr, "Unexpected HTTP status: %d\n", ucl->status_code);
        goto err_out;
    }

    struct blob_attr *attrs[2];
    blobmsg_parse_attr(status_policy, 2, attrs, ucl->meta);
    if (!attrs[0]) {
        return;
    }

    ucl_set_grpc_status(state, blobmsg_get_string(attrs[0]), attrs[1] ?
            blobmsg_get_string(attrs[1]) : NULL);
    return;

err_out:
    state->error = ERROR_REPORTED;
    ucl_finished(ucl);
}

static const char *resolve_types(const char *method_name,
        struct message_type **prequest_msg_type,
        struct message_type **presponse_msg_type)
{
    struct method_type* method = resolve_method_type(method_name);
    if (!method) {
        return method_name;
    }

    //
    // If the message type names are not fully qualified and reflecting on the
    // method name did not get it, then attempt to reflect on the message type
    // names will likely fail, but they really should have come along with the
    // method descriptor, so it's not worth trying to deal it any better.
    //

    *prequest_msg_type = resolve_msg_type(&method->request_type_name,
            method->name);
    if (!*prequest_msg_type) {
        return method->request_type_name;
    }

    *presponse_msg_type = resolve_msg_type(
            &method->response_type_name, method->name);
    if (!*presponse_msg_type) {
        return method->response_type_name;
    }

    return NULL;
}

static struct json_object *prepare_reflect(struct service_state *state, const char *symbol, struct message_type **prequest_type)
{
    state->reflect_sym = symbol;
    if (resolve_types( "@reflection_v1.ServerReflection.ServerReflectionInfo",
                prequest_type, &state->response_type)) {
        // shouldn't happen
        return NULL;
    }

    struct json_object *str_jobj = json_object_new_string(symbol);
    if (!str_jobj) {
        return NULL;
    }

    struct json_object *jobj = json_object_new_object();
    if (jobj && !json_object_object_add_ex(jobj, "file_containing_symbol",
                str_jobj, JSON_C_OBJECT_ADD_CONSTANT_KEY |
                JSON_C_OBJECT_ADD_KEY_IS_NEW)) {
        return jobj;
    }

    json_object_put(str_jobj);
    return NULL;
}

static char *make_url(const char *host, const char *method_name)
{
    char *url = malloc(7 + strlen(host) + 1 + strlen(method_name) + 1);

    if (url) {
        char *str = stpcpy(url, "http://");
        str = stpcpy(str, host);
        *str++ = '/';
        strcpy(str, method_name);
        char *dot = strrchr(str, '.');
        if (dot) {
            *dot = '/';
        } else {
            free(url);
            url = NULL;
        }
    }

    return url;
}

static int uclient_writer(struct output_buffer *outbuf, void *arg)
{
    struct uclient *ucl = (struct uclient *)arg;
    char *start = (char *)outbuf + sizeof(struct output_buffer);

    int len = (int)((char *)outbuf->current - start);
    if (len) {
        if (uclient_write(ucl, start, len) < 0) {
            fprintf(stderr, "uclient write error\n");
            return ERROR_REPORTED;
        }
    }

    return 0;
}

static int send_data(struct uclient *ucl, struct json_object *jobj,
        const char *json_str, struct message_type *request_type)
{
    int rv;
    if (uclient_connect(ucl)) {
        fprintf(stderr, "uclient connect error\n");
        rv = ERROR_REPORTED;
        goto out_err;
    }

    struct output_buffer *outbuf = new_outbuf(MAX_READ, uclient_writer, ucl);
    if (!outbuf) {
        rv = ERROR_ALLOCATION;
        goto out_err;
    }

    if (jobj) {
        rv = jobj_encode_msg(jobj, request_type, outbuf);
    } else {
        rv = jstr_encode_msg(json_str, request_type, outbuf);
    }

    free(outbuf);

out_err:
    json_object_put(jobj);
    return rv;
}

static void free_decode_ctx(struct service_state *state)
{
    if (!state->decode_ctx) {
        return;
    }

    if (state->reflect_sym) {
        free_rr_decode_ctx(state->decode_ctx);
    } else if (state->opts->light) {
        free_jstream_decode_ctx(state->decode_ctx);
    } else {
        free_jobj_decode_ctx(state->decode_ctx);
    }

    state->decode_ctx = NULL;
}

static int prepare_receive(struct uclient *ucl, struct service_state *state)
{
    state->prefix_bytes_pending = 5;
    state->grpc_status = -1;

    if (state->reflect_sym) {
        state->decode_ctx = new_rr_decode_ctx(state->opts->fds_outbuf);
        state->decode_ops = &rr_decode_ops;
    } else if (state->opts->light) {
        state->decode_ctx = new_jstream_decode_ctx();
        state->decode_ops = &jstream_decode_ops;
    } else {
        state->decode_ctx = new_jobj_decode_ctx();
        state->decode_ops = &jobj_decode_ops;
    }
    if (!state->decode_ctx) {
        return ERROR_ALLOCATION;
    }

    if (uclient_request(ucl)) {
        fprintf(stderr, "Error submitting uclient request\n");
        free_decode_ctx(state);
        return ERROR_REPORTED;
    }

    return 0;
}

static void ucl_eof(struct uclient *ucl)
{
    struct service_state *state = (struct service_state *)ucl->priv;

    if (state->error) {
        goto done;
    }

    // check for unfinished processing
    if (state->message_bytes_pending) {
        fprintf(stderr, "Incomplete response message\n");
        state->error = ERROR_REPORTED;
        goto done;
    }
    if (state->trailer_bytes_pending) {
        fprintf(stderr, "Incomplete grpc trailers\n");
        state->error = ERROR_REPORTED;
        goto done;
    }
    if (state->prefix_bytes_pending != 5) {
        fprintf(stderr, "Incomplete grpc prefix\n");
        state->error = ERROR_REPORTED;
        goto done;
    }

    if (!state->reflect_sym) {
        goto done;
    }

    struct json_object *reflect_jobj = NULL;
    struct message_type *request_type;
    const char *method_name;
    const char *symbol;
    if (state->json_str) {
        symbol = resolve_types(state->method_name, &request_type,
                &state->response_type);
    } else {
        // reflection only
        if (state->grpc_status <= 0) {
            goto done;
        }

        symbol = state->method_name;
    }

    free_decode_ctx(state);

    if (symbol) {
        // need to reflect something, redirect to that service

        if (state->grpc_status && !state->reflect_v1_failed) {
            state->reflect_v1_failed = true;
        } else if (!strcmp(symbol, state->reflect_sym)) {
            fprintf(stderr, "Unable to resolve symbol %s via reflection\n",
                    symbol);
            state->error = ERROR_REPORTED;
            goto done;
        }

        reflect_jobj = prepare_reflect(state, symbol, &request_type);
        if (!reflect_jobj) {
            state->error = ERROR_REPORTED;
            goto done;
        }

        method_name = state->reflect_v1_failed ?
            "grpc.reflection.v1alpha.ServerReflection.ServerReflectionInfo" :
            "grpc.reflection.v1.ServerReflection.ServerReflectionInfo";
    } else {
        method_name = state->method_name;
        state->reflect_sym = NULL;
    }

    char *url = make_url(state->host, method_name);
    if (!url) {
        goto out_put;
    }

    int rv = uclient_set_url(ucl, url, NULL);
    free(url);
    if (rv) {
        fprintf(stderr, "uclient error %d\n", rv);
        state->error = ERROR_REPORTED;
        goto out_put;
    }

    rv = send_data(ucl, reflect_jobj, state->json_str, request_type);
    if (rv) {
        state->error = ERROR_REPORTED;
        goto done;
    }

    reflect_jobj = NULL;

    rv = prepare_receive(ucl, state);
    if (!rv) {
        return;
    }

out_put:
    json_object_put(reflect_jobj);
done:
    ucl_finished(ucl);
}

static void ucl_read(struct uclient *ucl)
{
    struct service_state *state = (struct service_state *)ucl->priv;

    while (!state->error) {
        uint8_t read_buf[128];

        int len = state->prefix_bytes_pending;
        bool new_prefix = len != 0;
        while (len) {
            int n = uclient_read(ucl, (char *)state->prefix_bytes + 5 - len,
                    len);
            if (n < 0) {
                fprintf(stderr, "uclient read error\n");
                state->error = ERROR_REPORTED;
                goto out_error;
            }
            if (n == 0) {
                return;
            }
            len -= n;
            state->prefix_bytes_pending = len;
        }

        if (new_prefix) {
            size_t tot_len = (size_t)state->prefix_bytes[1] << 24 |
                (size_t)state->prefix_bytes[2] << 16 |
                (size_t)state->prefix_bytes[3] << 8 |
                (size_t)state->prefix_bytes[4];

            if (tot_len > MAX_MESSAGE_SIZE) {
                fprintf(stderr, "Message size exceeded maximum (%d)\n",
                        MAX_MESSAGE_SIZE);
                state->error = ERROR_REPORTED;
                goto out_error;
            }

            if (state->prefix_bytes[0] == 0x80) {
                // grpc-web trailers
                state->message_bytes_pending = 0;
                state->trailer_bytes_pending = tot_len < MAX_TRAILERS_LEN - 1 ?
                    (int)tot_len : MAX_TRAILERS_LEN - 1;
                state->trailer_str = malloc(state->trailer_bytes_pending + 1);
                if (!state->trailer_str) {
                    state->error = ERROR_ALLOCATION;
                    goto out_error;
                }
                state->trailer_off = 0;
            } else if (state->prefix_bytes[0] == 0x00) {
                state->pb_info = new_parse_info((int)tot_len);
                if (!state->pb_info) {
                    state->error = ERROR_ALLOCATION;
                    goto out_error;
                }

                state->jobj = json_object_new_object();
                if (!state->jobj) {
                    free_parse_info(state->pb_info);
                    state->error = ERROR_ALLOCATION;
                    goto out_error;
                }

                init_jx_decode_ctx(state->decode_ctx, state->jobj,
                        state->response_type);

                state->message_bytes_pending = tot_len;
                state->trailer_bytes_pending = 0;
            } else {
                state->error = ERROR_DECODE;
                goto out_error;
            }
        }

        len = state->trailer_bytes_pending;
        while (len) {
            int n = uclient_read(ucl, state->trailer_str + state->trailer_off,
                    len);
            if (n < 0) {
                fprintf(stderr, "uclient read error\n");
                state->error = ERROR_REPORTED;
                goto out_error;
            }
            if (n == 0) {
                return;
            }
            len -= n;
            state->trailer_bytes_pending = len;
            state->trailer_off += n;

            if (len == 0) {
                *(state->trailer_str + state->trailer_off) = '\0';
                ucl_parse_trailers(state);
                free(state->trailer_str);
                state->trailer_str = NULL;
                //
                // trailers are always last and may not have read the whole
                // thing if exceeded MAX_TRAILERS_LEN
                //
                // uclient doesn't appear to have a way to abort reading data
                // while still getting the eof callback, so just call it
                // directly
                //
                state->prefix_bytes_pending = 5;
                ucl_eof(ucl);
                return;
            }
        }

        len = state->message_bytes_pending;
        while (len) {
            int read_len = len < (int)sizeof(read_buf) ? len :
                    (int)sizeof(read_buf);
            int n = uclient_read(ucl, (char *)read_buf, read_len);
            if (n < 0) {
                fprintf(stderr, "uclient read error\n");
                state->error = ERROR_REPORTED;
                goto out_error;
            }
            if (n == 0) {
                return;
            }

            if (pb_decode_chunk(state->pb_info, read_buf, (size_t)n,
                        state->decode_ops, state->decode_ctx) < 0) {
                fprintf(stderr, "Message decode error\n");
                state->error = ERROR_REPORTED;
                goto out_error;
            }

            len -= n;
            state->message_bytes_pending = len;

            if (!len) {
                free_parse_info(state->pb_info);

                int rv;
                if (state->reflect_sym) {
                    rv = check_reflection_response(state->jobj);
                    if (rv) {
                        state->error = rv;
                    }
                } else if ((rv = print_jobj_result(state->jobj, state->opts))) {
                    state->error = rv;
                } else if (state->opts->light) {
                    check_jstream_mishandled(state->decode_ctx);
                }

                json_object_put(state->jobj);
                state->jobj = NULL;
            }
        }

        state->prefix_bytes_pending = 5;
    }

out_error:
    ucl_finished(ucl);
}

static void ucl_error(struct uclient *ucl, int code)
{
    fprintf(stderr, "uclient error: %d\n", code);
    ucl_finished(ucl);
}

static const struct uclient_cb ucl_ops = {
    .data_read = ucl_read,
    .data_eof = ucl_eof,
    .header_done = ucl_response_headers,
    .error = ucl_error,
//    .log_msg = ucl_log,   // this is only used for SSL failures
};

static const char* headers[][2] = {
    { "Content-Type", "application/grpc-web+proto" },
    { "User-Agent", "libuclient/1.0"},
    { "X-User-Agent", "grpc-web-c-ugrpc/0.1"},
    { "X-Grpc-Web", "1" },
};

int uclient_service(const char *host, const char *meth_or_sym,
        const char *json_str, struct ugrpc_options *opts)
{
    struct service_state state;
    memset(&state, 0, sizeof(state));
    state.opts = opts;

    struct json_object *reflect_jobj = NULL;
    struct message_type *request_type;
    const char *reflect_sym;
    const char *method_name;
    if (json_str) {
        method_name = meth_or_sym;
        reflect_sym = resolve_types(meth_or_sym, &request_type,
                &state.response_type);
    } else {
        // reflection only, "meth_or_sym" is any symbol, not just method
        method_name = NULL;
        reflect_sym = meth_or_sym;
    }

    if (reflect_sym) {
        // redirect to the reflection service and save off the original params
        // so they can be used once reflection has completed
        if (opts->no_reflection) {
            fprintf(stderr, "Unknown symbol %s\n", reflect_sym);
            return ERROR_REPORTED;
        }

        int load_rv = load_reflection_v1();
        if (load_rv) {
            return load_rv;
        }

        reflect_jobj = prepare_reflect(&state, reflect_sym, &request_type);
        if (!reflect_jobj) {
            return ERROR_ALLOCATION;
        }

        state.method_name = method_name;
        state.host = host;
        state.json_str = json_str;
        method_name = "grpc.reflection.v1.ServerReflection.ServerReflectionInfo";
    }

    char *url = make_url(host, method_name);
    if (!url) {
        goto out_put;
    }

    struct uclient *ucl = uclient_new(url, NULL, &ucl_ops);
    free(url);
    if (!ucl) {
        goto out_put;
    }

    ucl->priv = &state;

    uloop_init();

    if (uclient_set_timeout(ucl, 10 * 1000)) {
        goto out_uloop;
    }

    if (uclient_http_set_request_type(ucl, "POST")) {
        goto out_uloop;
    }

    int i;
    for (i = 0; i < (int)(sizeof(headers) / sizeof(headers[0])); i++) {
        if (uclient_http_set_header(ucl, headers[i][0], headers[i][1])) {
            goto out_uloop;
        }
    }

    int rv = send_data(ucl, reflect_jobj, json_str, request_type);
    reflect_jobj = NULL;
    if (rv){
        goto out_uloop;
    }

    rv = prepare_receive(ucl, &state);
    if (rv) {
        goto out_uloop;
    }

    // receiving data and initiating subsequent requests happens
    // asynchronously in uloop callbacks
    uloop_run();

    if (state.error) {
        rv = state.error;
    } else if (state.grpc_status > 0) {
        rv = ERROR_REPORTED;
    }

    free_decode_ctx(&state);
    free(state.trailer_str);

out_uloop:
    uloop_done();
    uclient_free(ucl);

out_put:
    json_object_put(reflect_jobj);

    return rv;
}

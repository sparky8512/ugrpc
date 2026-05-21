#include "error.h"
#include "decode_common.h"
#include "fd_decode.h"
#include "jobj_xcode.h"
#include "options.h"
#include "pb_decode.h"
#include "reflection_v1_fd.h"
#include "symbol_db.h"
#ifdef UCLIENT_SERVICE
#include "uclient_service.h"
#endif

#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <json-c/json_object.h>

#define CMD_DECODE 0
#define CMD_ENCODE 1
#define CMD_REFLECT 2
#define CMD_SERVICE 3

static int decode_osfd(int fd, size_t tot_len, struct pb_decode_info *info, const struct decode_ops *decode_ops, void *ctx)
{
    if (tot_len > (size_t)MAX_MESSAGE_SIZE) {
        fprintf(stderr, "Message size exceeded maximum (%d)\n", MAX_MESSAGE_SIZE);
        return ERROR_REPORTED;
    }

    if (tot_len == 0) {
        // empty message at top level won't even enter following loop
        return decode_ops->end_message(ctx);
    }

    size_t tot_read = 0;
    while (tot_read < tot_len) {
        uint8_t read_buf[MAX_READ];
        ssize_t n = tot_len - tot_read;
        n = n < MAX_READ ? n : MAX_READ;
        n = read(fd, read_buf, n);
        if (n == -1) {
            perror("Error reading file");
            return ERROR_REPORTED;
        }
        if (n == 0) {
            fprintf(stderr, "Unexpected end of file\n");
            return ERROR_REPORTED;
        }
        int rv = pb_decode_chunk(info, read_buf, n, decode_ops, ctx);
        if (rv < 0) {
            return rv;
        }
        tot_read += n;
    }

    return 0;
}

// like read(), but always reads full length unless EOF, which it
// treats as error unless it happens at start of input
static ssize_t read_fully(int fd, void *buf, size_t len)
{
    size_t tot_read = 0;
    while (tot_read < len) {
        ssize_t n = read(fd, (uint8_t *)buf + tot_read, len - tot_read);
        if (n == -1) {
            perror("Error reading input");
            return -1;
        }
        if (n == 0) {
            if (!tot_read) {
                break;
            }
            fprintf(stderr, "Unexpected end of file\n");
            return -1;
        }
        tot_read += n;
    }

    return tot_read;
}

static int read_trailers(int fd, struct json_object **pjobj, size_t len)
{
    size_t read_len = len < MAX_TRAILERS_LEN - 1 ? len : MAX_TRAILERS_LEN - 1;
    uint8_t *read_buf = malloc(read_len + 1);

    if (!read_buf) {
        return ERROR_ALLOCATION;
    }

    ssize_t n = read_fully(fd, read_buf, read_len);
    if (n == -1) {
        free(read_buf);
        return ERROR_REPORTED;
    }

    read_buf[n] = '\0';
    int32_t grpc_status = parse_trailers((char *)read_buf, false);
    free(read_buf);

    // if len exceeds MAX_TRAILERS_LEN, the remaining input will not be
    // read, but EOF will still be indicated in order to avoid an unbounded
    // read due to garbled input

    if (grpc_status) {
        return ERROR_REPORTED;
    }

    struct json_object *jobj = json_object_new_boolean(false);
    if (!jobj) {
        return ERROR_ALLOCATION;
    }

    *pjobj = jobj;
    return 0;
}

//
// decode a grpc prefixed message into a json object
//
// return value in pjobj is boolean type object for EOF, or object type object
// for decoded message
//
static int jobj_from_osfd(int fd, struct json_object **pjobj,
        struct message_type *msg_type, const struct decode_ops *decode_ops,
        void *decode_ctx)
{
    uint8_t read_buf[5];
    struct json_object *jobj;

    ssize_t n = read_fully(fd, read_buf, 5);
    if (n == -1) {
        return ERROR_REPORTED;
    }

    if (n == 0) {
        // EOF
        jobj = json_object_new_boolean(false);
        if (!jobj) {
            return ERROR_ALLOCATION;
        }

        *pjobj = jobj;
        return 0;
    }

    size_t tot_len = (size_t)read_buf[1] << 24 | (size_t)read_buf[2] << 16 |
        (size_t)read_buf[3] << 8 | (size_t)read_buf[4];

    if (read_buf[0] == 0x80) {
        // grpc-web trailers
        return read_trailers(fd, pjobj, tot_len);
    }

    if (read_buf[0] != 0) {
        fprintf(stderr, "Bad grpc prefix\n");
        return ERROR_REPORTED;
    }

    struct pb_decode_info *info = new_parse_info((int)tot_len);
    if (!info) {
        return ERROR_ALLOCATION;
    }

    int rv;
    jobj = json_object_new_object();
    if (!jobj) {
        rv = ERROR_ALLOCATION;
        goto out_free_info;
    }

    init_jx_decode_ctx(decode_ctx, jobj, msg_type);

    rv = decode_osfd(fd, tot_len, info, decode_ops, decode_ctx);
    if (rv) {
        json_object_put(jobj);
    } else {
        *pjobj = jobj;
    }

out_free_info:
    free_parse_info(info);

    return rv;
}

////////////////////////////////////////

static int decode_fdx_file(const char *filename, bool fds)
{
    void *decode_ctx;
    const struct decode_ops *decode_ops;
    if (fds) {
        decode_ctx = new_fds_decode_ctx();
        decode_ops = &fds_decode_ops;
    } else {
        decode_ctx = new_fd_decode_ctx();
        decode_ops = &fd_decode_ops;
    }

    if (!decode_ctx) {
        return ERROR_ALLOCATION;
    }

    int rv;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Error opening FileDescriptor file");
        rv = ERROR_REPORTED;
        goto out_free_ctx;
    }

    struct stat stat_buf;
    if (fstat(fd, &stat_buf) != 0) {
        perror("Error getting file info");
        rv = ERROR_REPORTED;
        goto out_close;
    }

    size_t tot_len = (size_t)stat_buf.st_size;
    struct pb_decode_info *info = new_parse_info((int)tot_len);
    if (!info) {
        rv = ERROR_ALLOCATION;
        goto out_close;
    }

    rv = decode_osfd(fd, tot_len, info, decode_ops, decode_ctx);

    free_parse_info(info);

out_close:
    close(fd);

out_free_ctx:
    free(decode_ctx);

    return rv;
}

static int decode_fd_file(const char *filename)
{
    return decode_fdx_file(filename, false);
}

static int decode_fds_file(const char *filename)
{
    return decode_fdx_file(filename, true);
}

static int decode_rr_file(const char *filename)
{
    int rv = load_reflection_v1();
    if (rv) {
        return rv;
    }

    struct message_type *msg_type =
        resolve_fq_msg_type("@reflection_v1.ServerReflectionResponse");
    if (!msg_type) {
        // shouldn't happen
        return ERROR_INTERNAL(1);
    }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Failed opening reflect response file");
        return ERROR_REPORTED;
    }

    bool done = false;
    while (!done) {
        void *ctx = new_rr_decode_ctx(NULL);
        if (!ctx) {
            rv = ERROR_ALLOCATION;
            break;
        }

        struct json_object *jobj;
        rv = jobj_from_osfd(fd, &jobj, msg_type, &rr_decode_ops, ctx);
        free_rr_decode_ctx(ctx);
        if (rv) {
            break;
        }

        if (json_object_is_type(jobj, json_type_boolean)) {
            // EOF
            done = true;
        } else {
            rv = check_reflection_response(jobj);
            if (rv) {
                done = true;
            }
        }

        json_object_put(jobj);
    }

    close(fd);

    return rv;
}

static int decode_to_json(const char *type_name, struct ugrpc_options *opts)
{
    struct message_type *msg_type = resolve_fq_msg_type(type_name);
    if (!msg_type) {
        return ERROR_REPORTED;
    }

    int rv;
    bool done = false;
    while (!done) {
        struct json_object *jobj;
        if (opts->light) {
            void *ctx = new_jstream_decode_ctx();
            if (!ctx) {
                rv = ERROR_ALLOCATION;
                break;
            }

            rv = jobj_from_osfd(STDIN_FILENO, &jobj, msg_type, &jstream_decode_ops, ctx);
            if (!rv) {
                check_jstream_mishandled(ctx);
            }

            free_jstream_decode_ctx(ctx);
        } else {
            void *ctx = new_jobj_decode_ctx();
            if (!ctx) {
                rv = ERROR_ALLOCATION;
                break;
            }

            rv = jobj_from_osfd(STDIN_FILENO, &jobj, msg_type, &jobj_decode_ops, ctx);
            free_jobj_decode_ctx(ctx);
        }
        if (rv) {
            break;
        }

        if (json_object_is_type(jobj, json_type_boolean)) {
            // EOF
            done = true;
        } else {
            rv = print_jobj_result(jobj, opts);
            if (rv) {
                done = true;
            }
        }

        json_object_put(jobj);
    }

    return rv;
}

static int fd_writer(struct output_buffer *outbuf, void *arg)
{
    int fd = (int)(long)arg;
    uint8_t *start = (uint8_t *)outbuf + sizeof(struct output_buffer);

    while (start < outbuf->current) {
        ssize_t written = write(fd, start, outbuf->current - start);
        if (written < 0) {
            perror("Error writing output");
            return ERROR_REPORTED;
        }
        start += written;
    }

    return 0;
}

static int encode_from_json(const char* json_str, const char *type_name)
{
    struct message_type *msg_type = resolve_fq_msg_type(type_name);
    if (!msg_type) {
        return ERROR_REPORTED;
    }

    struct output_buffer *outbuf;
    outbuf = new_outbuf(MAX_READ, fd_writer, (void *)STDOUT_FILENO);
    if (!outbuf) {
        return ERROR_ALLOCATION;
    }

    int rv = jstr_encode_msg(json_str, msg_type, outbuf);

    free(outbuf);

    return rv;
}

#ifdef UCLIENT_SERVICE
static int reflect(const char *host, const char *symbol_name,
        struct ugrpc_options *opts)
{
    opts->fds_outbuf = new_outbuf(MAX_READ, fd_writer, (void *)STDOUT_FILENO);
    if (!opts->fds_outbuf) {
        return ERROR_ALLOCATION;
    }

    int rv = uclient_service(host, symbol_name, NULL, opts);

    free(opts->fds_outbuf);

    return rv;
}
#endif

#ifdef CLEANUP_SYMBOL_DB
// the symbol database is normally just leaked, but not doing so can be
// useful to check for other leaks
static void cleanup_symbol_db(void)
{
    cleanup_fd_symbols();
}
#endif

static int usage(const char *prog_name)
{
    fprintf(stderr, "Usage:\n"
            "    %s [options] decode MESSAGE_TYPE\n"
            "    %s [options] encode MESSAGE_TYPE JSON_STRING\n"
#ifdef UCLIENT_SERVICE
            "    %s [options] reflect HOST[:PORT] SYMBOL_NAME\n"
            "    %s [options] service HOST[:PORT] SERVICE_NAME.METHOD JSON_STRING\n"
#endif
            "\n"
            "All protobuf names must be fully qualified\n"
            "\n"
            "Global options:\n"
            "    -f FILENAME    load serialized FileDescriptorProto message from file\n"
            "    -r FILENAME    load grpc reflection service response from file\n"
            "    -s FILENAME    load serialized FileDescriptorSet message from file\n"
            "\n"
#ifdef UCLIENT_SERVICE
            "Decode and service options:\n"
#else
            "Decode options:\n"
#endif
            "    -l             use low-memory algorithm to emit JSON string\n"
#ifdef UCLIENT_SERVICE
            "    -n             disable use of reflection for unknown symbols\n"
#endif
            "    -p             pretty-print JSON string (unless low-memory algo)\n"
            "\n",
            prog_name, prog_name
#ifdef UCLIENT_SERVICE
            , prog_name, prog_name
#endif
            );

    return 1;
}

static int report_error(int rv)
{
    if (rv >= 0) {
        return 0;
    }

    switch (rv)  {
        case ERROR_ALLOCATION:
            fprintf(stderr, "Memory allocation error\n");
            break;
        case ERROR_DECODE:
            fprintf(stderr, "Badly formed message or protocol mismatch\n");
            break;
        case ERROR_REPORTED:
            break;
        default:
            fprintf(stderr, "Internal error %d\n", INTERNAL_ERROR_CODE(rv));
            break;
    }

    return 1;
}

int main(int argc, char **argv)
{
    static const char optstring[] = "lnpf:s:r:";

#ifdef CLEANUP_SYMBOL_DB
    atexit(cleanup_symbol_db);
#endif

    int opt;
    while ((opt = getopt(argc, argv, optstring)) != -1) {
        // first pass is just a syntax check
        if (opt == '?') {
            return usage(argv[0]);
        }
    }

    int nargs = argc - optind;
    if (nargs < 1) {
        return usage(argv[0]);
    }

    int cmd;
    int sym_ind;
    if (!strcmp(argv[optind], "decode")) {
        if (nargs != 2) {
            return usage(argv[0]);
        }

        cmd = CMD_DECODE;
        sym_ind = optind + 1;
    } else if (!strcmp(argv[optind], "encode")) {
        if (nargs != 3) {
            return usage(argv[0]);
        }

        cmd = CMD_ENCODE;
        sym_ind = optind + 1;
#ifdef UCLIENT_SERVICE
    } else if (!strcmp(argv[optind], "reflect")) {
        if (nargs != 3) {
            return usage(argv[0]);
        }

        cmd = CMD_REFLECT;
        sym_ind = optind + 2;
    } else if (!strcmp(argv[optind], "service")) {
        if (nargs != 4) {
            return usage(argv[0]);
        }

        cmd = CMD_SERVICE;
        sym_ind = optind + 2;
#endif
    } else {
        return usage(argv[0]);
    }

    // allow fully qualified names either with or without leading '.'
    if (*argv[sym_ind] == '.') {
        argv[sym_ind]++;
    }

    int rv;

    // allow use of internal reflection service fd for encode or decode
    if ((cmd == CMD_DECODE || cmd == CMD_ENCODE) &&
            !strncmp(argv[sym_ind], "@reflection_v1.", 15)) {
        rv = load_reflection_v1();
        if (rv) {
            return report_error(rv);
        }
    }

    struct ugrpc_options uopts;
    memset(&uopts, 0, sizeof(uopts));

    optind = 1;
    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
            case 'l':
                uopts.light = true;
                break;
            case 'n':
                uopts.no_reflection = true;
                break;
            case 'p':
                uopts.pretty = true;
                break;
            case 'f':
                rv = decode_fd_file(optarg);
                if (rv) {
                    return report_error(rv);
                }
                break;
            case 's':
                rv = decode_fds_file(optarg);
                if (rv) {
                    return report_error(rv);
                }
                break;
            case 'r':
                rv = decode_rr_file(optarg);
                if (rv) {
                    return report_error(rv);
                }
                break;
            default:
                // shouldn't happen
                return report_error(ERROR_INTERNAL(2));
        }
    }

    switch (cmd) {
        case CMD_DECODE:
            rv = decode_to_json(argv[optind + 1], &uopts);
            break;
        case CMD_ENCODE:
            rv = encode_from_json(argv[optind + 2], argv[optind + 1]);
            break;
#ifdef UCLIENT_SERVICE
        case CMD_REFLECT:
            rv = reflect(argv[optind + 1], argv[optind + 2], &uopts);
            break;
        case CMD_SERVICE:
            rv = uclient_service(argv[optind + 1], argv[optind + 2],
                    argv[optind + 3], &uopts);
            break;
#endif
    }

    return report_error(rv);
}

#ifndef _UGRPC_OPTIONS_H
#define _UGRPC_OPTIONS_H

#include <stdbool.h>

struct ugrpc_options {
    bool light;
    bool pretty;
    bool no_reflection;
    struct output_buffer *fds_outbuf;
};

#endif /* _UGRPC_OPTIONS_H */

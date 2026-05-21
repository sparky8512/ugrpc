#ifndef _UGRPC_UCLIENT_SERVICE_H
#define _UGRPC_UCLIENT_SERVICE_H

#include "options.h"

int uclient_service(const char *host, const char *method_name,
        const char *json_str, struct ugrpc_options *opts);

#endif /* _UGRPC_UCLIENT_SERVICE_H */

#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include "server.h"

// Common function to handle requests for both HTTP/1.1 and HTTP/2
int handle_request_common(Connection *conn, const char *path, const char *method, const char *version, 
                         const char *raw_request, size_t raw_len);

#endif
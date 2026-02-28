#ifndef HTTP1_H
#define HTTP1_H

#include "server.h"

// Handle read events for HTTP/1.x connection
// Returns 0 on success, < 0 on critical error (close connection)
void http1_handle_read(Connection *conn);

// Handle write events for HTTP/1.x connection
void http1_handle_write(Connection *conn);

#endif

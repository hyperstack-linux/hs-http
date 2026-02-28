# C Plugin API

## Overview

The C API allows writing plugins for hs-http in plain C. Plugins are compiled as shared objects (.so files) and loaded dynamically by the server.

## Plugin Structure

A plugin must export a `PluginInfo` structure with the following fields:

```c
typedef struct {
    int api_version;
    const char* name;
    int (*init)(PluginAPI* api);
    void (*cleanup)(void);
    PluginEndpoint* endpoints;
    int endpoint_count;
} PluginInfo;
```

## Macros

### PLUGIN_INIT

Use the `PLUGIN_INIT` macro to define your plugin:

```c
PLUGIN_INIT("my-plugin", init_fn, cleanup_fn, endpoints_array);
```

Parameters:
- `my-plugin` - plugin name (string)
- `init_fn` - initialization function name
- `cleanup_fn` - cleanup function name  
- `endpoints_array` - array of PluginEndpoint structures

## Data Types

### PluginRequest

```c
typedef struct {
    int client_fd;           // Client socket file descriptor
    char method[16];         // HTTP method (GET, POST, etc.)
    char path[1024];         // Request path
    char full_path[1024];    // Full path with query string
    char version[16];       // HTTP version
    QueryParam* query_params;    // Query parameters array
    int query_param_count;       // Number of query parameters
    Header* headers;             // HTTP headers array
    int header_count;            // Number of headers
    unsigned char* body;         // Request body
    unsigned int body_len;       // Body length
} PluginRequest;
```

### PluginAPI

```c
typedef struct {
    void (*send_response)(int client_fd, int status_code, const char* status_text, 
                          const char* content_type, const unsigned char* body, unsigned int body_len);
    void (*send_response_with_headers)(int client_fd, int status_code, const char* status_text, 
                          const char* content_type, const unsigned char* body, unsigned int body_len,
                          Header* headers, int header_count);
    void (*log)(int level, const char* format, ...);
    const char* (*get_query_param)(PluginRequest* req, const char* key);
    const char* (*get_header)(PluginRequest* req, const char* key);
    int (*sse_init)(int client_fd);
    int (*sse_send)(int client_fd, const char* event, const unsigned char* data, unsigned int data_len);
    void (*sse_close)(int client_fd);
} PluginAPI;
```

### PluginEndpoint

```c
typedef struct {
    const char* path;       // URL path (e.g., "/api/users")
    PluginHandler handler;  // Handler function
    const char* method;     // HTTP method (GET, POST, etc.) or NULL for any
} PluginEndpoint;
```

## PluginAPI Functions

### send_response

Send a simple HTTP response.

```c
void send_response(int client_fd, int status_code, const char* status_text, 
                  const char* content_type, const unsigned char* body, unsigned int body_len);
```

Parameters:
- `client_fd` - client socket file descriptor from PluginRequest
- `status_code` - HTTP status code (200, 404, etc.)
- `status_text` - status text ("OK", "Not Found", etc.)
- `content_type` - MIME type ("text/plain", "application/json", etc.)
- `body` - response body bytes
- `body_len` - body length in bytes

### send_response_with_headers

Send an HTTP response with custom headers.

```c
void send_response_with_headers(int client_fd, int status_code, const char* status_text, 
                                const char* content_type, const unsigned char* body, unsigned int body_len,
                                Header* headers, int header_count);
```

### log

Log a message to the server log.

```c
void log(int level, const char* format, ...);
```

Log levels:
- `LOG_ERROR` (1)
- `LOG_WARN` (2)
- `LOG_INFO` (3)
- `LOG_DEBUG` (4)
- `LOG_TRACE` (5)

### get_query_param

Get a query parameter value by key.

```c
const char* get_query_param(PluginRequest* req, const char* key);
```

Returns the value string or NULL if not found.

### get_header

Get an HTTP header value by key (case-insensitive).

```c
const char* get_header(PluginRequest* req, const char* key);
```

Returns the value string or NULL if not found.

## Server-Sent Events (SSE)

### sse_init

Initialize SSE response. Sends SSE headers and keeps connection open.

```c
int sse_init(int client_fd);
```

Returns 0 on success, -1 on failure.

### sse_send

Send an SSE message.

```c
int sse_send(int client_fd, const char* event, const unsigned char* data, unsigned int data_len);
```

Parameters:
- `client_fd` - client socket file descriptor
- `event` - event name (optional, can be NULL)
- `data` - message data
- `data_len` - data length

### sse_close

Close SSE connection.

```c
void sse_close(int client_fd);
```

## Example Plugin

```c
#include "plugin_api.h"
#include <string.h>
#include <stdio.h>

static PluginAPI* api = NULL;

int init(PluginAPI* plugin_api) {
    api = plugin_api;
    api->log(LOG_INFO, "My plugin initialized");
    return 0;
}

int handle_index(PluginRequest* req) {
    const char* response = "Hello from plugin!";
    api->send_response(req->client_fd, 200, "OK", "text/plain",
                      (const unsigned char*)response, strlen(response));
    return 0;
}

int handle_sse(PluginRequest* req) {
    api->sse_init(req->client_fd);
    
    for (int i = 0; i < 10; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Message %d", i);
        api->sse_send(req->client_fd, "message", 
                     (const unsigned char*)msg, strlen(msg));
    }
    
    api->sse_close(req->client_fd);
    return 0;
}

void cleanup(void) {
    api->log(LOG_INFO, "My plugin cleanup");
}

PluginEndpoint endpoints[] = {
    { "/", handle_index, "GET" },
    { "/events", handle_sse, "GET" }
};

PLUGIN_INIT("my-plugin", init, cleanup, endpoints);
```

## Building

Compile your plugin as a shared library:

```bash
gcc -shared -fPIC -I/path/to/hs-http/src/include my_plugin.c -o libmy_plugin.so
```

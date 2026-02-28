# hs-http

A high-performance, lightweight HTTP server written in C with an event-driven architecture using epoll, featuring a powerful plugin system for dynamic content generation.

## License

This project is licensed under the BSD 3-Clause License. See the LICENSE file for details.

## Philosophy

**hs-http** is designed with simplicity and performance in mind. It provides:

- **Zero dependencies** for the core server (only standard C library)
- **Event-driven I/O** using epoll for maximum scalability
- **Non-blocking operations** for both network and disk I/O
- **Extensible plugin system** with a clean C/C++ API
- **Minimal configuration** - sensible defaults, easy customization

The server handles static files efficiently while allowing dynamic content through plugins written in C or C++ that are loaded as shared libraries (.so files).

## Features

- ✅ Asynchronous I/O with epoll
- ✅ HTTP/1.1 with GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH methods (plugin-handled)
- ✅ Static file serving with MIME type detection
- ✅ Plugin system for dynamic content (C and C++)
- ✅ Query string parsing
- ✅ TCP reverse proxy support
- ✅ Configurable logging levels
- ✅ Customizable error pages
- ✅ Request timing headers

## Building

```bash
meson setup build
meson compile -C build
```

The binary will be created at `build/hs-http`.

## Configuration

Configuration is read from `config` file (or `/etc/hs-http/config`). All settings are optional with sensible defaults.

### Configuration Format

```ini
# Server settings
listen=8080
interface=0.0.0.0
root=webdocs

# Logging
log=stdout info
# or: log=/var/log/hs-http.log debug

# Require file extensions (true/false)
# true:  /about.html works, /about returns 404
# false: both /about and /about.html work (tries .html automatically)
require_extensions=false

# Plugins (shared libraries for dynamic content)
plugin ./plugin.so /api
plugin ./auth.so /auth

# TCP reverse proxy
redirect=/backend 127.0.0.1:3000 tcp
```

### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `listen` | `8080` | Port to listen on |
| `interface` | `0.0.0.0` | Network interface to bind to |
| `root` | `webdocs` | Directory for static files |
| `log` | *(disabled)* | Log file path and level (stdout/stderr/path level) |
| `require_extensions` | `true` | Whether URLs must include file extensions |
| `plugin` | *(none)* | Load plugin: `plugin <path.so> <endpoint>` |
| `redirect` | *(none)* | TCP proxy: `redirect <path> <host:port> tcp` |

### Log Levels

- `error` - Only errors
- `warn` - Warnings and errors
- `info` - Informational messages (default for production)
- `debug` - Debug information
- `trace` - Detailed trace information

## Plugin System

Plugins are shared libraries (.so files) that handle dynamic content. They can be written in C or C++ and are loaded at server startup.

### C Plugin API

**Example Plugin (C):**

```c
#include "plugin_api.h"
#include <string.h>

static PluginAPI* api = NULL;

int init(PluginAPI* plugin_api) {
    api = plugin_api;
    api->log(LOG_INFO, "Plugin initialized");
    return 0;
}

int handle_hello(PluginRequest* req) {
    const char* name = api->get_query_param(req, "name");
    
    char response[1024];
    if (name) {
        snprintf(response, sizeof(response), "Hello, %s!", name);
    } else {
        strcpy(response, "Hello, World!");
    }
    
    api->send_response(req->client_fd, 200, "OK", "text/plain",
                      (const unsigned char*)response, strlen(response));
    return 0;
}

int handle_json(PluginRequest* req) {
    const char* json = "{\"status\":\"ok\",\"message\":\"Plugin works!\"}";
    api->send_response(req->client_fd, 200, "OK", "application/json",
                      (const unsigned char*)json, strlen(json));
    return 0;
}

void cleanup(void) {
    api->log(LOG_INFO, "Plugin cleanup");
}

PluginEndpoint endpoints[] = {
    { "/", handle_hello, "GET" },
    { "/json", handle_json, "GET" },
    { "/status", handle_json, NULL }  // NULL = any method
};

PLUGIN_INIT("my-plugin", init, cleanup, endpoints);
```

**Compile:**
```bash
gcc -shared -fPIC -I./src/include plugin.c -o plugin.so
```

**Register in config:**
```ini
plugin ./plugin.so /api
```

**Access:**
- `http://localhost:8080/api/` → "Hello, World!"
- `http://localhost:8080/api/?name=John` → "Hello, John!"
- `http://localhost:8080/api/json` → JSON response

### C++ Plugin API

**Example Plugin (C++):**

```cpp
#include "plugin_api.hpp"

Plugin* plugin = nullptr;

static void __attribute__((constructor(101))) init_plugin() {
    plugin = new Plugin("MyCppPlugin");
}

static void __attribute__((constructor(102))) loop() {
    if (!plugin) return;

    // Simple text response (any method)
    plugin->registerHandler("/", [](Request& req, Response& res) {
        res.sendText("Hello from C++!");
    });

    // JSON response for GET
    plugin->registerHandler("/users", [](Request& req, Response& res) {
        res.sendJSON(R"({"users": ["alice", "bob"]})");
    }, "GET");

    // Echo body and headers for POST
    plugin->registerHandler("/echo", [](Request& req, Response& res) {
        // Read body easily
        std::string body = req.getBodyString();
        const char* ct = req.getHeader("Content-Type");

        // Set a header in response
        res.setHeader("X-Echo-Content-Type", ct ? ct : "(none)");
        res.send("text/plain", body);
    }, "POST");

    // Single registration that accepts any method (method="")
    plugin->registerHandler("/any", [](Request& req, Response& res) {
        std::string out = std::string(req.getMethod()) + " " + req.getFullPath() + "\n";
        for (auto &h : req.getHeaders()) {
            out += h.first + ": " + h.second + "\n";
        }
        res.sendText(out);
    });

    // Callbacks
    plugin->setInitCallback([]() {
        Plugin::logInfo("C++ plugin initialized!");
    });

    plugin->setCleanupCallback([]() {
        Plugin::logInfo("C++ plugin cleanup");
    });
}
```

**Compile:**
```bash
g++ -shared -fPIC -std=c++11 -I./src/include plugin.cpp -o plugin.so
```

### Plugin API Reference

#### C API

**Types:**
- `PluginRequest` - HTTP request information
  - `int client_fd` - Client socket
  - `char method[16]` - HTTP method (GET, HEAD, etc.)
  - `char path[1024]` - Request path (within plugin endpoint)
  - `char full_path[1024]` - Complete request path
  - `QueryParam* query_params` - Array of query parameters
  - `int query_param_count` - Number of query parameters
  - `Header* headers` - Array of request headers (key/value)
  - `int header_count` - Number of headers
  - `const unsigned char* body` - Pointer to request body (if present)
  - `unsigned int body_len` - Length of the request body

**Functions:**
- `api->send_response(fd, status, status_text, content_type, body, len)` - Send HTTP response
- `api->log(level, format, ...)` - Write to server log
- `api->get_query_param(req, key)` - Get query parameter value
- `api->get_header(req, key)` - Get request header value (case-insensitive)

**Log Levels:**
- `LOG_ERROR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG`, `LOG_TRACE`

#### C++ API

**Request Class:**
- `const char* getMethod()` - Get HTTP method
- `const char* getPath()` - Get request path
- `const char* getFullPath()` - Get complete path
- `const char* getQueryParam(key)` - Get query parameter
- `bool hasQueryParam(key)` - Check if parameter exists
- `int getQueryParamCount()` - Get number of parameters
- `const char* getQueryParamKey(index)` - Get parameter key by index
- `const char* getQueryParamValue(index)` - Get parameter value by index
- `const char* getHeader(key)` - Get header value (case-insensitive)
- `int getHeaderCount()` - Number of headers
- `const char* getHeaderKey(index)` - Header key by index
- `const char* getHeaderValue(index)` - Header value by index
- `const unsigned char* getBody()` - Pointer to request body
- `unsigned int getBodyLen()` - Length of request body

**Response Class:**
- `sendText(text, status=200)` - Send plain text response
- `sendJSON(json, status=200)` - Send JSON response
- `sendHTML(html, status=200)` - Send HTML response
- `send(status, status_text, content_type, body)` - Send custom response

**Plugin Class:**
- `registerHandler(path, handler, method="GET")` - Register endpoint handler. Use `method=""` (empty string) to accept any HTTP method.
- `setInitCallback(callback)` - Set initialization callback
- `setCleanupCallback(callback)` - Set cleanup callback
- `static void logInfo(msg)` - Log informational message
- `static void logDebug(msg)` - Log debug message
- `static void logError(msg)` - Log error message

## HTTP Headers

All responses include:
- `Server: hs-http/1.0`
- `Date: <RFC 2822 date>`
- `Content-Type: <mime-type>`
- `Content-Length: <bytes>`
- `X-Request-Time: <seconds>` - Request processing time
- `Connection: close`

To customize the server name/version, edit `SERVER_NAME` and `SERVER_VERSION` in `src/include/server.h`.

## Error Pages

Custom error pages are located in `src/errors/*.html`:
- `400.html` - Bad Request
- `403.html` - Forbidden
- `404.html` - Not Found
- `405.html` - Method Not Allowed
- `500.html` - Internal Server Error

These files are compiled into the binary at build time.

## Performance

The server uses epoll with edge-triggered mode for optimal performance:
- Non-blocking accept, read, write operations
- Efficient file serving with zero-copy when possible
- Connection pooling and reuse
- Minimal memory allocations per request

Plugins run synchronously but the server handles I/O asynchronously, ensuring one slow plugin doesn't block other connections.

## Reverse Proxy

hs-http can proxy TCP connections to backend servers:

```ini
redirect=/api 127.0.0.1:3000 tcp
redirect=/ws 127.0.0.1:8080 tcp
```

Requests to `/api/*` will be proxied to `127.0.0.1:3000`.

## Examples

### Serving Static Files

```bash
# Start server
./build/hs-http

# Access files
curl http://localhost:8080/index.html
curl http://localhost:8080/static/style.css
```

### Using Plugins

```bash
# Create plugin
gcc -shared -fPIC -I./src/include my_plugin.c -o my_plugin.so

# Configure
echo "plugin ./my_plugin.so /api" >> config

# Restart server
./build/hs-http
```

### Query Parameters

```bash
# Plugin can access query params
curl "http://localhost:8080/api/search?q=test&limit=10"
```

## Contributing

Contributions are welcome! Please ensure:
- Code follows the existing style
- Plugins maintain API compatibility
- Error handling is robust
- No memory leaks (test with valgrind)

## License

Copyright (c) 2026, Aleksander Płomiński
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

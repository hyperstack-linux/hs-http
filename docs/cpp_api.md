# C++ Plugin API

## Overview

The C++ API provides a convenient wrapper around the C API with object-oriented design. It uses RAII and std::string for easier memory management.

Include: `#include "plugin_api.hpp"`

## Classes

### Request

Represents an incoming HTTP request.

```cpp
class Request {
public:
    Request(PluginRequest* r);
    
    int getFd() const;
    const char* getMethod() const;
    const char* getPath() const;
    const char* getFullPath() const;
    
    // Query parameters
    const char* getQueryParam(const char* key) const;
    bool hasQueryParam(const char* key) const;
    int getQueryParamCount() const;
    const char* getQueryParamKey(int index) const;
    const char* getQueryParamValue(int index) const;
    
    // HTTP headers
    const char* getHeader(const char* key) const;
    int getHeaderCount() const;
    const char* getHeaderKey(int index) const;
    const char* getHeaderValue(int index) const;
    std::vector<std::pair<std::string, std::string>> getHeaders() const;
    
    // Body
    std::string getBodyString() const;
    std::vector<unsigned char> getBodyRaw() const;
    unsigned int getBodyLen() const;
    
    // Form data (urlencoded)
    std::unordered_map<std::string, std::string>& getBody() const;
    const char* getBodyParam(const std::string& key) const;
};
```

### Response

Represents an HTTP response. Use to send responses to clients.

```cpp
class Response {
public:
    Response(int fd);
    
    // Headers
    void setHeader(const std::string& key, const std::string& value);
    void setStatus(int s, const std::string& text);
    
    // Sending methods
    void sendRaw(const std::string& content_type, const unsigned char* body, size_t body_len);
    void send(const std::string& content_type, const std::string& body);
    void sendText(const std::string& text, int s = 200);
    void sendJSON(const std::string& json, int s = 200);
    void sendHTML(const std::string& html, int s = 200);
    void sendBinary(const std::vector<unsigned char>& data, const std::string& content_type, int s = 200);
    
    // Server-Sent Events
    void startSSE();
    void sendSSE(const std::string& data, const std::string& event = "");
    void closeSSE();
};
```

### Plugin

Main plugin class for registering handlers.

```cpp
class Plugin {
public:
    Plugin(const std::string& plugin_name);
    ~Plugin();
    
    void registerHandler(const std::string& path, 
                        std::function<void(Request&, Response&)> handler,
                        const std::string& method = "");
    
    void setInitCallback(std::function<void()> callback);
    void setCleanupCallback(std::function<void()> callback);
    
    // Static logging methods
    static void log(int level, const char* format, ...);
    static void logInfo(const std::string& msg);
    static void logDebug(const std::string& msg);
};
```

## Usage

### Creating a Plugin

Create a global `Plugin` instance and register handlers in a constructor function:

```cpp
#include "plugin_api.hpp"

Plugin* plugin = nullptr;

static void __attribute__((constructor(101))) init_plugin() {
    plugin = new Plugin("MyPlugin");
}

static void __attribute__((constructor(102))) register_handlers() {
    plugin->registerHandler("/", [](Request& req, Response& res) {
        res.sendText("Hello, World!");
    });
    
    plugin->registerHandler("/api/users", [](Request& req, Response& res) {
        res.sendJSON(R"({"users": ["alice", "bob"]})");
    });
}
```

### Handler Signatures

Handlers are lambdas or functions with this signature:

```cpp
void handler(Request& req, Response& res)
```

### Query Parameters

```cpp
plugin->registerHandler("/search", [](Request& req, Response& res) {
    const char* query = req.getQueryParam("q");
    if (query) {
        res.sendText("Searching for: " + std::string(query));
    } else {
        res.sendText("No query provided");
    }
});
```

### Headers

```cpp
plugin->registerHandler("/echo", [](Request& req, Response& res) {
    const char* auth = req.getHeader("Authorization");
    if (auth) {
        res.sendText("Auth: " + std::string(auth));
    }
});
```

### Request Body

```cpp
plugin->registerHandler("/post", [](Request& req, Response& res) {
    std::string body = req.getBodyString();
    res.sendText("Received: " + body);
});
```

### Form Data

```cpp
plugin->registerHandler("/form", [](Request& req, Response& res) {
    const char* name = req.getBodyParam("name");
    const char* email = req.getBodyParam("email");
    
    std::string response = "Name: ";
    response += name ? name : "(none)";
    response += "\nEmail: ";
    response += email ? email : "(none)";
    
    res.sendText(response);
});
```

### JSON Response

```cpp
plugin->registerHandler("/api/data", [](Request& req, Response& res) {
    res.sendJSON(R"({"status": "ok", "count": 42})");
});
```

### HTML Response

```cpp
plugin->registerHandler("/", [](Request& req, Response& res) {
    res.sendHTML("<html><body><h1>Hello!</h1></body></html>");
});
```

### Custom Headers

```cpp
plugin->registerHandler("/redirect", [](Request& req, Response& res) {
    res.setHeader("Location", "https://example.com");
    res.setStatus(302, "Found");
    res.sendText("");
});
```

## Server-Sent Events (SSE)

### Simple SSE

```cpp
plugin->registerHandler("/events", [](Request& req, Response& res) {
    res.startSSE();
    
    for (int i = 0; i < 10; i++) {
        res.sendSSE("Message " + std::to_string(i), "message");
    }
    
    res.closeSSE();
});
```

### Event Types

```cpp
plugin->registerHandler("/events", [](Request& req, Response& res) {
    res.startSSE();
    
    // Different event types
    res.sendSSE("User connected", "connect");
    res.sendSSE("{\"id\": 1, \"name\": \"Alice\"}", "user");
    res.sendSSE("Message text here", "message");
    res.sendSSE("1", "count");
    
    res.closeSSE();
});
```

### Multi-line Data

```cpp
res.sendSSE("line1\nline2\nline3", "data");
```

This produces:
```
data: line1
data: line2
data: line3
```

## Logging

```cpp
plugin->registerHandler("/test", [](Request& req, Response& res) {
    Plugin::logInfo("Handling /test request");
    Plugin::logDebug("Request path: " + std::string(req.getPath()));
    
    res.sendText("Done");
});
```

## Init and Cleanup Callbacks

```cpp
static void __attribute__((constructor(101))) init_plugin() {
    plugin = new Plugin("MyPlugin");
}

static void __attribute__((constructor(102))) register_handlers() {
    plugin->setInitCallback([]() {
        Plugin::logInfo("Plugin initialized!");
    });
    
    plugin->setCleanupCallback([]() {
        Plugin::logInfo("Plugin cleanup!");
    });
    
    // Register handlers...
}
```

## Building

Compile your C++ plugin:

```bash
g++ -shared -fPIC -std=c++17 -I/path/to/hs-http/src/include my_plugin.cpp -o libmy_plugin.so
```

Or use the meson build system - see `plugin_examples/meson.build` for reference.

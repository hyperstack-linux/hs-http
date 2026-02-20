#ifndef PLUGIN_API_HPP
#define PLUGIN_API_HPP

extern "C" {
#include "plugin_api.h"
}

#include <functional>
#include <vector>
#include <string>
#include <cstring>
#include <strings.h>
#include <cstdarg>
#include <unordered_map>
#include <cstdlib>

class Request {
private:
    PluginRequest* req;
    mutable std::unordered_map<std::string, std::string> body_map;
    mutable bool body_parsed = false;

    static std::string urlDecode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            if (c == '+') {
                out.push_back(' ');
            } else if (c == '%' && i + 2 < s.size()) {
                char hex[3] = { s[i+1], s[i+2], '\0' };
                char* endptr = nullptr;
                long val = strtol(hex, &endptr, 16);
                if (endptr && *endptr == '\0') {
                    out.push_back((char)val);
                    i += 2;
                } else {
                    out.push_back('%');
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    void parseBody() const {
        if (body_parsed) return;
        body_parsed = true;
        std::string b = getBodyString();
        if (b.empty()) return;
        size_t i = 0;
        while (i < b.size()) {
            size_t j = b.find('&', i);
            if (j == std::string::npos) j = b.size();
            std::string token = b.substr(i, j - i);
            size_t eq = token.find('=');
            std::string k, v;
            if (eq == std::string::npos) {
                k = urlDecode(token);
                v = std::string();
            } else {
                k = urlDecode(token.substr(0, eq));
                v = urlDecode(token.substr(eq + 1));
            }
            body_map.emplace(k, v);
            i = j + 1;
        }
    }

public:
    Request(PluginRequest* r) : req(r) {}
    int getFd() const { return req->client_fd; }
    const char* getMethod() const { return req->method; }
    const char* getPath() const { return req->path; }
    const char* getFullPath() const { return req->full_path; }

    const char* getQueryParam(const char* key) const {
        for (int i = 0; i < req->query_param_count; i++) {
            if (strcmp(req->query_params[i].key, key) == 0) {
                return req->query_params[i].value;
            }
        }
        return nullptr;
    }

    bool hasQueryParam(const char* key) const {
        return getQueryParam(key) != nullptr;
    }

    int getQueryParamCount() const {
        return req->query_param_count;
    }

    const char* getQueryParamKey(int index) const {
        if (index >= 0 && index < req->query_param_count) {
            return req->query_params[index].key;
        }
        return nullptr;
    }

    const char* getQueryParamValue(int index) const {
        if (index >= 0 && index < req->query_param_count) {
            return req->query_params[index].value;
        }
        return nullptr;
    }

    // Headers
    const char* getHeader(const char* key) const {
        if (!req || !key) return nullptr;
        for (int i = 0; i < req->header_count; i++) {
            if (strcasecmp(req->headers[i].key, key) == 0) return req->headers[i].value;
        }
        return nullptr;
    }

    int getHeaderCount() const { return req->header_count; }
    const char* getHeaderKey(int index) const { if (index >= 0 && index < req->header_count) return req->headers[index].key; return nullptr; }
    const char* getHeaderValue(int index) const { if (index >= 0 && index < req->header_count) return req->headers[index].value; return nullptr; }

    // Headers as vector
    std::vector<std::pair<std::string, std::string>> getHeaders() const {
        std::vector<std::pair<std::string, std::string>> out;
        if (!req) return out;
        for (int i = 0; i < req->header_count; i++) {
            out.emplace_back(req->headers[i].key, req->headers[i].value);
        }
        return out;
    }

    // Body helpers
    std::string getBodyString() const { if (!req || !req->body || req->body_len == 0) return std::string(); return std::string((const char*)req->body, req->body_len); }
    std::vector<unsigned char> getBodyRaw() const { std::vector<unsigned char> v; if (!req || !req->body || req->body_len == 0) return v; v.assign(req->body, req->body + req->body_len); return v; }
    unsigned int getBodyLen() const { return req ? req->body_len : 0; }

    // Parsed body as map (e.g., form urlencoded)
    std::unordered_map<std::string, std::string>& getBody() const { parseBody(); return body_map; }
    const char* getBodyParam(const std::string& key) const { parseBody(); auto it = body_map.find(key); if (it == body_map.end()) return nullptr; return it->second.c_str(); }
};

class Response {
private:
    int client_fd;
    static PluginAPI* api;
    std::vector<std::pair<std::string,std::string>> headers;
    int status;
    std::string status_text;
    bool sse_started = false;
public:
    Response(int fd) : client_fd(fd), status(200), status_text("OK") {}
    static void setAPI(PluginAPI* a) { api = a; }

    void setHeader(const std::string& key, const std::string& value) { headers.emplace_back(key, value); }
    void setStatus(int s, const std::string& text) { status = s; status_text = text; }

    void sendRaw(const std::string& content_type, const unsigned char* body, size_t body_len) {
        if (!api) return;
        if (!headers.empty() && api->send_response_with_headers) {
            Header hdrs[64];
            int hdr_count = 0;
            for (auto &h : headers) {
                if (hdr_count >= 64) break;
                strncpy(hdrs[hdr_count].key, h.first.c_str(), sizeof(hdrs[hdr_count].key) - 1);
                hdrs[hdr_count].key[sizeof(hdrs[hdr_count].key) - 1] = '\0';
                strncpy(hdrs[hdr_count].value, h.second.c_str(), sizeof(hdrs[hdr_count].value) - 1);
                hdrs[hdr_count].value[sizeof(hdrs[hdr_count].value) - 1] = '\0';
                hdr_count++;
            }
            api->send_response_with_headers(client_fd, status, status_text.c_str(), content_type.c_str(), body, (unsigned int)body_len, hdrs, hdr_count);
        } else {
            api->send_response(client_fd, status, status_text.c_str(), content_type.c_str(), body, (unsigned int)body_len);
        }
    }

    void send(const std::string& content_type, const std::string& body) { sendRaw(content_type, (const unsigned char*)body.c_str(), body.size()); }
    void sendText(const std::string& text, int s = 200) { setStatus(s, "OK"); send("text/plain", text); }
    void sendJSON(const std::string& json, int s = 200) { setStatus(s, "OK"); send("application/json", json); }
    void sendHTML(const std::string& html, int s = 200) { setStatus(s, "OK"); send("text/html", html); }
    void sendBinary(const std::vector<unsigned char>& data, const std::string& content_type, int s = 200) { setStatus(s, "OK"); sendRaw(content_type, data.data(), data.size()); }

    void startSSE() {
        if (!api || !api->sse_init || sse_started) return;
        if (api->sse_init(client_fd) == 0) {
            sse_started = true;
        }
    }

    void sendSSE(const std::string& data, const std::string& event = "") {
        if (!api || !api->sse_send || !sse_started) return;
        api->sse_send(client_fd, event.empty() ? nullptr : event.c_str(), (const unsigned char*)data.data(), (unsigned int)data.size());
    }

    void closeSSE() {
        if (!api || !api->sse_close || !sse_started) return;
        api->sse_close(client_fd);
        sse_started = false;
    }
};

PluginAPI* Response::api = nullptr;

class Plugin {
private:
    struct EndpointHandler {
        std::string path;
        std::string method;
        std::function<void(Request&, Response&)> handler;
    };

    std::string name;
    std::vector<EndpointHandler>* handlers;
    std::function<void()> init_callback;
    std::function<void()> cleanup_callback;
    std::vector<PluginEndpoint>* endpoint_storage;
    
public:
    static PluginAPI* api;
    static Plugin* instance;

    Plugin(const std::string& plugin_name) : name(plugin_name) {
        handlers = new std::vector<EndpointHandler>();
        endpoint_storage = new std::vector<PluginEndpoint>();
        instance = this;
    }

    ~Plugin() {
        // free allocated c-strings in endpoint_storage
        if (endpoint_storage) {
            for (auto &ep : *endpoint_storage) {
                if (ep.path) free((void*)ep.path);
                if (ep.method) free((void*)ep.method);
            }
            delete endpoint_storage;
            endpoint_storage = nullptr;
        }
        if (handlers) delete handlers;
    }

    void registerHandler(const std::string& path, 
                        std::function<void(Request&, Response&)> handler,
                        const std::string& method = "") {
        handlers->push_back({path, method, handler});
    }

    void setInitCallback(std::function<void()> callback) { init_callback = callback; }
    void setCleanupCallback(std::function<void()> callback) { cleanup_callback = callback; }

    static void log(int level, const char* format, ...) {
        if (!api) return;
        va_list args;
        va_start(args, format);
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        api->log(level, "%s", buffer);
    }

    static void logInfo(const std::string& msg) { if (api) api->log(LOG_INFO, "%s", msg.c_str()); }
    static void logDebug(const std::string& msg) { if (api) api->log(LOG_DEBUG, "%s", msg.c_str()); }

    int _init(PluginAPI* plugin_api) {
        api = plugin_api;
        Response::setAPI(plugin_api);
        
        // Najpierw callback init
        if (init_callback) init_callback();
        
        // Potem budujemy tablicę endpointów
        // Najpierw zwalniamy poprzednie zaalokowane C-stringi (jeśli istnieją)
        for (auto &ep_old : *endpoint_storage) {
            if (ep_old.path) free((void*)ep_old.path);
            if (ep_old.method) free((void*)ep_old.method);
        }
        endpoint_storage->clear();
        for (const auto& h : *handlers) {
            PluginEndpoint ep;
            char* p = strdup(h.path.c_str());
            char* m = h.method.empty() ? nullptr : strdup(h.method.c_str());
            ep.path = p;
            ep.method = m;
            ep.handler = [](PluginRequest* req) -> int {
                if (!instance || !instance->handlers) return -1;
                Request request(req);
                Response response(req->client_fd);
                for (const auto& handler : *(instance->handlers)) {
                    if (handler.path == req->path && (handler.method.empty() || handler.method == req->method)) {
                        try {
                            handler.handler(request, response);
                        } catch (const std::exception& e) {
                            if (api) api->log(LOG_ERROR, "C++ handler threw exception: %s", e.what());
                            return -1;
                        } catch (...) {
                            if (api) api->log(LOG_ERROR, "C++ handler threw unknown exception");
                            return -1;
                        }
                        return 0;
                    }
                }
                return -1;
            };
            endpoint_storage->push_back(ep);
        }

        return 0;
    }

    void _cleanup() {
        if (cleanup_callback) cleanup_callback();
    }

    const char* getName() const { return name.c_str(); }
    PluginEndpoint* getEndpoints() { return endpoint_storage->empty() ? nullptr : &(*endpoint_storage)[0]; }
    int getEndpointCount() { return endpoint_storage->size(); }
};

PluginAPI* Plugin::api = nullptr;
Plugin* Plugin::instance = nullptr;

extern "C" {
    PLUGIN_EXPORT PluginInfo plugin_info = {
        .api_version = PLUGIN_API_VERSION,
        .name = "CppPlugin",
        .init = nullptr,
        .cleanup = nullptr,
        .endpoints = nullptr,
        .endpoint_count = 0
    };
    
    static int _plugin_init(PluginAPI* api) {
        if (Plugin::instance) {
            int result = Plugin::instance->_init(api);
            // Po init aktualizujemy plugin_info
            plugin_info.endpoints = Plugin::instance->getEndpoints();
            plugin_info.endpoint_count = Plugin::instance->getEndpointCount();
            return result;
        }
        return -1;
    }
    
    static void _plugin_cleanup() {
        if (Plugin::instance) Plugin::instance->_cleanup();
    }
}

static void __attribute__((constructor)) _update_plugin_info() {
    plugin_info.init = _plugin_init;
    plugin_info.cleanup = _plugin_cleanup;
    if (Plugin::instance) {
        plugin_info.name = Plugin::instance->getName();
    }
}

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dlfcn.h>
#include <stdarg.h>
#include "include/server.h"
#include "include/plugin_api.h"
#include "include/http2.h"

static ServerConfig* global_config = NULL;
static Connection* current_h2_conn = NULL;
static uint32_t current_h2_stream = 0;

void set_plugin_h2_context(Connection* conn, uint32_t stream) {
    current_h2_conn = conn;
    current_h2_stream = stream;
}

static void plugin_log(int level, const char* format, ...) {
    if (!global_config) return;
    
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    log_message(global_config, level, "%s", buffer);
}

static const char* get_query_param(PluginRequest* req, const char* key) {
    if (!req || !key) return NULL;
    
    for (int i = 0; i < req->query_param_count; i++) {
        if (strcmp(req->query_params[i].key, key) == 0) {
            return req->query_params[i].value;
        }
    }
    return NULL;
}

static const char* get_header(PluginRequest* req, const char* key) {
    if (!req || !key) return NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].key, key) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

static void plugin_send_response(int client_fd, int status_code, const char* status_text, const char* content_type, const unsigned char* body, unsigned int body_len) {
    if (current_h2_conn) {
        http2_send_response(current_h2_conn, current_h2_stream, status_code, content_type, body, body_len);
    } else {
        send_response(client_fd, status_code, status_text, content_type, body, body_len);
    }
}

static void plugin_send_response_with_headers(int client_fd, int status_code, const char* status_text, const char* content_type, const unsigned char* body, unsigned int body_len, struct Header* headers, int header_count) {
    if (current_h2_conn) {
        // HTTP/2 response with extra headers is not yet fully supported in http2_send_response
        // Sending basic response for now.
        http2_send_response(current_h2_conn, current_h2_stream, status_code, content_type, body, body_len);
    } else {
        send_response_with_headers(client_fd, status_code, status_text, content_type, body, body_len, headers, header_count);
    }
}

static PluginAPI api = {
    .send_response = plugin_send_response,
    .send_response_with_headers = plugin_send_response_with_headers,
    .log = plugin_log,
    .get_query_param = get_query_param,
    .get_header = get_header
};

int load_plugins(ServerConfig* config) {
    global_config = config;
    
    for (int i = 0; i < config->plugin_count; i++) {
        Plugin* p = &config->plugins[i];
        
        log_message(config, LOG_INFO, "Loading plugin %s at endpoint %s", p->so_path, p->endpoint);
        
        p->handle = dlopen(p->so_path, RTLD_NOW | RTLD_LOCAL);
        if (!p->handle) {
            log_message(config, LOG_ERROR, "Failed to load plugin %s: %s", p->so_path, dlerror());
            continue;
        }
        
        PluginInfo* info = (PluginInfo*)dlsym(p->handle, "plugin_info");
        if (!info) {
            log_message(config, LOG_ERROR, "Plugin %s does not export plugin_info", p->so_path);
            dlclose(p->handle);
            p->handle = NULL;
            continue;
        }
        
        if (info->api_version != PLUGIN_API_VERSION) {
            log_message(config, LOG_ERROR, "Plugin %s has incompatible API version %d (expected %d)", 
                       p->so_path, info->api_version, PLUGIN_API_VERSION);
            dlclose(p->handle);
            p->handle = NULL;
            continue;
        }
        
        p->plugin_info = info;
        
        if (info->init) {
            if (info->init(&api) != 0) {
                log_message(config, LOG_ERROR, "Plugin %s initialization failed", p->so_path);
                dlclose(p->handle);
                p->handle = NULL;
                p->plugin_info = NULL;
                continue;
            }
            
            // Po init, odśwież wskaźnik do plugin_info (może się zmienić w C++)
            PluginInfo* refreshed_info = (PluginInfo*)dlsym(p->handle, "plugin_info");
            if (refreshed_info) {
                info = refreshed_info;
                p->plugin_info = info;
            }
        }
        
        log_message(config, LOG_INFO, "Plugin %s (%s) loaded successfully with %d endpoints", 
                   p->so_path, info->name, info->endpoint_count);
        
        for (int j = 0; j < info->endpoint_count; j++) {
            PluginEndpoint* ep = &info->endpoints[j];
            const char* method = ep->method ? ep->method : "GET";
            log_message(config, LOG_DEBUG, "  - %s %s%s", method, p->endpoint, ep->path);
        }
    }
    
    return 0;
}

void unload_plugins(ServerConfig* config) {
    for (int i = 0; i < config->plugin_count; i++) {
        Plugin* p = &config->plugins[i];
        
        if (p->handle && p->plugin_info) {
            PluginInfo* info = (PluginInfo*)p->plugin_info;
            if (info->cleanup) {
                info->cleanup();
            }
            dlclose(p->handle);
            p->handle = NULL;
            p->plugin_info = NULL;
        }
    }
}

int handle_plugin_request(int client_fd, const char* path, char* method, char* version, const char* raw_request, size_t raw_len, ServerConfig* config) {
    for (int i = 0; i < config->plugin_count; i++) {
        Plugin* p = &config->plugins[i];

        if (!p->handle || !p->plugin_info) {
            continue;
        }

        size_t endpoint_len = strlen(p->endpoint);

        if (endpoint_len == 1 && p->endpoint[0] == '/') {
            if (path[0] != '/' || path[1] == '\0') {
                continue;
            }
        } else {
            if (strncmp(path, p->endpoint, endpoint_len) != 0) {
                continue;
            }

            if (path[endpoint_len] != '\0' && path[endpoint_len] != '/') {
                continue;
            }
        }

        PluginInfo* info = (PluginInfo*)p->plugin_info;
        if (!info->endpoints || info->endpoint_count == 0) {
            continue;
        }

        const char* subpath;
        if (endpoint_len == 1 && p->endpoint[0] == '/') {
            subpath = path;
        } else {
            subpath = path + endpoint_len;
        }

        if (*subpath == '\0') {
            subpath = "/";
        }

        // Parse query string
        char path_without_query[1024];
        strncpy(path_without_query, subpath, sizeof(path_without_query) - 1);
        path_without_query[sizeof(path_without_query) - 1] = '\0';

        char* query_start = strchr(path_without_query, '?');
        QueryParam query_params[32];
        int query_param_count = 0;

        if (query_start) {
            *query_start = '\0';
            query_start++;

            char* token = strtok(query_start, "&");
            while (token && query_param_count < 32) {
                char* eq = strchr(token, '=');
                if (eq) {
                    *eq = '\0';
                    strncpy(query_params[query_param_count].key, token, sizeof(query_params[0].key) - 1);
                    query_params[query_param_count].key[sizeof(query_params[0].key) - 1] = '\0';
                    strncpy(query_params[query_param_count].value, eq + 1, sizeof(query_params[0].value) - 1);
                    query_params[query_param_count].value[sizeof(query_params[0].value) - 1] = '\0';
                    query_param_count++;
                }
                token = strtok(NULL, "&");
            }
            subpath = path_without_query;
        }

        for (int j = 0; j < info->endpoint_count; j++) {
            PluginEndpoint* ep = &info->endpoints[j];

            if (strcmp(subpath, ep->path) != 0) {
                continue;
            }

            if (ep->method && strcmp(method, ep->method) != 0) {
                continue;
            }

            PluginRequest req;
            req.client_fd = client_fd;
            strncpy(req.method, method, sizeof(req.method) - 1);
            req.method[sizeof(req.method) - 1] = '\0';
            strncpy(req.path, subpath, sizeof(req.path) - 1);
            req.path[sizeof(req.path) - 1] = '\0';
            strncpy(req.full_path, path, sizeof(req.full_path) - 1);
            req.full_path[sizeof(req.full_path) - 1] = '\0';
            strncpy(req.version, version, sizeof(req.version) - 1);
            req.version[sizeof(req.version) - 1] = '\0';
            req.query_params = query_params;
            req.query_param_count = query_param_count;

            // Parse headers and body from raw_request (non-destructive copy)
            char* buf = NULL;
            Header headers_temp[64];
            Header* headers = NULL;
            int header_count = 0;
            unsigned char* body_ptr = NULL;
            unsigned int body_len = 0;

            if (raw_request && raw_len > 0) {
                buf = malloc(raw_len + 1);
                if (buf) {
                    memcpy(buf, raw_request, raw_len);
                    buf[raw_len] = '\0';

                    log_message(config, LOG_DEBUG, "Raw request (len=%zu) first 200 bytes (printable):\n%.200s", raw_len, buf);
                    // Hex dump first 120 bytes for reliable inspection
                    {
                        char hex[512];
                        int pos = 0;
                        int limit = raw_len < 120 ? raw_len : 120;
                        for (int i = 0; i < limit && pos < (int)sizeof(hex)-3; i++) {
                            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", (unsigned char)buf[i]);
                        }
                        hex[pos] = '\0';
                        log_message(config, LOG_DEBUG, "Raw request hex (first %d bytes): %s", limit, hex);
                    }

                    // Find end of headers (look for blank line: \r\n\r\n or \n\n)
                    char* hdr_end = NULL;
                    for (size_t ii = 0; ii + 1 < raw_len; ii++) {
                        if (buf[ii] == '\n') {
                            for (size_t jj = ii + 1; jj < ii + 5 && jj < raw_len; jj++) {
                                if (buf[jj] == '\n') {
                                    hdr_end = buf + jj;
                                    break;
                                }
                            }
                            if (hdr_end) break;
                        }
                    }

                    // Parse headers line by line - iterate by byte position to handle embedded NULs
                    size_t pos = 0;
                    int line_num = 0;

                    while (pos < raw_len && header_count < 64) {
                        // Find next newline
                        size_t line_start = pos;
                        size_t line_end = pos;
                        while (line_end < raw_len && buf[line_end] != '\n') {
                            line_end++;
                        }

                        // Calculate line length (excluding \r\n or \n)
                        int line_len = line_end - line_start;
                        if (line_len > 0 && (buf[line_end - 1] == '\r' || buf[line_end - 1] == '\0')) {
                            line_len--;
                        }

                        log_message(config, LOG_DEBUG, "Line %d: len=%d, content=%.*s",
                                   line_num, line_len, line_len > 50 ? 50 : line_len, buf + line_start);

                        // Skip first line (request line) and empty lines
                        if (line_num > 0 && line_len > 0) {
                            // Look for ':' separator
                            int colon_pos = -1;
                            for (int i = 0; i < line_len; i++) {
                                if (buf[line_start + i] == ':') {
                                    colon_pos = i;
                                    break;
                                }
                            }

                            if (colon_pos > 0) {
                                // Extract header key
                                int key_len = colon_pos;
                                if (key_len < 256) {
                                    strncpy(headers_temp[header_count].key, buf + line_start, key_len);
                                    headers_temp[header_count].key[key_len] = '\0';
                                }

                                // Extract header value (skip leading spaces)
                                int value_start = colon_pos + 1;
                                while (value_start < line_len && buf[line_start + value_start] == ' ') {
                                    value_start++;
                                }
                                int value_len = line_len - value_start;
                                if (value_len > 0 && value_len < 1024) {
                                    strncpy(headers_temp[header_count].value, buf + line_start + value_start, value_len);
                                    headers_temp[header_count].value[value_len] = '\0';
                                }

                                log_message(config, LOG_DEBUG, "Header %d: '%s' = '%s'",
                                           header_count, headers_temp[header_count].key, headers_temp[header_count].value);
                                header_count++;
                            }
                        }

                        // Stop at blank line (hdr_end)
                        if (hdr_end && line_start >= (size_t)(hdr_end - buf)) {
                            break;
                        }

                        pos = line_end + 1;
                        line_num++;
                    }

                    // Allocate and copy headers
                    if (header_count > 0) {
                        headers = malloc(sizeof(Header) * header_count);
                        if (headers) {
                            memcpy(headers, headers_temp, sizeof(Header) * header_count);
                        }
                    }

                    // Extract body
                    if (hdr_end) {
                        body_ptr = malloc((size_t)(buf + raw_len - (char*)(hdr_end + 1)));
                        if ((size_t)(buf + raw_len - (char*)(hdr_end + 1)) > 0) {
                            body_len = (unsigned int)(buf + raw_len - (char*)(hdr_end + 1));
                            memcpy(body_ptr, hdr_end + 1, body_len);
                        } else {
                            body_len = 0;
                        }
                        log_message(config, LOG_DEBUG, "Found header end (offset=%ld), body_len=%u", (long)(hdr_end - buf), body_len);
                    } else {
                        log_message(config, LOG_DEBUG, "Did not find header end in request copy");
                    }
                }
            }

            req.headers = headers;
            req.header_count = header_count;
            req.body = body_ptr;
            req.body_len = body_len;

            log_message(config, LOG_DEBUG, "Plugin %s handling %s %s (headers=%d body=%u)",
                       info->name, method, path, header_count, body_len);

            int rv = ep->handler(&req);

            if (buf) free(buf);
            if (headers) free(headers);
            if (body_ptr) free(body_ptr);
            return rv;
        }
    }

    return -1;
}

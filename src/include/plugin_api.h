#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include <stddef.h>

#define PLUGIN_API_VERSION 1

#define LOG_ERROR   1
#define LOG_WARN    2
#define LOG_INFO    3
#define LOG_DEBUG   4
#define LOG_TRACE   5

typedef struct {
    char key[256];
    char value[1024];
} QueryParam;

typedef struct Header {
    char key[256];
    char value[1024];
} Header;

typedef struct {
    int client_fd;
    char method[16];
    char path[1024];
    char full_path[1024];
    char version[16];
    QueryParam* query_params;
    int query_param_count;
    Header* headers;
    int header_count;
    unsigned char* body;
    unsigned int body_len;
} PluginRequest;

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

typedef int (*PluginHandler)(PluginRequest* req);

typedef struct {
    const char* path;
    PluginHandler handler;
    const char* method;
} PluginEndpoint;

typedef struct {
    int api_version;
    const char* name;
    int (*init)(PluginAPI* api);
    void (*cleanup)(void);
    PluginEndpoint* endpoints;
    int endpoint_count;
} PluginInfo;

#define PLUGIN_EXPORT __attribute__((visibility("default")))

#define PLUGIN_INIT(plugin_name, init_fn, cleanup_fn, endpoint_array) \
    PLUGIN_EXPORT PluginInfo plugin_info = { \
        .api_version = PLUGIN_API_VERSION, \
        .name = plugin_name, \
        .init = init_fn, \
        .cleanup = cleanup_fn, \
        .endpoints = endpoint_array, \
        .endpoint_count = sizeof(endpoint_array) / sizeof(PluginEndpoint) \
    }

#endif

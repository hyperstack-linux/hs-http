#ifndef SERVER_H
#define SERVER_H

#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <stdarg.h>

#define BUFFER_SIZE 4096
#define SERVER_NAME "hs-http"
#define SERVER_VERSION "1.0"
#define MAX_CACHE_ENTRIES 256
#define MAX_CACHE_FILE_SIZE (1024 * 1024)  // 1MB

#define LOG_ERROR   1
#define LOG_WARN    2
#define LOG_INFO    3
#define LOG_DEBUG   4
#define LOG_TRACE   5

#define MAX_REDIRECTS 32
#define MAX_PLUGINS 16

typedef struct {
    char path_prefix[256];
    char target_host[64];
    int target_port;
} RedirectRule;

typedef struct {
    char endpoint[256];
    char so_path[512];
    void* handle;
    void* plugin_info;
} Plugin;

typedef struct {
    char path[512];
    unsigned char* content;
    size_t size;
    time_t mtime;
    ino_t ino;
    char etag[64];
    char last_modified[128];
    char content_type[64];
    time_t cached_at;
} CacheEntry;

typedef struct {
    CacheEntry entries[MAX_CACHE_ENTRIES];
    int count;
} FileCache;

typedef struct {
    int listen;
    char interface[64];
    char root[256];
    char log_path[256];
    int log_level;
    RedirectRule redirects[MAX_REDIRECTS];
    int redirect_count;
    Plugin plugins[MAX_PLUGINS];
    int plugin_count;
    int require_extensions;
    int cache_max_age;
    int cached_time;
    FileCache* file_cache;
} ServerConfig;

void log_message(ServerConfig* config, int level, const char* format, ...);

int load_config(const char* path, ServerConfig* config);

const char* get_mime_type(const char* path);

struct Header;

void send_response(int client_fd, int status_code, const char* status_text, 
                   const char* content_type, const unsigned char* body, unsigned int body_len);

void send_response_with_headers(int client_fd, int status_code, const char* status_text, 
                   const char* content_type, const unsigned char* body, unsigned int body_len,
                   struct Header* headers, int header_count);

void handle_error(int client_fd, int status_code, const char* status_text, 
                  const char* root, const char* request_path);
void handle_400(int client_fd);
void handle_403(int client_fd);
void handle_404(int client_fd);
void handle_405(int client_fd);
void handle_500(int client_fd);

int parse_request(int client_fd, char* buffer, size_t buffer_size,
                  char* method, size_t method_size,
                  char* path, size_t path_size,
                  char* version, size_t version_size);

void proxy_request(int client_fd, const char* target_host, int target_port, const char* suffix, ServerConfig* config);

void handle_client(int client_fd, ServerConfig* config);

int load_plugins(ServerConfig* config);
void unload_plugins(ServerConfig* config);
int handle_plugin_request(int client_fd, const char* path, char* method, char* version, const char* raw_request, size_t raw_len, ServerConfig* config);

#endif

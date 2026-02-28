#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "server.h"

static const char* log_level_name(int level) {
    switch (level) {
        case LOG_ERROR: return "ERROR";
        case LOG_WARN:  return "WARN";
        case LOG_INFO:  return "INFO";
        case LOG_DEBUG: return "DEBUG";
        case LOG_TRACE: return "TRACE";
        default:        return "UNKNOWN";
    }
}

static int parse_log_level(const char* s) {
    if (strcmp(s, "error") == 0) return LOG_ERROR;
    if (strcmp(s, "warn") == 0)  return LOG_WARN;
    if (strcmp(s, "info") == 0)  return LOG_INFO;
    if (strcmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcmp(s, "trace") == 0) return LOG_TRACE;
    return atoi(s);
}

void log_message(ServerConfig* config, int level, const char* format, ...) {
    if (config->log_level == 0 || config->log_path[0] == '\0') {
        return;
    }

    if (level > config->log_level) {
        return;
    }

    FILE* f;
    if (strcmp(config->log_path, "stdout") == 0) {
        f = stdout;
    } else if (strcmp(config->log_path, "stderr") == 0) {
        f = stderr;
    } else {
        f = fopen(config->log_path, "a");
        if (!f) return;
    }

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(f, "[%s] %s: ", timestamp, log_level_name(level));

    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);

    fprintf(f, "\n");
    fflush(f);

    if (f != stdout && f != stderr) {
        fclose(f);
    }
}

static FILE* try_open_config(const char* path) {
    return fopen(path, "r");
}

int load_config(const char* path, ServerConfig* config) {
    config->listen = 8080;
    strcpy(config->interface, "0.0.0.0");
    strcpy(config->root, "webdocs");
    config->log_path[0] = '\0';
    config->log_level = 0;
    config->redirect_count = 0;
    config->plugin_count = 0;
    config->require_extensions = 1;
    config->cache_max_age = 3600;
    config->cached_time = 300;  // Default: 5 minutes
    config->file_cache = NULL;

    FILE* f = try_open_config("/etc/hs-http/config");
    if (!f) {
        f = try_open_config("config");
    }
    if (!f) {
        fprintf(stderr, "Warning: config file not found, using defaults\n");
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        char* cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char* key = strtok(line, " =");
        if (!key) continue;

        if (strcmp(key, "plugin") == 0) {
            char* so_path = strtok(NULL, " ");
            char* endpoint = strtok(NULL, " ");
            if (!so_path || !endpoint) {
                continue;
            }
            if (config->plugin_count >= MAX_PLUGINS) {
                continue;
            }
            Plugin* p = &config->plugins[config->plugin_count++];
            strncpy(p->so_path, so_path, sizeof(p->so_path) - 1);
            strncpy(p->endpoint, endpoint, sizeof(p->endpoint) - 1);
            p->handle = NULL;
            p->plugin_info = NULL;
        } else if (strcmp(key, "redirect") == 0) {
            char* path = strtok(NULL, " ");
            char* target = strtok(NULL, " ");
            char* proto = strtok(NULL, " ");
            if (!path || !target || !proto || strcmp(proto, "tcp") != 0) {
                continue;
            }
            if (config->redirect_count >= MAX_REDIRECTS) {
                continue;
            }
            RedirectRule* r = &config->redirects[config->redirect_count++];
            strncpy(r->path_prefix, path, sizeof(r->path_prefix) - 1);
            char* colon = strchr(target, ':');
            if (!colon) {
                config->redirect_count--;
                continue;
            }
            *colon = '\0';
            strncpy(r->target_host, target, sizeof(r->target_host) - 1);
            r->target_port = atoi(colon + 1);
        } else {
            char* value = strtok(NULL, " ");
            if (!value) continue;
            
            if (strcmp(key, "listen") == 0) {
                config->listen = atoi(value);
            } else if (strcmp(key, "interface") == 0) {
                strncpy(config->interface, value, sizeof(config->interface) - 1);
            } else if (strcmp(key, "root") == 0) {
                strncpy(config->root, value, sizeof(config->root) - 1);
            } else if (strcmp(key, "cache_max_age") == 0) {
                config->cache_max_age = atoi(value);
            } else if (strcmp(key, "cached_time") == 0) {
                config->cached_time = atoi(value);
            } else if (strcmp(key, "require_extensions") == 0) {
                if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                    config->require_extensions = 1;
                } else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
                    config->require_extensions = 0;
                }
            } else if (strcmp(key, "log") == 0) {
                char* level = strtok(NULL, " ");
                strncpy(config->log_path, value, sizeof(config->log_path) - 1);
                if (level) {
                    config->log_level = parse_log_level(level);
                }
            }
        }
    }

    fclose(f);
    return 0;
}

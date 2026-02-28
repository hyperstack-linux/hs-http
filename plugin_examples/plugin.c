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
    const char* response = "Hello from plugin index!";
    api->send_response(req->client_fd, 200, "OK", "text/plain",
                      (const unsigned char*)response, strlen(response));
    return 0;
}

int handle_users(PluginRequest* req) {
    const char* response = "{\"users\": [\"alice\", \"bob\"]}";
    api->send_response(req->client_fd, 200, "OK", "application/json",
                      (const unsigned char*)response, strlen(response));
    return 0;
}

int handle_query(PluginRequest* req) {
    char response[2048] = "Query params from C:\n";

    for (int i = 0; i < req->query_param_count; i++) {
        strcat(response, req->query_params[i].key);
        strcat(response, " = ");
        strcat(response, req->query_params[i].value);
        strcat(response, "\n");
    }

    const char* id = api->get_query_param(req, "id");
    if (id) {
        strcat(response, "\nID parameter: ");
        strcat(response, id);
    }

    api->send_response(req->client_fd, 200, "OK", "text/plain",
                      (const unsigned char*)response, strlen(response));
    return 0;
}

int handle_post(PluginRequest* req) {
    const char* ct = api->get_header(req, "Content-Type");
    char response[4096];
    snprintf(response, sizeof(response), "Received %u bytes (Content-Type: %s)\n",
             req->body_len, ct ? ct : "(none)");
    if (req->body && req->body_len > 0) {
        size_t copy_len = req->body_len;
        if (copy_len > sizeof(response) - strlen(response) - 1) copy_len = sizeof(response) - strlen(response) - 1;
        strncat(response, (const char*)req->body, copy_len);
    }
    api->send_response(req->client_fd, 200, "OK", "text/plain",
                      (const unsigned char*)response, strlen(response));
        api->log(LOG_INFO, "Handled POST request with %s auth", req->query_params[0].value);

    return 0;
}

int handle_put(PluginRequest* req) {
    return handle_post(req);
}

int handle_patch(PluginRequest* req) {
    return handle_post(req);
}

int handle_delete(PluginRequest* req) {
    const char* response = "Deleted (simulated)";
    api->send_response(req->client_fd, 200, "OK", "text/plain",
                      (const unsigned char*)response, strlen(response));
    return 0;
}

int handle_options(PluginRequest* req) {
    const char* header = "HTTP/1.1 204 No Content\r\nAllow: GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS, TRACE\r\nConnection: close\r\n\r\n";
    api->send_response(req->client_fd, 204, "No Content", "text/plain",
                      (const unsigned char*)"", 0);
    (void)header;
    return 0;
}

int handle_trace(PluginRequest* req) {
    // Echo the request-line and headers in the body
    char buf[8192];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s %s %s\n", req->method, req->full_path, req->version);
    for (int i = 0; i < req->header_count; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s: %s\n", req->headers[i].key, req->headers[i].value);
    }
    api->send_response(req->client_fd, 200, "OK", "message/http", (const unsigned char*)buf, pos);
    return 0;
}

int handle_connect(PluginRequest* req) {
    const char* response = "CONNECT not implemented";
    api->send_response(req->client_fd, 501, "Not Implemented", "text/plain",
                      (const unsigned char*)response, strlen(response));
    return 0;
}

void cleanup(void) {
    api->log(LOG_INFO, "My plugin cleanup");
}

PluginEndpoint endpoints[] = {
    { "/", handle_index, "GET" },
    { "/users", handle_users, "GET" },
    { "/query", handle_query, NULL },
    { "/echo", handle_post, "POST" },
    { "/echo", handle_put, "PUT" },
    { "/echo", handle_patch, "PATCH" },
    { "/resource", handle_delete, "DELETE" },
    { "/options", handle_options, "OPTIONS" },
    { "/trace", handle_trace, "TRACE" },
    { "/connect", handle_connect, "CONNECT" }
};

PLUGIN_INIT("my-plugin", init, cleanup, endpoints);
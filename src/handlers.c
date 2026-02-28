#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "server.h"
#include "400.h"
#include "403.h"
#include "404.h"
#include "405.h"
#include "500.h"

static unsigned char* read_custom_error_page(const char* root, const char* request_path, 
                                              int status_code, unsigned int* out_len) {
    // Try to find custom error page in the directory of the request
    char dir_path[2048];
    char error_file[2048];
    
    // Extract directory from request path
    strncpy(dir_path, request_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    
    char* last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0';  // Keep the trailing slash
    } else {
        strcpy(dir_path, "/");
    }
    
    // Build path to custom error page
    snprintf(error_file, sizeof(error_file), "%s%s%d.html", root, dir_path, status_code);
    
    struct stat st;
    if (stat(error_file, &st) < 0) {
        // Try in root directory as fallback
        snprintf(error_file, sizeof(error_file), "%s/%d.html", root, status_code);
        if (stat(error_file, &st) < 0) {
            return NULL;
        }
    }
    
    int fd = open(error_file, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    
    unsigned char* content = malloc(st.st_size);
    if (!content) {
        close(fd);
        return NULL;
    }
    
    ssize_t bytes_read = read(fd, content, st.st_size);
    close(fd);
    
    if (bytes_read != st.st_size) {
        free(content);
        return NULL;
    }
    
    *out_len = st.st_size;
    return content;
}

void handle_error(int client_fd, int status_code, const char* status_text,
                  const char* root, const char* request_path) {
    unsigned int custom_len = 0;
    unsigned char* custom_content = NULL;
    
    if (root && request_path) {
        custom_content = read_custom_error_page(root, request_path, status_code, &custom_len);
    }
    
    if (custom_content) {
        send_response(client_fd, status_code, status_text, "text/html", custom_content, custom_len);
        free(custom_content);
        return;
    }
    
    // Fallback to built-in error pages
    switch (status_code) {
        case 400:
            send_response(client_fd, 400, "Bad Request", "text/html",
                         ___src_errors_400_html, ___src_errors_400_html_len);
            break;
        case 403:
            send_response(client_fd, 403, "Forbidden", "text/html",
                         ___src_errors_403_html, ___src_errors_403_html_len);
            break;
        case 404:
            send_response(client_fd, 404, "Not Found", "text/html",
                         ___src_errors_404_html, ___src_errors_404_html_len);
            break;
        case 405:
            send_response(client_fd, 405, "Method Not Allowed", "text/html",
                         ___src_errors_405_html, ___src_errors_405_html_len);
            break;
        case 500:
            send_response(client_fd, 500, "Internal Server Error", "text/html",
                         ___src_errors_500_html, ___src_errors_500_html_len);
            break;
        default:
            send_response(client_fd, status_code, status_text, "text/plain",
                         (const unsigned char*)"", 0);
            break;
    }
}

void handle_400(int client_fd) {
    send_response(client_fd, 400, "Bad Request", "text/html", 
                  ___src_errors_400_html, ___src_errors_400_html_len);
}

void handle_403(int client_fd) {
    send_response(client_fd, 403, "Forbidden", "text/html", 
                  ___src_errors_403_html, ___src_errors_403_html_len);
}

void handle_404(int client_fd) {
    send_response(client_fd, 404, "Not Found", "text/html", 
                  ___src_errors_404_html, ___src_errors_404_html_len);
}

void handle_405(int client_fd) {
    send_response(client_fd, 405, "Method Not Allowed", "text/html", 
                  ___src_errors_405_html, ___src_errors_405_html_len);
}

void handle_500(int client_fd) {
    send_response(client_fd, 500, "Internal Server Error", "text/html", 
                  ___src_errors_500_html, ___src_errors_500_html_len);
}

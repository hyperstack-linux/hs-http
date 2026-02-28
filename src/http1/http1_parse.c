#include "../include/http1.h"
#include "../include/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int http1_parse_request(Connection *conn, char *method, char *path, char *version,
                       char *http2_settings_b64, int *is_upgrade_h2c) {
  *is_upgrade_h2c = 0;
  http2_settings_b64[0] = '\0';

  char *line = strtok(conn->read_buffer, "\r\n");
  if (!line || sscanf(line, "%15s %1023s %15s", method, path, version) != 3) {
    return -1;
  }

  while ((line = strtok(NULL, "\r\n")) != NULL && strlen(line) > 0) {
    if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
      char *start = line + 18;
      while (*start == ' ')
        start++;
      strncpy(conn->if_modified_since, start, sizeof(conn->if_modified_since) - 1);
      char *s = conn->if_modified_since;
      while (*s == ' ')
        s++;
      if (s != conn->if_modified_since)
        memmove(conn->if_modified_since, s, strlen(s) + 1);
    } else if (strncasecmp(line, "If-None-Match:", 14) == 0) {
      char *start = line + 14;
      while (*start == ' ')
        start++;
      strncpy(conn->if_none_match, start, sizeof(conn->if_none_match) - 1);
      char *s = conn->if_none_match;
      while (*s == ' ')
        s++;
      if (s != conn->if_none_match)
        memmove(conn->if_none_match, s, strlen(s) + 1);
    } else if (strncasecmp(line, "Upgrade:", 8) == 0) {
      char *val = line + 8;
      while (*val == ' ')
        val++;
      if (strncasecmp(val, "h2c", 3) == 0)
        *is_upgrade_h2c = 1;
    } else if (strncasecmp(line, "HTTP2-Settings:", 15) == 0) {
      char *val = line + 15;
      while (*val == ' ')
        val++;
      strncpy(http2_settings_b64, val, 511);
    }
  }

  return 0;
}

#include "../include/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

CacheEntry *find_cache_entry(const char *path, struct stat *st,
                             ServerConfig *config) {
  if (!config->file_cache)
    return NULL;
  FileCache *cache = config->file_cache;
  time_t now = time(NULL);

  for (int i = 0; i < cache->count; i++) {
    CacheEntry *entry = &cache->entries[i];
    if (strcmp(entry->path, path) == 0) {
      if (entry->mtime == st->st_mtime && entry->ino == st->st_ino) {
        if ((now - entry->cached_at) > config->cached_time) {
          free(entry->content);
          for (int j = i; j < cache->count - 1; j++) {
            cache->entries[j] = cache->entries[j + 1];
          }
          cache->count--;
          return NULL;
        }
        return entry;
      } else {
        free(entry->content);
        for (int j = i; j < cache->count - 1; j++) {
          cache->entries[j] = cache->entries[j + 1];
        }
        cache->count--;
        return NULL;
      }
    }
  }
  return NULL;
}

void add_cache_entry(ServerConfig *config, const char *path, struct stat *st,
                     unsigned char *content, const char *mime, const char *etag,
                     const char *last_modified) {
  if (!config->file_cache)
    return;
  FileCache *cache = config->file_cache;

  if (st->st_size > MAX_CACHE_FILE_SIZE) {
    return;
  }

  if (cache->count >= MAX_CACHE_ENTRIES) {
    free(cache->entries[0].content);
    for (int i = 0; i < cache->count - 1; i++) {
      cache->entries[i] = cache->entries[i + 1];
    }
    cache->count--;
  }

  CacheEntry *entry = &cache->entries[cache->count];
  strncpy(entry->path, path, sizeof(entry->path) - 1);
  entry->content = content;
  entry->size = st->st_size;
  entry->mtime = st->st_mtime;
  entry->ino = st->st_ino;
  strncpy(entry->etag, etag, sizeof(entry->etag) - 1);
  strncpy(entry->last_modified, last_modified,
          sizeof(entry->last_modified) - 1);
  strncpy(entry->content_type, mime, sizeof(entry->content_type) - 1);
  entry->cached_at = time(NULL);

  cache->count++;
}

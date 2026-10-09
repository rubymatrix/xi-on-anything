/* Files over HTTP for the browser build (httpfs_web.c): the game install under /game/ and host64's
 * own files under /app/, from the local server. Off (httpfs_owns is 0) unless XI_HTTP_URL is set. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct HttpFile HttpFile;
typedef struct HttpDir HttpDir;

int httpfs_owns(const char* path); /* under a mount: these functions answer for it */
int httpfs_stat(const char* path, int64_t* size, int* is_dir);
HttpFile* httpfs_open(const char* path); /* read only; NULL if missing or a folder */
int64_t httpfs_read(HttpFile* f, void* buf, uint32_t n);
int64_t httpfs_seek(HttpFile* f, int64_t offset, int whence); /* 0 set, 1 current, 2 end */
int64_t httpfs_size(HttpFile* f);
void httpfs_close(HttpFile* f);
unsigned char* httpfs_read_all(const char* path, size_t* size);
HttpDir* httpfs_dir_open(const char* path);
const char* httpfs_dir_next(HttpDir* d);
void httpfs_dir_close(HttpDir* d);

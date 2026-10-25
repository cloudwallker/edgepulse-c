#ifndef EP_FILE_STORE_H
#define EP_FILE_STORE_H
#include "ep_store.h"
typedef enum { EP_FILE_OK, EP_FILE_NOT_FOUND, EP_FILE_BUSY, EP_FILE_FORMAT, EP_FILE_IO } ep_file_status;
typedef struct {
    ep_store *store;
#ifdef _WIN32
    void *handle;
#else
    int fd;
#endif
    bool healthy;
    ep_file_status status;
} ep_file_store;
/* UTF-8 path. Existing files require exactly 48 bytes and a valid v1 slot.
 * create=true may exclusively create a new file with a valid default record.
 * Open owns an exclusive lock; no symlinks/reparse points/nonregular files.
 * On failure the supplied store is not changed. Do not reopen an open handle.
 * The file object must outlive the bound store; close leaves saves disabled. */
ep_file_status ep_file_store_open(ep_file_store *file, ep_store *store, const char *path, bool create);
void ep_file_store_close(ep_file_store *file);
const char *ep_file_status_string(ep_file_status status);
#endif

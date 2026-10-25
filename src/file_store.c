/* Host-only backend: the portable device core still has no OS dependencies. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "ep_file_store.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef EP_FILE_TESTING
/* Implemented only by tests/file_store_worker.c. No production test switches. */
extern int ep_file_test_event(const char *stage, size_t step);
static size_t read_step, write_step, sync_step;
static void reset_events(void) { read_step=0; write_step=0; sync_step=0; }
static int io_event(const char *stage) {
    size_t *step=strcmp(stage,"read")==0 ? &read_step :
                 strcmp(stage,"write")==0 ? &write_step : &sync_step;
    return ep_file_test_event(stage,(*step)++);
}
static bool boundary(const char *stage,size_t step) { return ep_file_test_event(stage,step)!=1; }
#else
static void reset_events(void) { }
static int io_event(const char *stage) { (void)stage; return 0; }
static bool boundary(const char *stage,size_t step) { (void)stage; (void)step; return true; }
#endif

static bool read_bytes(ep_file_store *file,size_t offset,uint8_t *bytes,size_t length) {
    while (length) {
        int action=io_event("read"); size_t requested=action==2 ? 1u : length;
        if (action==1) return false;
#ifdef _WIN32
        LARGE_INTEGER position; DWORD received=0;
        position.QuadPart=(LONGLONG)offset;
        if (!SetFilePointerEx((HANDLE)file->handle,position,NULL,FILE_BEGIN) ||
            !ReadFile((HANDLE)file->handle,bytes,(DWORD)requested,&received,NULL) || !received) return false;
        size_t count=(size_t)received;
#else
        ssize_t received=pread(file->fd,bytes,requested,(off_t)offset);
        if (received<0 && errno==EINTR) continue;
        if (received<=0) return false;
        size_t count=(size_t)received;
#endif
        bytes+=count; offset+=count; length-=count;
    }
    return true;
}

static bool write_bytes(ep_file_store *file,size_t offset,const uint8_t *bytes,size_t length) {
    while (length) {
        int action=io_event("write"); size_t requested=action==2 ? 1u : length;
        if (action==1) return false;
#ifdef _WIN32
        LARGE_INTEGER position; DWORD written=0;
        position.QuadPart=(LONGLONG)offset;
        if (!SetFilePointerEx((HANDLE)file->handle,position,NULL,FILE_BEGIN) ||
            !WriteFile((HANDLE)file->handle,bytes,(DWORD)requested,&written,NULL) || !written) return false;
        size_t count=(size_t)written;
#else
        ssize_t written=pwrite(file->fd,bytes,requested,(off_t)offset);
        if (written<0 && errno==EINTR) continue;
        if (written<=0) return false;
        size_t count=(size_t)written;
#endif
        bytes+=count; offset+=count; length-=count;
    }
    return true;
}

static bool sync_file(ep_file_store *file) {
    if (io_event("sync")==1) return false;
#ifdef _WIN32
    return FlushFileBuffers((HANDLE)file->handle)!=0;
#else
    int result;
    do { result=fsync(file->fd); } while (result<0 && errno==EINTR);
    return result==0;
#endif
}

static bool commit_record(void *ctx,size_t target,const uint8_t record[EP_SLOT_SIZE]) {
    ep_file_store *file=ctx; const uint8_t invalid=0; size_t offset=target*EP_SLOT_SIZE;
    if (!file->healthy || target>1u) return false;
    reset_events();
    if (!write_bytes(file,offset+23u,&invalid,1u) || !sync_file(file) || !boundary("clear",0)) goto fail;
    /* Byte writes also make interrupted writes observable at every record byte.
     * This is a small, infrequently changed configuration, not a bulk I/O path. */
    for (size_t i=0;i<23u;i++) {
        if (!write_bytes(file,offset+i,record+i,1u) || !boundary("payload",i+1u)) goto fail;
    }
    if (!sync_file(file) || !boundary("payload-sync",0)) goto fail;
    if (!write_bytes(file,offset+23u,record+23u,1u) || !boundary("commit",0) ||
        !sync_file(file) || !boundary("committed",0)) goto fail;
    return true;
fail:
    /* The final write may already be visible even when synchronization failed.
     * Stop writes until reopened; the caller must not report success. */
    file->healthy=false; file->status=EP_FILE_IO; return false;
}

void ep_file_store_close(ep_file_store *file) {
    if (!file) return;
#ifdef _WIN32
    if (file->handle && file->handle!=INVALID_HANDLE_VALUE) CloseHandle((HANDLE)file->handle);
    file->handle=INVALID_HANDLE_VALUE;
#else
    if (file->fd>=0) (void)close(file->fd);
    file->fd=-1;
#endif
    /* Keep the callback bound: saves after close must fail, not become RAM-only. */
    file->healthy=false;
}

static ep_file_status open_failure(ep_file_store *file,ep_file_status status) {
    ep_file_store_close(file); file->status=status; return status;
}

ep_file_status ep_file_store_open(ep_file_store *file,ep_store *store,const char *path,bool create) {
    ep_store loaded; ep_config config; uint32_t generation; bool created=false;
    if (!file || !store || !path || !*path) return EP_FILE_IO;
    memset(file,0,sizeof(*file)); file->status=EP_FILE_IO;
#ifdef _WIN32
    file->handle=INVALID_HANDLE_VALUE;
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    if (!count) return EP_FILE_IO;
    wchar_t *wide=malloc((size_t)count*sizeof(*wide));
    if (!wide) return EP_FILE_IO;
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,count)) { free(wide); return EP_FILE_IO; }
    HANDLE handle=CreateFileW(wide,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    DWORD error=handle==INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (handle==INVALID_HANDLE_VALUE && error==ERROR_FILE_NOT_FOUND && create) {
        handle=CreateFileW(wide,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        error=handle==INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        created=handle!=INVALID_HANDLE_VALUE;
    }
    free(wide);
    if (handle==INVALID_HANDLE_VALUE) {
        if (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) file->status=EP_FILE_NOT_FOUND;
        else if (error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION || error==ERROR_FILE_EXISTS) file->status=EP_FILE_BUSY;
        return file->status;
    }
    file->handle=handle;
    BY_HANDLE_FILE_INFORMATION info; LARGE_INTEGER size;
    if (GetFileType(handle)!=FILE_TYPE_DISK || !GetFileInformationByHandle(handle,&info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))!=0 || !GetFileSizeEx(handle,&size))
        return open_failure(file,EP_FILE_FORMAT);
    if (!created && size.QuadPart!=48) return open_failure(file,EP_FILE_FORMAT);
#else
    file->fd=-1;
    int flags=O_RDWR|O_NONBLOCK|O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags|=O_CLOEXEC;
#endif
    int fd=open(path,flags);
    if (fd<0 && errno==ENOENT && create) {
        fd=open(path,flags|O_CREAT|O_EXCL,0600);
        created=fd>=0;
    }
    if (fd<0) {
        if (errno==ENOENT) file->status=EP_FILE_NOT_FOUND;
        else if (errno==EEXIST) file->status=EP_FILE_BUSY;
        else if (errno==ELOOP) file->status=EP_FILE_FORMAT;
        return file->status;
    }
    file->fd=fd;
    if (flock(fd,LOCK_EX|LOCK_NB)<0)
        return open_failure(file,(errno==EWOULDBLOCK || errno==EAGAIN) ? EP_FILE_BUSY : EP_FILE_IO);
    struct stat info;
    if (fstat(fd,&info)<0) return open_failure(file,EP_FILE_IO);
    if (!S_ISREG(info.st_mode) || (!created && info.st_size!=48)) return open_failure(file,EP_FILE_FORMAT);
#endif
    reset_events(); ep_store_init(&loaded);
    if (created) {
        const ep_config defaults={100,30000};
        if (!ep_store_save(&loaded,&defaults,EP_STORE_COMPLETE) ||
            !write_bytes(file,0,(const uint8_t *)&loaded.slots,sizeof(loaded.slots)) || !sync_file(file)) return open_failure(file,EP_FILE_IO);
    } else if (!read_bytes(file,0,(uint8_t *)&loaded.slots,sizeof(loaded.slots))) return open_failure(file,EP_FILE_IO);
    if (!ep_store_load(&loaded,&config,&generation)) return open_failure(file,EP_FILE_FORMAT);
    loaded.writes=0; loaded.commit=commit_record; loaded.commit_ctx=file;
    *store=loaded; file->store=store; file->healthy=true; file->status=EP_FILE_OK;
    return EP_FILE_OK;
}

const char *ep_file_status_string(ep_file_status status) {
    switch (status) {
        case EP_FILE_OK: return "ok";
        case EP_FILE_NOT_FOUND: return "not found";
        case EP_FILE_BUSY: return "busy";
        case EP_FILE_FORMAT: return "invalid format";
        case EP_FILE_IO: return "I/O failure (result unconfirmed)";
    }
    return "unknown storage error";
}

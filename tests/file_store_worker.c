/* Test-only process driver. EP_FILE_TESTING probes never enter the product CLI. */
#include "ep_file_store.h"
#include "ep_device.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const char *probe_stage;
static size_t probe_step;
static const char *probe_mode;
static bool probe_enabled;

int ep_file_test_event(const char *stage, size_t step) {
    if (!probe_enabled) return 0;
    if (strcmp(probe_mode, "short") == 0 &&
        (strcmp(stage, "write") == 0 || strcmp(stage, "read") == 0)) return 2;
    if (!probe_stage || strcmp(stage, probe_stage) != 0 || step != probe_step) return 0;
    if (strcmp(probe_mode, "fail") == 0) return 1;
    if (strcmp(probe_mode, "kill") == 0) {
        puts("READY"); fflush(stdout);
        (void)getchar();
    }
    return 0;
}

static bool sensor(void *ctx, int32_t *value) { (void)ctx; *value=30000; return true; }
static bool send_frame(void *ctx, const ep_frame *frame) { (void)ctx; (void)frame; return true; }
static void show(const ep_store *store) {
    ep_config config; uint32_t generation;
    (void)ep_store_load(store,&config,&generation);
    printf("{\"period_ms\":%" PRIu32 ",\"threshold_mc\":%" PRId32 ",\"generation\":%" PRIu32 "}\n",
           config.period_ms,config.threshold_mc,generation);
}

static int worker_main(int argc,char **argv) {
    ep_file_store file; ep_store store={0}; ep_file_status status;
    bool create; ep_config wanted={750,-5000};
    if (argc<3) return 2;
    create=strcmp(argv[1],"set")==0 || strcmp(argv[1],"short")==0;
    if (strcmp(argv[1],"short")==0) { probe_enabled=true; probe_mode="short"; }
    if (strcmp(argv[1],"openfail")==0) {
        probe_enabled=true; probe_mode="fail"; probe_stage="read"; probe_step=0;
    }
    status=ep_file_store_open(&file,&store,argv[2],create);
    if (status!=EP_FILE_OK) {
        fprintf(stderr,"store error: %s\n",ep_file_status_string(status));
        return 1;
    }
    if (strcmp(argv[1],"show")==0) { show(&store); ep_file_store_close(&file); return 0; }
    if (strcmp(argv[1],"hold")==0) {
        puts("READY"); fflush(stdout); (void)getchar(); ep_file_store_close(&file); return 0;
    }
    if (strcmp(argv[1],"closed")==0) {
        ep_file_store_close(&file);
        if (ep_store_save(&store,&wanted,EP_STORE_COMPLETE)) return 1;
        puts("closed_save_rejected"); return 0;
    }
    if (strcmp(argv[1],"probe")==0) {
        ep_device device; ep_hal hal={sensor,send_frame,NULL}; ep_config before;
        uint8_t slots_before[2][EP_SLOT_SIZE]; ep_command_result result;
        if (argc!=6) { ep_file_store_close(&file); return 2; }
        ep_device_init(&device,&store,hal,7,0);
        before=device.config; memcpy(slots_before,store.slots,sizeof(slots_before));
        probe_stage=argv[3]; probe_step=(size_t)strtoul(argv[4],NULL,10); probe_mode=argv[5];
        probe_enabled=true;
        result=ep_device_configure(&device,77,wanted,EP_STORE_COMPLETE);
        if (strcmp(probe_mode,"fail")==0) {
            bool cache_empty=true;
            for (size_t i=0;i<EP_COMMAND_WINDOW;i++) if (device.commands[i].valid) cache_empty=false;
            if (result!=EP_CMD_STORAGE || file.healthy || device.config.period_ms!=before.period_ms ||
                device.config.threshold_mc!=before.threshold_mc || device.metrics.commands_applied!=0 ||
                !cache_empty || memcmp(slots_before,store.slots,sizeof(slots_before))!=0 ||
                ep_device_configure(&device,77,wanted,EP_STORE_COMPLETE)!=EP_CMD_STORAGE) {
                ep_file_store_close(&file); return 1;
            }
            ep_file_store_close(&file); puts("failure_verified"); return 0;
        }
        if (result!=EP_CMD_OK) { ep_file_store_close(&file); return 1; }
    } else if (strcmp(argv[1],"set")==0 || strcmp(argv[1],"short")==0) {
        if (argc!=5) { ep_file_store_close(&file); return 2; }
        wanted.period_ms=(uint32_t)strtoul(argv[3],NULL,10);
        wanted.threshold_mc=(int32_t)strtol(argv[4],NULL,10);
        if (!ep_store_save(&store,&wanted,EP_STORE_COMPLETE)) { ep_file_store_close(&file); return 1; }
    } else { ep_file_store_close(&file); return 2; }
    show(&store); ep_file_store_close(&file); return 0;
}

#ifdef _WIN32
int wmain(int argc,wchar_t **wide) {
    char **args=calloc((size_t)argc,sizeof(*args)); int result=1;
    if (!args) return 1;
    for (int i=0;i<argc;i++) {
        int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,NULL,0,NULL,NULL);
        if (!bytes || !(args[i]=malloc((size_t)bytes))) goto cleanup;
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,args[i],bytes,NULL,NULL)) goto cleanup;
    }
    result=worker_main(argc,args);
cleanup:
    for (int i=0;i<argc;i++) free(args[i]);
    free(args); return result;
}
#else
int main(int argc,char **argv) { return worker_main(argc,argv); }
#endif

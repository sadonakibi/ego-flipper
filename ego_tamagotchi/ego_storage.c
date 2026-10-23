#include "ego_storage.h"
#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>

#define EGO_SAVE_PATH APP_DATA_PATH("ego.save")
#define EGO_SAVE_MAGIC 0x45474F31UL // "EGO1"
#define EGO_SAVE_VERSION 12 // 12: нагрузка заданий, отжимания и приседания отдельно, модуль контроля (старые сохранения отбрасываются)
#define EGO_LOG_PATH APP_DATA_PATH("ego_log.csv")

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    EgoState state;
} EgoSave;

bool ego_storage_load(EgoState* state) {
    bool ok = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, EGO_SAVE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        EgoSave save;
        if(storage_file_read(file, &save, sizeof(save)) == sizeof(save) &&
           save.magic == EGO_SAVE_MAGIC && save.version == EGO_SAVE_VERSION &&
           save.size == sizeof(EgoState)) {
            *state = save.state;
            ok = true;
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool ego_storage_save(const EgoState* state) {
    bool ok = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH());
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, EGO_SAVE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        EgoSave save = {
            .magic = EGO_SAVE_MAGIC,
            .version = EGO_SAVE_VERSION,
            .size = sizeof(EgoState),
            .state = *state,
        };
        ok = storage_file_write(file, &save, sizeof(save)) == sizeof(save);
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

void ego_log(uint32_t ts, const char* event, int32_t id, int32_t delta, uint8_t ego) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH());
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, EGO_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        if(storage_file_size(file) == 0) {
            const char* header = "ts,event,id,delta,ego\n";
            storage_file_write(file, header, strlen(header));
        }
        char line[96];
        int n = snprintf(line, sizeof(line), "%lu,%s,%ld,%ld,%u\n", ts, event, id, delta, ego);
        if(n > 0) storage_file_write(file, line, (size_t)n);
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

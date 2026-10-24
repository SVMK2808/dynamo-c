#include "../include/storage.h"
#include "../include/ring.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void free_entry(storage_entry_t *entry){
    if(!entry) return;
    free(entry -> key);
    for(size_t i = 0; i < entry -> version_count; i++){
        free(entry -> versions[i].val);
        vclock_free(&entry -> versions[i].vclock);
    }

    free(entry -> versions);
    free(entry);
}

static storage_entry_t *find_entry(const storage_engine_t *engine, const void *key, size_t key_len, size_t *out_bucket_idx){
    if(!engine || !key || key_len == 0) return NULL;

    uint64_t hash = hash_key(key, key_len);
    size_t b = hash % engine -> num_buckets;
    if(out_bucket_idx) *out_bucket_idx = b;

    storage_entry_t *curr = engine -> buckets[b];
    while(curr){
        if(curr -> key_len == key_len && memcmp(curr -> key, key, key_len) == 0){
            return curr;
        }

        curr = curr -> next;
    }
    return NULL;
}

const storage_entry_t *storage_get(const storage_engine_t *engine, const void *key, size_t key_len){
    return find_entry(engine, key, key_len, NULL);
}

bool storage_delete(storage_engine_t *engine, const void *key, size_t key_len){
    if(!engine || !key || key_len == 0) return false;


    uint64_t hash = hash_key(key, key_len);
    size_t b = hash % engine -> num_buckets;
    storage_entry_t *prev = NULL;
    storage_entry_t *curr = engine -> buckets[b];
    while(curr){
        if(curr -> key_len == key_len && memcmp(curr -> key, key, key_len) == 0){
            if(!prev){
                engine -> buckets[b] = curr -> next;
            }else{
                prev -> next = curr -> next;
            }
            free_entry(curr);
            engine -> total_keys--;
            return true;
        }
        prev = curr;
        curr = curr -> next;
    }
    return false;
}

int storage_init(storage_engine_t *engine, size_t num_buckets){
    if(!engine) return -1;
    if(num_buckets == 0) num_buckets = DEFAULT_STORAGE_BUCKETS;

    engine -> buckets = (storage_entry_t **) calloc(num_buckets, sizeof(storage_entry_t *));
    if(!engine -> buckets) return -1;

    engine -> num_buckets = num_buckets;
    engine -> total_keys = 0;
    return 0;
}

void storage_free(storage_engine_t *engine){
    if(!engine || !engine -> buckets) return;

    for(size_t b = 0; b < engine -> num_buckets; b++){
        storage_entry_t *curr = engine -> buckets[b];
        while(curr){
            storage_entry_t *next = curr -> next;
            free_entry(curr);
            curr = next;
        }
    }

    free(engine -> buckets);
    engine -> buckets = NULL;
    engine -> num_buckets = 0;
    engine -> total_keys = 0;
    
}

int storage_put(storage_engine_t *engine, const void *key, size_t key_len, const void *val, size_t val_len, const vclock_t *vclock){
    if(!engine || !key || key_len == 0 || !val || val_len == 0 || !vclock){
        return -1;
    }

    size_t b = 0;
    storage_entry_t *entry = find_entry(engine, key, key_len, &b);

    if(!entry){
        // New entry 
        entry = (storage_entry_t *)malloc(sizeof(storage_entry_t));
        entry -> key = malloc(key_len);
        memcpy(entry -> key, key, key_len);
        entry -> key_len = key_len;

        entry -> versions = (storage_version_t *) malloc(2 * sizeof(storage_version_t));
        entry -> version_capacity = 2;
        entry -> version_count = 1;
        
        entry -> versions[0].val = malloc(val_len);
        memcpy(entry -> versions[0].val, val, val_len);
        entry -> versions[0].val_len = val_len;
        vclock_init(&entry -> versions[0].vclock);
        vclock_copy(&entry -> versions[0].vclock, vclock);

        entry -> next = engine -> buckets[b];
        engine -> buckets[b] = entry;
        engine -> total_keys++;
        return 0;
    }else{
        for(size_t i = 0; i < entry -> version_count; i++){
            vclock_rel_t rel = vclock_compare(vclock, &entry -> versions[i].vclock);
            if(rel == VC_ANCESTOR || rel == VC_EQUAL){
                return 0; // Incoming write is obsolete or duplicate - safely ignore
            }
        }

        size_t write_idx = 0;
        for(size_t read_idx = 0; read_idx < entry -> version_count; read_idx++){
            vclock_rel_t rel = vclock_compare(vclock, &entry -> versions[read_idx].vclock);
            if(rel == VC_DESCENDANT){
                // Old version is superceded: free it
                free(entry -> versions[read_idx].val);
                vclock_free(&entry -> versions[read_idx].vclock);

            } else {
                entry -> versions[write_idx++] = entry -> versions[read_idx];
            }
        }
        entry -> version_count = write_idx;

        if(entry -> version_count >= entry -> version_capacity){
            size_t new_cap = (entry -> version_capacity == 0) ? 2 : entry -> version_capacity * 2;
            storage_version_t *new_versions = (storage_version_t *) realloc(entry -> versions, new_cap * sizeof(storage_version_t));
            if(!new_versions) return -1;
            entry -> versions = new_versions;
            entry -> version_capacity = new_cap; 
        }

        size_t idx = entry -> version_count;
        entry -> versions[idx].val = malloc(val_len);
        if(!entry -> versions[idx].val) return -1;
        memcpy(entry -> versions[idx].val, val, val_len);
        entry -> versions[idx].val_len = val_len;

        vclock_init(&entry -> versions[idx].vclock);
        vclock_copy(&entry -> versions[idx].vclock, vclock);
        entry -> version_count++;
        
        return 0;
    }
}
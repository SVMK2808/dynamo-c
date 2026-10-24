#include "../include/hint.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int hint_store_init(hint_store_t *store){
    if(!store) return -1;
    
    store -> hints = NULL;
    store -> count = 0;
    store -> capacity = 0;
    if(pthread_mutex_init(&store -> lock, NULL) != 0){
        return -1;
    }

    return 0;
}

void hint_store_free(hint_store_t *store){
    if(!store) return;

    pthread_mutex_lock(&store -> lock);
    for(size_t i = 0; i < store -> count; i++){
        if(store -> hints[i].key) free(store -> hints[i].key);
        if(store -> hints[i].val) free(store -> hints[i].val);
        vclock_free(&store -> hints[i].vclock);
    }

    free(store -> hints);
    store -> hints = NULL;
    store -> count = 0; 
    store -> capacity = 0;
    pthread_mutex_unlock(&store -> lock);

    pthread_mutex_destroy(&store -> lock);
}

int hint_store_add(hint_store_t *store, uint32_t target_node_id, 
                   const void *key, size_t key_len,
                   const void *val, size_t val_len,
                   const vclock_t *vc){

    if(!store || !key || key_len == 0 || !val || val_len == 0) return -1;

    pthread_mutex_lock(&store -> lock);

    if(store -> count >= store -> capacity){
        size_t new_cap = (store -> capacity == 0) ? 8 : store -> capacity * 2;
        hint_entry_t *new_arr = realloc(store -> hints, sizeof(hint_entry_t) * new_cap);

        if(!new_arr){
            pthread_mutex_unlock(&store -> lock);
            return -1;
        }

        store -> hints = new_arr;
        store -> capacity = new_cap;
    }

    hint_entry_t *entry = &store -> hints[store -> count];
    entry -> target_node_id = target_node_id;

    entry -> key = malloc(key_len);
    if(!entry -> key){
        pthread_mutex_unlock(&store -> lock);
        return -1;
    }

    memcpy(entry -> key, key, key_len);
    entry -> key_len = key_len;

    entry -> val = malloc(val_len);
    if(!entry -> val){
        free(entry -> key);
        pthread_mutex_unlock(&store -> lock);
        return -1;
    }

    memcpy(entry -> val, val, val_len);
    entry -> val_len = val_len;

    vclock_init(&entry -> vclock);
    if(vc){
        vclock_copy(&entry -> vclock, vc);
    }

    store -> count++;
    pthread_mutex_unlock(&store -> lock);
    return 0;
}

size_t hint_store_count(hint_store_t *store){
    if(!store){
        return 0;
    }

    pthread_mutex_lock(&store -> lock);
    size_t c = store -> count;
    pthread_mutex_unlock(&store -> lock);
    return c;
}

size_t hint_store_count_for_target(hint_store_t *store, uint32_t target_node_id){
    if(!store) return 0;
    pthread_mutex_lock(&store -> lock);
    size_t c = 0;
    for(size_t i = 0; i < store ->count; i++){
        if(store -> hints[i].target_node_id == target_node_id){
            c++;
        }
    }

    pthread_mutex_unlock(&store -> lock);
    return c;
}

int hint_store_flush_target(hint_store_t *store, uint32_t target_node_id, const char *target_ip, uint16_t target_port){
    if(!store || !target_ip || target_port == 0) return -1;

    if(hint_store_count_for_target(store, target_node_id) == 0){
        return 0; //Nothing to deliver
    }

    int fd = net_connect(target_ip, target_port);
    if(fd < 0){
        return -1; //Target node is still offline.
    }

    pthread_mutex_lock(&store -> lock);
    int delivered = 0;

    for(size_t i = 0; i < store -> count; i++){
        if(store -> hints[i].target_node_id != target_node_id){
            continue;
        }

        hint_entry_t *entry = &store -> hints[i];
        put_req_t req = {
            .key = entry -> key,
            .key_len = entry -> key_len,
            .val = entry -> val,
            .val_len = entry -> val_len,
            .vclock = entry -> vclock
        };

        size_t sz = put_req_serialised_size(&req);
        void *payload = malloc(sz);
        if(!payload) break;

        if(put_req_serialise(&req, payload, sz) == (int)sz){
            if(net_send_msg(fd, MSG_PUT_REQ, 8888, STATUS_OK, FLAG_HINTED_HANDOFF, payload, sz) == 0){
                msg_header_t resp_hdr;
                void *resp_payload = NULL;
                if(net_recv_msg(fd, &resp_hdr, &resp_payload) > 0){
                    if(resp_hdr.msg_type == MSG_PUT_RESP && resp_hdr.status == STATUS_OK){
                        delivered++;

                        //Free delivered hint resources
                        free(entry -> key);
                        free(entry -> val);
                        vclock_free(&entry -> vclock);

                        //Connect dynamic array
                        memmove(&store -> hints[i], &store -> hints[i + 1], sizeof(hint_entry_t) * (store -> count -1 -i));
                        store -> count--;
                        i--; //Re-evaluate current index
                    }
                    if(resp_payload) free(resp_payload);
                }
            }
        }
         free(payload);
    }

    close(fd);
    pthread_mutex_unlock(&store -> lock);
    return delivered;
}

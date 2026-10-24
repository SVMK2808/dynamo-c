#include "../include/gossip.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

int gossip_table_init(gossip_table_t *table, uint32_t local_node_id, const char *host, int port, uint64_t suspect_threshold, uint64_t dead_threshold){
    if(!table) return -1;

    memset(table, 0, sizeof(gossip_table_t));
    table -> local_node_id = local_node_id;
    table -> suspect_threshold = suspect_threshold ? suspect_threshold : 3;
    table -> dead_threshold = dead_threshold ? dead_threshold : 6;
    table -> current_tick = 0;

    if(pthread_mutex_init(&table -> lock, NULL) != 0){
        return -1;
    }


    //Allocate initial capacity for cluster members.
    table -> capacity = 8;
    table -> members = calloc(table -> capacity, sizeof(gossip_member_t));
    if(!table -> members){
        pthread_mutex_destroy(&table -> lock);
        return -1;
    }

    //Add local node as member 0
    gossip_member_t *self = &table -> members[0];
    self -> node_id = local_node_id;
    if(host){
        strncpy(self -> host, host, sizeof(self -> host) - 1);
    }else{
        strcpy(self -> host, "127.0.0.1");
    }

    self -> port = port;
    self -> generation = (uint64_t)time(NULL);
    self -> heartbeat = 1;
    self -> status = NODE_STATUS_ALIVE;
    self -> last_seen_tick = 0;
    table -> count = 1;

    return 0;
}

void gossip_table_free(gossip_table_t *table){
    if(!table) return;

    pthread_mutex_lock(&table -> lock);
    if(table -> members){
        free(table -> members);
        table -> members = NULL;
    }

    table -> count = 0;
    table -> capacity = 0;
    pthread_mutex_unlock(&table -> lock);
    pthread_mutex_destroy(&table -> lock);
}

void gossip_table_tick(gossip_table_t *table){
    if(!table) return;

    pthread_mutex_lock(&table -> lock);
    table -> current_tick++;

    for(size_t i = 0; i < table -> count; i++){
        gossip_member_t *m = &table -> members[i];
        if(m -> node_id == table -> local_node_id){
            //Self heartbeat always increments each round
            m -> heartbeat++;
            m -> last_seen_tick = table -> current_tick;
            m -> status = NODE_STATUS_ALIVE;
            continue;
        }

        // Failure detection for remote nodes. 
        if(m -> status == NODE_STATUS_ALIVE){
            if(table -> current_tick - m -> last_seen_tick >= table -> suspect_threshold){
                m -> status = NODE_STATUS_SUSPECT;
            }
        }

        if(m -> status == NODE_STATUS_SUSPECT){
            if(table -> current_tick - m -> last_seen_tick >= table -> dead_threshold){
                m -> status = NODE_STATUS_DEAD;
            }
        }
    }
    pthread_mutex_unlock(&table -> lock);
}

int gossip_table_add_seed(gossip_table_t *table, uint32_t node_id, const char *host, int port){
    if(!table || !host) return -1;

    pthread_mutex_lock(&table -> lock);

    //Check if already exists.
    for(size_t i = 0; i < table -> count; i++){
        if(table -> members[i].node_id == node_id){
            pthread_mutex_unlock(&table -> lock);
            return 0; //Already known
        }
    }

    // Grow dynamic array if full.
    if(table -> count >= table -> capacity){
        size_t new_cap = table -> capacity * 2;
        gossip_member_t *new_arr = realloc(table -> members, sizeof(gossip_member_t) * new_cap);
        if(!new_arr){
            pthread_mutex_unlock(&table -> lock);
            return -1;
        }

        table -> members = new_arr;
        table -> capacity = new_cap;
    }

    gossip_member_t *seed = &table -> members[table -> count++];
    memset(seed, 0, sizeof(gossip_member_t));
    seed -> node_id = node_id;
    strncpy(seed -> host, host, sizeof(seed -> host) - 1);
    seed -> port = port;
    seed -> generation = 0;
    seed -> heartbeat = 0;
    seed -> status = NODE_STATUS_ALIVE;
    seed -> last_seen_tick = table -> current_tick;

    pthread_mutex_unlock(&table -> lock);
    return 0;
}

int gossip_table_select_peer(gossip_table_t *table, gossip_member_t *out_peer){
    if(!table || !out_peer) return -1;

    pthread_mutex_lock(&table -> lock);

    //Collect eligible peers (alive or suspect, excluding self).
    size_t eligible_indices[table -> count];
    size_t eligible_count = 0;

    for(size_t i = 0; i < table -> count; i++){
        if(table -> members[i].node_id != table -> local_node_id && table -> members[i].status != NODE_STATUS_DEAD){
            eligible_indices[eligible_count++] = i;
        }
    }

    if(eligible_count == 0){
        pthread_mutex_unlock(&table -> lock);
        return -1;
    }

    // Random selection.
    size_t chosen_idx = eligible_indices[rand() % eligible_count];
    *out_peer = table -> members[chosen_idx];

    pthread_mutex_unlock(&table -> lock);
    return 0;
}

int gossip_table_merge(gossip_table_t *table, const gossip_member_t *remote_members, size_t remote_count){
    if(!table || !remote_members || remote_count == 0) return 0;

    pthread_mutex_lock(&table -> lock);
    int updates = 0;

    for(size_t r = 0; r < remote_count; r++){
        const gossip_member_t *rm = &remote_members[r];

        // Do not let remote digest override local self - heartbeat.
        if(rm -> node_id == table -> local_node_id){
            continue;
        }

        //Look for existing member.
        gossip_member_t *lm = NULL;
        for(size_t l = 0; l < table -> count; l++){
            if(table -> members[l].node_id == rm -> node_id){
                lm = &table -> members[l];
                break;
            }
        }

        if(!lm){
            // New member discovered via gossip
            if(table -> count >= table -> capacity){
                size_t new_cap = table -> capacity * 2;
                gossip_member_t *new_arr = realloc(table -> members, sizeof(gossip_member_t) * new_cap);
                if(!new_arr) continue;
                table -> members = new_arr;
                table -> capacity = new_cap;
            }

            gossip_member_t *new_m = &table -> members[table -> count++];
            *new_m = *rm;
            new_m -> last_seen_tick = table -> current_tick;
            updates++;
            continue;
        }

        //Existing member: reconcile state.
        if(rm -> generation > lm -> generation){
            //Remote has newer boot generation (node restarted)
            *lm = *rm;
            lm -> last_seen_tick = table -> current_tick;
            updates++;
        }else if(rm -> generation == lm -> generation){
            if(rm -> heartbeat > lm -> heartbeat){
                lm -> heartbeat = rm -> heartbeat;
                lm -> last_seen_tick = table -> current_tick;
                if(lm -> status == NODE_STATUS_SUSPECT || lm -> status == NODE_STATUS_DEAD){
                    lm -> status = NODE_STATUS_ALIVE;
                }
                updates++;
            }

            //Propogate DEAD status if verified.
            if(rm -> status == NODE_STATUS_DEAD && lm -> status != NODE_STATUS_DEAD){
                lm -> status = NODE_STATUS_DEAD;
                updates++;
            }
        }
    }

    pthread_mutex_unlock(&table -> lock);
    return updates;
}

node_status_t gossip_table_get_status(gossip_table_t *table, uint32_t node_id){
    if(!table) return NODE_STATUS_DEAD;

    pthread_mutex_lock(&table -> lock);
    for(size_t i = 0; i < table -> count; i++){
        if(table -> members[i].node_id == node_id){
            node_status_t st = table -> members[i].status;
            pthread_mutex_unlock(&table -> lock);
            return st;
        }
    }

    pthread_mutex_unlock(&table -> lock);
    return NODE_STATUS_DEAD;
}

int gossip_table_get_member(gossip_table_t *table, uint32_t node_id, gossip_member_t *out_member){
    if(!table || !out_member) return -1;

    pthread_mutex_lock(&table -> lock);
    for(size_t i = 0; i < table -> count; i++){
        if(table -> members[i].node_id == node_id){
            *out_member = table -> members[i];
            pthread_mutex_unlock(&table -> lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&table -> lock);
    return -1;
}

size_t gossip_table_snapshot(gossip_table_t *table, gossip_member_t **out_members){
    if(!table || !out_members) return 0;

    pthread_mutex_lock(&table -> lock);
    if(table -> count == 0){
        *out_members = NULL;
        pthread_mutex_unlock(&table -> lock);
        return 0;
    }

    gossip_member_t *snap = malloc(sizeof(gossip_member_t) * table -> count);
    if(!snap){
        *out_members = NULL;
        pthread_mutex_unlock(&table -> lock);
        return 0;
    }

    memcpy(snap, table -> members, sizeof(gossip_member_t) * table -> count);
    size_t count = table -> count;
    *out_members = snap;

    pthread_mutex_unlock(&table -> lock);
    return count;
}
#include "../include/ring.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static bool binary_search_vnode(const hash_ring_t *ring, uint64_t token, size_t *out_idx){
    if(!ring || ring -> count == 0){
        if(out_idx) *out_idx = 0;
        return false;
    }

    size_t low = 0;
    size_t high = ring -> count;

    while(low < high){
        size_t mid = low + (high - low) / 2;
        if(ring -> vnodes[mid].token == token){
            if(out_idx) *out_idx = mid;
            return true;
        }else if(ring -> vnodes[mid].token < token){
            low = mid + 1;
        }else{
            high = mid;
        }
    }

    if(out_idx) *out_idx = low;
    return false;
}

uint64_t hash_key(const void* key, size_t len){
    if(!key || len == 0) return 0;
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t *bytes = (const uint8_t *) key;

    for(size_t i = 0; i < len; i++){
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void ring_init(hash_ring_t *ring){
    if(!ring) return;
    ring -> vnodes = NULL;
    ring -> count = 0;
    ring -> capacity = 0;
}

void ring_free(hash_ring_t *ring){
    if(!ring) return;

    if(ring -> vnodes){
        free(ring -> vnodes);
        ring -> vnodes = NULL;
    }
    ring -> count = 0;
    ring -> capacity = 0;
}

int ring_add_node(hash_ring_t *ring, uint32_t node_id, size_t num_vnodes){
    if(!ring || num_vnodes == 0) return -1;

    for(size_t i = 0; i < num_vnodes; i++){
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "node_%d_vnode_%zu", node_id, i);
        uint64_t token = hash_key(buf, (size_t)len);


        size_t idx = 0;
        binary_search_vnode(ring, token, &idx);

        if(ring -> count >= ring -> capacity){
            size_t new_cap = (ring -> capacity == 0) ? DEFAULT_VNODES_PER_NODE : (ring -> capacity * 2);
            vnode_t *new_vnodes = (vnode_t *)realloc(ring -> vnodes, new_cap *sizeof(vnode_t));

            if(!new_vnodes) return -1;
            ring -> vnodes = new_vnodes;
            ring -> capacity = new_cap;
        }
        
        if(idx < ring -> count){
            memmove(&ring -> vnodes[idx + 1], &ring -> vnodes[idx], (ring -> count - idx) * sizeof(vnode_t));
        }
        ring -> vnodes[idx].token = token;
        ring -> vnodes[idx].node_id = node_id;
        ring -> count++;
    }
    return 0;
}

size_t ring_remove_node(hash_ring_t *ring, uint32_t node_id){
    if(!ring || ring -> count == 0) return 0;

    size_t write_idx = 0;
    

    for(size_t read_idx = 0; read_idx < ring -> count; read_idx++){
        if(ring -> vnodes[read_idx].node_id != node_id){
            ring ->vnodes[write_idx] = ring -> vnodes[read_idx];
            write_idx++;
        }
    }

    size_t removed = ring -> count - write_idx;
    ring -> count = write_idx;
    return removed;
}

int ring_find_coordinator(const hash_ring_t *ring, const void *key, size_t key_len, uint32_t *out_node_id){
    if(!ring || ring -> count == 0 || !key || key_len == 0 || !out_node_id) return -1;

    uint64_t token = hash_key(key, key_len);

    size_t idx = 0;
    binary_search_vnode(ring, token, &idx);

    if(idx == ring -> count){
        idx = 0;
    }

    *out_node_id = ring -> vnodes[idx].node_id;
    return 0;
}

size_t ring_get_preference_list(const hash_ring_t *ring, const void *key, size_t key_len, size_t N, uint32_t *out_node_ids, size_t max_out){
    if(!ring || ring -> count == 0 || !key || key_len == 0 || N == 0 || !out_node_ids || max_out == 0) return 0;

    size_t target_count = (N < max_out) ? N : max_out;

    uint64_t token = hash_key(key, key_len);
    size_t start_idx = 0;
    binary_search_vnode(ring, token, &start_idx);
    if(start_idx >= ring -> count) start_idx = 0;

    size_t curr_idx = start_idx;
    size_t collected = 0;
    size_t steps = 0;
    while(collected < target_count && steps < ring -> count){
        uint64_t candidate = ring -> vnodes[curr_idx].node_id;
        bool already_present = false;
        for(size_t i = 0; i < collected; i++){
            if(out_node_ids[i] == candidate){
                already_present = true;
                break;
            }
        }
        if(!already_present) out_node_ids[collected++] = candidate;

        curr_idx = (curr_idx + 1) % ring -> count;
        steps++;
    }

    return collected;
}

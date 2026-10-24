#include "../include/vector_clock.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * This file is where you will write the implementation!
 * We will go function by function:
 * 1. vclock_init & vclock_free
 * 2. binary_search_node (helper)
 * 3. vclock_increment
 * 4. vclock_get_counter
 * 5. vclock_compare (The two-pointer causality scan)
 * 6. vclock_copy & vclock_prune
 */

 static bool binary_search_node(const vclock_t *vc, uint32_t node_id, size_t *out_idx){
    if(!vc || vc -> count == 0){
        if(out_idx) *out_idx = 0;
        return false;
    }
    size_t low = 0;
    size_t high = vc -> count;

    while(low < high){
        size_t mid = low + (high - low) / 2;
        if(vc -> entries[mid].node_id == node_id){
            if(out_idx) *out_idx = mid;
            return true;
        }else if(vc -> entries[mid].node_id < node_id){
            low = mid + 1;
        }else if(vc -> entries[mid].node_id > node_id){
            high = mid;
        }
    }

    if(out_idx) *out_idx = low;
    return false;
 }
void vclock_init(vclock_t *vc){
    if(!vc) return;

    vc -> entries = NULL;
    vc -> count = 0;
    vc -> capacity = 0;
}

void vclock_free(vclock_t *vc){
    if(!vc) return;

    if(vc -> entries){
        free(vc -> entries);
        vc -> entries = NULL;
    } 
    vc -> count = 0;
    vc -> capacity = 0;
}

uint64_t vclock_get_counter(const vclock_t *vc, uint32_t node_id){
    if(!vc || vc -> count == 0) return 0;
    
    size_t idx = 0;

    bool res = binary_search_node(vc,  node_id, &idx);
    if(res){
        return vc ->entries[idx].counter;
    }else{
        return 0;
    }
}

int vclock_increment(vclock_t *vc, uint32_t node_id, uint64_t timestamp){
    if(!vc) return -1;

    size_t idx = 0;
    bool found = binary_search_node(vc, node_id, &idx);
    if(found){
        vc -> entries[idx].counter++;
        vc -> entries[idx].timestamp = timestamp;
        return 0;
    } else {
        size_t new_cap = 0;
        if(vc -> count >= vc -> capacity){
            new_cap = vc -> capacity == 0 ? VCLOCK_INITIAL_CAPACITY : 2 * vc -> capacity;
            vclock_entry_t *new_entries = (vclock_entry_t *)realloc(vc -> entries, new_cap * sizeof(vclock_entry_t));
        if(!new_entries){
            return -1;
        }
        vc -> entries = new_entries;
        vc -> capacity = new_cap;

        }
        
        if(idx < vc -> count){
            memmove(&vc -> entries[idx + 1], &vc -> entries[idx], (vc -> count - idx) * sizeof(vclock_entry_t));
            
        }
    }
    
    vc -> entries[idx].node_id = node_id;
    vc -> entries[idx].counter = 1;
    vc -> entries[idx].timestamp = timestamp;
    vc -> count++;

    return 0;
}

vclock_rel_t vclock_compare(const vclock_t *a, const vclock_t *b){
    if(!a && !b) return VC_EQUAL;
    if(!a || a -> count == 0){
        if(!b || b -> count == 0) return VC_EQUAL;
        else return VC_ANCESTOR;
    }

    if(!b || b -> count == 0) return VC_DESCENDANT;

    bool a_has_greater = false;
    bool b_has_greater = false;
    size_t i = 0;
    size_t j = 0;

    while(i < a -> count && j < b -> count){
        uint32_t node_a = a -> entries[i].node_id;
        uint32_t node_b = b -> entries[j].node_id;

        if(node_a == node_b){
            if(a -> entries[i].counter > b -> entries[j].counter) a_has_greater = true;
            if(b -> entries[j].counter > a -> entries[i].counter) b_has_greater = true;
            i++; j++;
        }else if(node_a < node_b){
            a_has_greater = true;
            i++;
        }else {
            b_has_greater = true;
            j++;
        }
    }

    while(i < a ->count){
        a_has_greater = true;
        i++;
    }

    while(j < b -> count){
        b_has_greater = true;
        j++;
    }

    if(a_has_greater && b_has_greater) return VC_CONCURRENT;
    else if(a_has_greater) return VC_DESCENDANT;
    else if(b_has_greater) return VC_ANCESTOR;
    else return VC_EQUAL;
}

int vclock_copy(vclock_t *dst, const vclock_t *src){
    if(!dst || !src) return -1;
    if(dst == src) return 0;
    vclock_free(dst);

    if(src -> count == 0){
        vclock_init(dst);
        return 0;
    }

    dst -> entries = (vclock_entry_t *) malloc(src -> count * sizeof(vclock_entry_t));
    if(!dst -> entries) return -1;
    
    memcpy(dst -> entries, src -> entries, src -> count * sizeof(vclock_entry_t));
    dst -> count = src -> count;
    dst -> capacity = src -> count;
    return 0;
}

int vclock_prune(vclock_t *vc, size_t max_entries){
    if(!vc || vc -> count <= max_entries || max_entries == 0) return 0;

    while(vc -> count > max_entries){
        size_t oldest_idx = 0;
        uint32_t oldest_ts = vc -> entries[0].timestamp;
        for(size_t i = 1; i < vc -> count; i++){
            if(vc -> entries[i].timestamp < oldest_ts){
                oldest_ts = vc -> entries[i].timestamp;
                oldest_idx = i;
            }
        }

        if(oldest_idx < vc -> count - 1){
            memmove(&vc -> entries[oldest_idx], 
                    &vc -> entries[oldest_idx + 1],
                (vc -> count - 1 - oldest_idx) * sizeof(vclock_entry_t));
        }
        vc -> count--;
    }
    return 0;
}

int vclock_to_string(const vclock_t *vc, char *buf, size_t buflen){
    if(!buf || buflen == 0) return -1;
    if(!vc || vc -> count == 0) return snprintf(buf, buflen, "[]");

    size_t offset = 0;

    int written = snprintf(buf + offset, buflen - offset, "[");
    if(written < 0 || (size_t)written >= buflen - offset) return -1;
    offset += (size_t)written;

    for(size_t i = 0; i < vc -> count; i++){
        const char *sep = (i + 1 < vc -> count) ? ", " : "";
        written = snprintf(buf + offset, buflen - offset, 
                  "(Node %u: %llu)%s", 
                    vc -> entries[i].node_id,
                (unsigned long long)vc -> entries[i].counter,
            sep);

        if(written < 0 || (size_t)written >= buflen - offset) return -1;
        offset += (size_t)written;
    }

    written = snprintf(buf + offset, buflen - offset, "]");
    if(written < 0 || (size_t)written >= buflen - offset) return -1;
    offset += (size_t)written;

    return (int)offset;
   
}
#ifndef DYNAMO_RING_H
#define DYNAMO_RING_H

#include <stdint.h> 
#include <stddef.h>
#include <stdbool.h>

#define DEFAULT_VNODES_PER_NODE 32

// Single point on the consistent hash ring
typedef struct {
    uint64_t token;
    uint32_t node_id;
} vnode_t;

// The consistent hash ring
typedef struct {
    vnode_t *vnodes;
    size_t count;
    size_t capacity;
} hash_ring_t;

uint64_t hash_key(const void *key, size_t len);

void ring_init(hash_ring_t *ring);
void ring_free(hash_ring_t *ring);

int ring_add_node(hash_ring_t *ring, uint32_t node_id, size_t num_vnodes);
size_t ring_remove_node(hash_ring_t *ring, uint32_t node_id);

int ring_find_coordinator(const hash_ring_t *ring, const void *key, size_t key_len, uint32_t *out_node_id);

size_t ring_get_preference_list(const hash_ring_t *ring, 
                                const void *key, 
                                size_t key_len,
                                size_t N,
                                uint32_t *out_node_ids,
                                size_t max_out);

#endif
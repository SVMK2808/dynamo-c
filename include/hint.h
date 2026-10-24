#ifndef DYNAMO_HINT_H
#define DYNAMO_HINT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "vector_clock.h"
#include "protocol.h"
#include "network.h"

/**
 * A single buffered write intended for a primary replica that was temporarily unreachable
 */

 typedef struct {
    uint32_t target_node_id; // The intended primary node ID (e.g., Node 2)
    void *key;
    size_t key_len;
    void *val;
    size_t val_len;
    vclock_t vclock;
 } hint_entry_t;

 /**
  * Thread - safe Hint Storage Queue.
  */
typedef struct {
    hint_entry_t *hints;
    size_t count;
    size_t capacity;
    pthread_mutex_t lock;
} hint_store_t;

/**
 * Lifecycle functions.
 */

 int hint_store_init(hint_store_t *store);
 void hint_store_free(hint_store_t *store);

 /**
  * Appends a new hint to the store for a temporarily unreachable target node.
  * Deep-copies key, val, and vector clock.
  * Returns 0 on success, or -1 on memory failure.
  */

 int hint_store_add(hint_store_t *store, uint32_t target_node_id,
                    const void *key, size_t key_len,
                    const void *val, size_t val_len,
                    const vclock_t *vc);

 /**
  * Returns total number of pending hints in the store.
  */

size_t hint_store_count(hint_store_t *store);

 /**
  * Returns number of pending hints destined for a specific target node.
  */
size_t hint_store_count_for_target(hint_store_t *store, uint32_t target_node_id);

/**
 * Flushes and replays all hints destined for 'target_node_id'.
 * Connects to target_ip:target_port via TCP, delivers each buffered write as a PUT_REQ,
 * and deletes hints upon receiving STATUS_OK.
 * Returns the number of successfully delivered hints, or -1 on connection failure. 
 */

 int hint_store_flush_target(hint_store_t *store, uint32_t target_node_id, 
                            const char *target_ip, uint16_t target_port);


#endif //DYNAMO_HINT_H
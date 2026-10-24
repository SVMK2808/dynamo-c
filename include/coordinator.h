#ifndef DYNAMO_COORDINATOR_H
#define DYNAMO_COORDINATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ring.h"
#include "storage.h"
#include "vector_clock.h"
#include "protocol.h"
#include "network.h"

/**
 * Information describing a node's physical network address.
 */

 typedef struct {
    uint32_t node_id;
    char ip[64];
    uint16_t port;
 } node_info_t;

 /**
  * Coordinator state machine
  */

  typedef struct {
    uint32_t local_node_id;
    uint16_t N; //Replication factor (e.g. 3)
    uint16_t R; //Read Quorum size (e.g. 2)
    uint16_t W; //Write Quorum size (e.g. 2)

    hash_ring_t *ring;          // Pointer to consistent hash ring
    storage_engine_t *storage;  // Pointer to local storage engine

    node_info_t *cluster_nodes; //Routing Table
    size_t cluster_node_count;

    struct hint_store *hints;   // Optional hint store for buffering missed replica writes
  } coordinator_t;

  /**
   * Initializes the coordinator instance.
   */

   int coordinator_init(coordinator_t *coord, uint32_t local_node_id, uint16_t N, uint16_t R, uint16_t W, hash_ring_t *ring, storage_engine_t *storage, const node_info_t *nodes, size_t node_count);

   /**
    * Frees any memory allocated by the coordinator.
    */

    void coordinator_free(coordinator_t *coord);

    /**
     * Executes a Quorum Write (PUT):
     * 1. Computes top - N Preference List from thr hash ring.
     * 2. Increments the Vector Clock locally (local_node_id: counter + 1, timestamp).
     * 3. Writes locally if coordinator is in the top - N.
     * 4. Dispatches PUT_REQ over TCP to the other remote nodes in the top - N.
     * 5. Returns STATUS_OK if >= W nodes acknowledge, else STATUS_ERROR.
     * Populates out_clock with the newly generated Vector Clock. 
     */

     int coordinator_put(coordinator_t *coord, 
                         const void *key, size_t key_len,
                         const void *val, size_t val_len,
                         const vclock_t *context_clock,
                         vclock_t *out_clock);


    /**
     * Executes a Quorum Read (GET):
     * 1. Computes top-N Preference List from the hash ring.
     * 2. Reads local storage if coordinator is in the top - N.
     * 3. Dispatches GET_REQ over TCP to the other remote nodes in the top - N.
     * 4. Waits for >= R responses.
     * 5. Reconciles versions/siblings across all received responses.
     * 6. (Optional) Triggers background read - repair for any stale nodes. 
     * Populates out_resp with the reconciled versions.
      */

      int coordinator_get(coordinator_t *coord, 
                          const void *key, size_t key_len,
                          get_resp_t *out_resp);

      /**
       * Executes a Sloppy Quorum Write.
       * If any primary nodes in the top - N are unreachable, it walks down the hash ring
       * to fallback nodes, sending the write with FLAG_HINTED_HANDOFF.
       * Returns STATUS_OK as long as total ACKs (primary + fallback hints) >= W.
       */

       int coordinator_put_sloppy(coordinator_t *coord, 
                                  const void *key, size_t key_len,
                                  const void *val, size_t val_len,
                                  const vclock_t *context_clock,
                                  vclock_t *out_clock);

#endif
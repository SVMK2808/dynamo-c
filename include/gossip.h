#ifndef DYNAMO_GOSSIP_H
#define DYNAMO_GOSSIP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/**
 * Node status in Dynamo's failure detecter
 */

 typedef enum {
    NODE_STATUS_DEAD    = 0,
    NODE_STATUS_ALIVE   = 1,
    NODE_STATUS_SUSPECT = 2
 } node_status_t;

 /**
  * Membership state record for a single cluster node.
  */

 typedef struct {
    uint32_t        node_id;
    char            host[64];
    int             port;
    uint64_t        generation;       // Node incarnation / boot epoch
    uint64_t        heartbeat;        // Monotonically increasing heartbeat counter.
    node_status_t   status;           // Current status (ALIVE, SUSPECT, DEAD)
    uint64_t        last_seen_tick;   // Local round / tick when heartbeat was last updated.    
 } gossip_member_t;

 /**
  * CLuster membership table maintained by each node. 
  */

  typedef struct {
    uint32_t        local_node_id;
    gossip_member_t *members;
    size_t          count;
    size_t          capacity;
    uint64_t        current_tick;       // Local clock / round counter.
    uint64_t        suspect_threshold;  // Number of ticks before marking SUSPECT.
    uint64_t        dead_threshold;     // Number of ticks before marking DEAD.
    pthread_mutex_t lock;               
  } gossip_table_t;

  /**
   * Initialise gossip table for local node.
   */

  int gossip_table_init(gossip_table_t *table, 
                         uint32_t local_node_id,
                         const char *host,
                         int port,
                         uint64_t suspect_threshold,
                         uint64_t dead_threshold);

  /**
   * Free all memory associated with the gossip table.
   */

  void gossip_table_free(gossip_table_t *table);

  /**
   * Advance local heartbeat and perform local failure detection sweep across all members.
   * - Increments local node's heartbeat.
   * - Any ALIVE node whose heartbeat has not advanced for >= suspect_threshold ticks -> SUSPECT.
   * - Any SUSPECT node whose heartbeat has not advanced for >= dead_threshold ticks -> DEAD.
   */

   void gossip_table_tick(gossip_table_t *table);

   /**
    * Add a static seed member to bootstap the cluster.
    */

    int gossip_table_add_seed(gossip_table_t *table,
                              uint32_t node_id,
                              const char *host, 
                              int port);


    /**
     * Select a random ALIVE or SUSPECT peer from the table (excluding local_node_id).
     * Returns 0 on success, -1 if no eligible peer exists.
     */

     int gossip_table_select_peer(gossip_table_t *table, gossip_member_t *out_peer);

     /**
      * Merge an incoming membership digest received from a peer.
      * Reconciles generation and heartbeat numbers according to Dynamo's rules:
      * 1. Higher generation supersedes all past generation data (node restarted).
      * 2. Equal generation with higher heartbeat updates heartbeat & resets last_seen_tick.
      * 3. Restores SUSPECT nodes back to ALIVE if heartbeat advanced. 
      * Returns the number of updated or newly inserted member records. 
      */

      int gossip_table_merge(gossip_table_t *table, 
                             const gossip_member_t *remote_members,
                             size_t remote_count);

      /**
       * Query current status of a node.
       * Returns NODE_STATUS_DEAD if node is unknown or DEAD.
       */

      node_status_t gossip_table_get_status(gossip_table_t *table, uint32_t node_id);

      /**
       * Find a member by node_id.
       * Copies member details into out_member.
       * Returns 0 if found, -1 if not found.
       */

      int gossip_table_get_member(gossip_table_t *table, uint32_t node_id, gossip_member_t *out_member);

       /**
        * Export a snapshot of all members in the table (caller must free out_members).
        * Returns count of members exported.
        */

        size_t gossip_table_snapshot(gossip_table_t *table, gossip_member_t **out_members);
   
#endif //DYNAMO_GOSSIP_H
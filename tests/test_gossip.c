#include "../include/gossip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static void test_init_and_self_heartbeat(void) {
    printf("[TEST] test_init_and_self_heartbeat...\n");

    gossip_table_t table;
    int rc = gossip_table_init(&table, 1, "127.0.0.1", 9001, 3, 6);
    assert(rc == 0);
    assert(table.local_node_id == 1);
    assert(table.count == 1);
    assert(table.current_tick == 0);

    gossip_member_t self;
    rc = gossip_table_get_member(&table, 1, &self);
    assert(rc == 0);
    assert(self.node_id == 1);
    assert(self.port == 9001);
    assert(self.status == NODE_STATUS_ALIVE);
    uint64_t initial_hb = self.heartbeat;
    assert(initial_hb >= 1);

    // Run 5 ticks
    for (int i = 0; i < 5; i++) {
        gossip_table_tick(&table);
    }

    assert(table.current_tick == 5);
    rc = gossip_table_get_member(&table, 1, &self);
    assert(rc == 0);
    assert(self.heartbeat == initial_hb + 5);
    assert(self.status == NODE_STATUS_ALIVE);
    assert(gossip_table_get_status(&table, 1) == NODE_STATUS_ALIVE);

    gossip_table_free(&table);
    printf("  -> PASS\n");
}

static void test_seed_addition_and_peer_selection(void) {
    printf("[TEST] test_seed_addition_and_peer_selection...\n");

    gossip_table_t table;
    gossip_table_init(&table, 1, "127.0.0.1", 9001, 3, 6);

    // Cannot select peer when only self is in table
    gossip_member_t peer;
    int rc = gossip_table_select_peer(&table, &peer);
    assert(rc == -1);

    // Add 2 seeds
    rc = gossip_table_add_seed(&table, 2, "127.0.0.1", 9002);
    assert(rc == 0);
    rc = gossip_table_add_seed(&table, 3, "127.0.0.1", 9003);
    assert(rc == 0);
    assert(table.count == 3);

    // Adding duplicate seed is idempotent
    rc = gossip_table_add_seed(&table, 2, "127.0.0.1", 9002);
    assert(rc == 0);
    assert(table.count == 3);

    // Peer selection should now succeed and pick either 2 or 3 (never self: 1)
    bool picked_2 = false;
    bool picked_3 = false;
    for (int i = 0; i < 50; i++) {
        rc = gossip_table_select_peer(&table, &peer);
        assert(rc == 0);
        assert(peer.node_id == 2 || peer.node_id == 3);
        if (peer.node_id == 2) picked_2 = true;
        if (peer.node_id == 3) picked_3 = true;
    }
    assert(picked_2 && picked_3);

    gossip_table_free(&table);
    printf("  -> PASS\n");
}

static void test_failure_detection_transitions(void) {
    printf("[TEST] test_failure_detection_transitions (ALIVE -> SUSPECT -> DEAD)...\n");

    gossip_table_t table;
    // suspect_threshold = 2, dead_threshold = 4
    gossip_table_init(&table, 1, "127.0.0.1", 9001, 2, 4);

    gossip_table_add_seed(&table, 2, "127.0.0.1", 9002);
    assert(gossip_table_get_status(&table, 2) == NODE_STATUS_ALIVE);

    // Tick 1: within suspect threshold
    gossip_table_tick(&table);
    assert(gossip_table_get_status(&table, 2) == NODE_STATUS_ALIVE);

    // Tick 2: reaches suspect threshold (2 ticks without heartbeat) -> SUSPECT
    gossip_table_tick(&table);
    assert(gossip_table_get_status(&table, 2) == NODE_STATUS_SUSPECT);

    // Tick 3: still SUSPECT
    gossip_table_tick(&table);
    assert(gossip_table_get_status(&table, 2) == NODE_STATUS_SUSPECT);

    // Tick 4: reaches dead threshold (4 ticks without heartbeat) -> DEAD
    gossip_table_tick(&table);
    assert(gossip_table_get_status(&table, 2) == NODE_STATUS_DEAD);

    // Once DEAD, peer selection must exclude it
    gossip_member_t peer;
    int rc = gossip_table_select_peer(&table, &peer);
    assert(rc == -1); // Node 2 is DEAD, Node 1 is self -> no eligible peers!

    gossip_table_free(&table);
    printf("  -> PASS (Failure detector state machine strictly verified!)\n");
}

static void test_gossip_merge_and_revival(void) {
    printf("[TEST] test_gossip_merge_and_revival (reconciling digests)...\n");

    // Node 1's table
    gossip_table_t table_1;
    gossip_table_init(&table_1, 1, "127.0.0.1", 9001, 3, 6);
    gossip_table_add_seed(&table_1, 2, "127.0.0.1", 9002);

    // Let Node 2 lapse into SUSPECT on Node 1
    gossip_table_tick(&table_1);
    gossip_table_tick(&table_1);
    gossip_table_tick(&table_1);
    assert(gossip_table_get_status(&table_1, 2) == NODE_STATUS_SUSPECT);

    // Node 2 has been alive and ticking on its own
    gossip_member_t node_2_digest = {
        .node_id = 2,
        .host = "127.0.0.1",
        .port = 9002,
        .generation = 100,
        .heartbeat = 42,
        .status = NODE_STATUS_ALIVE,
        .last_seen_tick = 0
    };

    // Node 1 receives digest containing Node 2's heartbeat
    int updates = gossip_table_merge(&table_1, &node_2_digest, 1);
    assert(updates == 1);

    // Node 2 should be restored to ALIVE on Node 1!
    assert(gossip_table_get_status(&table_1, 2) == NODE_STATUS_ALIVE);

    gossip_member_t m2;
    gossip_table_get_member(&table_1, 2, &m2);
    assert(m2.generation == 100);
    assert(m2.heartbeat == 42);

    gossip_table_free(&table_1);
    printf("  -> PASS (Heartbeat merge revived SUSPECT node!)\n");
}

static void test_generation_dominance_on_restart(void) {
    printf("[TEST] test_generation_dominance_on_restart (incarnation change)...\n");

    gossip_table_t table;
    gossip_table_init(&table, 1, "127.0.0.1", 9001, 2, 4);

    // Known node 5 at generation 10, heartbeat 500, DEAD
    gossip_member_t old_digest = {
        .node_id = 5,
        .host = "127.0.0.1",
        .port = 9005,
        .generation = 10,
        .heartbeat = 500,
        .status = NODE_STATUS_DEAD,
        .last_seen_tick = 0
    };
    gossip_table_merge(&table, &old_digest, 1);
    assert(gossip_table_get_status(&table, 5) == NODE_STATUS_DEAD);

    // Node 5 reboots! New generation 11, heartbeat 1, ALIVE
    gossip_member_t reboot_digest = {
        .node_id = 5,
        .host = "127.0.0.1",
        .port = 9005,
        .generation = 11,
        .heartbeat = 1,
        .status = NODE_STATUS_ALIVE,
        .last_seen_tick = 0
    };
    int updates = gossip_table_merge(&table, &reboot_digest, 1);
    assert(updates == 1);

    // Higher generation MUST override old DEAD status and lower heartbeat!
    assert(gossip_table_get_status(&table, 5) == NODE_STATUS_ALIVE);
    gossip_member_t m5;
    gossip_table_get_member(&table, 5, &m5);
    assert(m5.generation == 11);
    assert(m5.heartbeat == 1);

    gossip_table_free(&table);
    printf("  -> PASS\n");
}

static void test_snapshot_isolation(void) {
    printf("[TEST] test_snapshot_isolation...\n");

    gossip_table_t table;
    gossip_table_init(&table, 1, "127.0.0.1", 9001, 3, 6);
    gossip_table_add_seed(&table, 2, "127.0.0.1", 9002);
    gossip_table_add_seed(&table, 3, "127.0.0.1", 9003);

    gossip_member_t *snapshot = NULL;
    size_t snap_count = gossip_table_snapshot(&table, &snapshot);
    assert(snap_count == 3);
    assert(snapshot != NULL);

    // Verify snapshot has all 3 nodes
    bool has_1 = false, has_2 = false, has_3 = false;
    for (size_t i = 0; i < snap_count; i++) {
        if (snapshot[i].node_id == 1) has_1 = true;
        if (snapshot[i].node_id == 2) has_2 = true;
        if (snapshot[i].node_id == 3) has_3 = true;
    }
    assert(has_1 && has_2 && has_3);

    // Free snapshot
    free(snapshot);
    gossip_table_free(&table);
    printf("  -> PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO GOSSIP TEST SUITE        \n");
    printf("=========================================\n");

    test_init_and_self_heartbeat();
    test_seed_addition_and_peer_selection();
    test_failure_detection_transitions();
    test_gossip_merge_and_revival();
    test_generation_dominance_on_restart();
    test_snapshot_isolation();

    printf("=========================================\n");
    printf(" ALL GOSSIP TESTS PASSED CLEANLY!        \n");
    printf("=========================================\n");
    return 0;
}

#include "ring.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_init_and_free(void) {
    printf("[TEST] test_init_and_free...\n");
    hash_ring_t ring;
    ring_init(&ring);
    assert(ring.vnodes == NULL);
    assert(ring.count == 0);
    assert(ring.capacity == 0);

    uint32_t coord = 0;
    assert(ring_find_coordinator(&ring, "test", 4, &coord) == -1);

    ring_free(&ring);
    assert(ring.vnodes == NULL);
    printf("  -> PASS\n");
}

static void test_add_and_sorted_tokens(void) {
    printf("[TEST] test_add_and_sorted_tokens...\n");
    hash_ring_t ring;
    ring_init(&ring);

    /* Add Node 1 with 32 vnodes, Node 2 with 32 vnodes, Node 3 with 32 vnodes */
    assert(ring_add_node(&ring, 1, 32) == 0);
    assert(ring.count == 32);

    assert(ring_add_node(&ring, 2, 32) == 0);
    assert(ring.count == 64);

    assert(ring_add_node(&ring, 3, 32) == 0);
    assert(ring.count == 96);

    /* INVARIANT: Every token in the ring MUST be sorted in ascending order */
    for (size_t i = 1; i < ring.count; i++) {
        assert(ring.vnodes[i].token >= ring.vnodes[i - 1].token);
    }

    ring_free(&ring);
    printf("  -> PASS (96 virtual nodes strictly sorted!)\n");
}

static void test_coordinator_and_preference_list(void) {
    printf("[TEST] test_coordinator_and_preference_list...\n");
    hash_ring_t ring;
    ring_init(&ring);

    /* Add 5 physical machines with 32 vnodes each */
    for (uint32_t id = 1; id <= 5; id++) {
        assert(ring_add_node(&ring, id, 32) == 0);
    }
    assert(ring.count == 160);

    const char *key = "user_shopping_cart_10928";
    size_t key_len = strlen(key);

    /* 1. Find the coordinator */
    uint32_t coordinator = 0;
    assert(ring_find_coordinator(&ring, key, key_len, &coordinator) == 0);
    assert(coordinator >= 1 && coordinator <= 5);
    printf("  Key '%s' maps to Coordinator Node: %u\n", key, coordinator);

    /* 2. Get the Preference List for N = 3 replicas */
    uint32_t replicas[3];
    size_t count = ring_get_preference_list(&ring, key, key_len, 3, replicas, 3);
    assert(count == 3);

    /* CRITICAL INVARIANT 1: The coordinator MUST be the first node in the preference list! */
    assert(replicas[0] == coordinator);

    /* CRITICAL INVARIANT 2: All N nodes in the preference list MUST be DISTINCT physical machines! */
    assert(replicas[0] != replicas[1]);
    assert(replicas[0] != replicas[2]);
    assert(replicas[1] != replicas[2]);

    printf("  Preference List (N=3): [%u, %u, %u]\n", replicas[0], replicas[1], replicas[2]);

    ring_free(&ring);
    printf("  -> PASS\n");
}

static void test_fewer_nodes_than_N(void) {
    printf("[TEST] test_fewer_nodes_than_N...\n");
    hash_ring_t ring;
    ring_init(&ring);

    /* Only 2 physical nodes in cluster */
    ring_add_node(&ring, 10, 16);
    ring_add_node(&ring, 20, 16);

    /* Request N = 5 replicas */
    uint32_t replicas[5];
    size_t count = ring_get_preference_list(&ring, "some_key", 8, 5, replicas, 5);

    /* Must safely return only the 2 distinct nodes available without infinite looping */
    assert(count == 2);
    assert((replicas[0] == 10 && replicas[1] == 20) || (replicas[0] == 20 && replicas[1] == 10));

    ring_free(&ring);
    printf("  -> PASS\n");
}

static void test_node_removal(void) {
    printf("[TEST] test_node_removal...\n");
    hash_ring_t ring;
    ring_init(&ring);

    ring_add_node(&ring, 1, 32);
    ring_add_node(&ring, 2, 32);
    ring_add_node(&ring, 3, 32);
    assert(ring.count == 96);

    /* Remove Node 2 */
    size_t removed = ring_remove_node(&ring, 2);
    assert(removed == 32);
    assert(ring.count == 64);

    /* Verify Node 2 no longer exists on the ring */
    for (size_t i = 0; i < ring.count; i++) {
        assert(ring.vnodes[i].node_id != 2);
    }

    /* Verify ring remains strictly sorted after removal */
    for (size_t i = 1; i < ring.count; i++) {
        assert(ring.vnodes[i].token >= ring.vnodes[i - 1].token);
    }

    ring_free(&ring);
    printf("  -> PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO HASH RING TEST SUITE     \n");
    printf("=========================================\n");

    test_init_and_free();
    test_add_and_sorted_tokens();
    test_coordinator_and_preference_list();
    test_fewer_nodes_than_N();
    test_node_removal();

    printf("=========================================\n");
    printf(" ALL HASH RING TESTS PASSED CLEANLY!     \n");
    printf("=========================================\n");
    return 0;
}

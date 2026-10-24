#include "../include/merkle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_empty_and_single_item(void) {
    printf("[TEST] test_empty_and_single_item...\n");

    merkle_tree_t tree;
    merkle_tree_init(&tree);
    assert(merkle_tree_root_hash(&tree) == 0);

    vclock_t vc;
    vclock_init(&vc);
    vclock_increment(&vc, 1, 100);

    merkle_item_t item = {
        .key = "single_key",
        .key_len = 10,
        .val = "single_val",
        .val_len = 10,
        .vclock = &vc
    };

    int rc = merkle_tree_build(&tree, &item, 1);
    assert(rc == 0);
    assert(tree.leaf_count == 1);
    assert(merkle_tree_root_hash(&tree) != 0);

    merkle_tree_free(&tree);
    vclock_free(&vc);
    printf("  -> PASS\n");
}

static void test_deterministic_ordering(void) {
    printf("[TEST] test_deterministic_ordering (order independence)...\n");

    vclock_t vc;
    vclock_init(&vc);
    vclock_increment(&vc, 1, 100);

    merkle_item_t items_a[4] = {
        { .key = "alpha", .key_len = 5, .val = "v1", .val_len = 2, .vclock = &vc },
        { .key = "beta",  .key_len = 4, .val = "v2", .val_len = 2, .vclock = &vc },
        { .key = "gamma", .key_len = 5, .val = "v3", .val_len = 2, .vclock = &vc },
        { .key = "delta", .key_len = 5, .val = "v4", .val_len = 2, .vclock = &vc }
    };

    // Shuffled insertion order
    merkle_item_t items_b[4] = {
        { .key = "gamma", .key_len = 5, .val = "v3", .val_len = 2, .vclock = &vc },
        { .key = "alpha", .key_len = 5, .val = "v1", .val_len = 2, .vclock = &vc },
        { .key = "delta", .key_len = 5, .val = "v4", .val_len = 2, .vclock = &vc },
        { .key = "beta",  .key_len = 4, .val = "v2", .val_len = 2, .vclock = &vc }
    };

    merkle_tree_t tree_a, tree_b;
    merkle_tree_init(&tree_a);
    merkle_tree_init(&tree_b);

    merkle_tree_build(&tree_a, items_a, 4);
    merkle_tree_build(&tree_b, items_b, 4);

    // Root hashes MUST match despite different initial ordering
    assert(merkle_tree_root_hash(&tree_a) == merkle_tree_root_hash(&tree_b));

    merkle_diff_t diff;
    int rc = merkle_tree_diff(&tree_a, &tree_b, &diff);
    assert(rc == 0);
    assert(diff.count == 0); // Zero differences!

    merkle_diff_free(&diff);
    merkle_tree_free(&tree_a);
    merkle_tree_free(&tree_b);
    vclock_free(&vc);
    printf("  -> PASS\n");
}

static void test_targeted_diff_detection(void) {
    printf("[TEST] test_targeted_diff_detection (pinpointing divergent keys)...\n");

    #define NUM_ITEMS 64
    merkle_item_t items_a[NUM_ITEMS];
    merkle_item_t items_b[NUM_ITEMS];
    char keys[NUM_ITEMS][32];
    char vals_a[NUM_ITEMS][32];
    char vals_b[NUM_ITEMS][32];
    vclock_t vcs[NUM_ITEMS];

    for (int i = 0; i < NUM_ITEMS; i++) {
        snprintf(keys[i], sizeof(keys[i]), "user_record_%03d", i);
        snprintf(vals_a[i], sizeof(vals_a[i]), "value_payload_%d", i);
        snprintf(vals_b[i], sizeof(vals_b[i]), "value_payload_%d", i);

        vclock_init(&vcs[i]);
        vclock_increment(&vcs[i], 1, (uint64_t)i);

        items_a[i].key = keys[i];
        items_a[i].key_len = strlen(keys[i]);
        items_a[i].val = vals_a[i];
        items_a[i].val_len = strlen(vals_a[i]);
        items_a[i].vclock = &vcs[i];

        items_b[i] = items_a[i];
        items_b[i].val = vals_b[i];
    }

    // Now introduce 2 divergences in Tree B:
    // 1. Modify value of user_record_017
    strcpy(vals_b[17], "MUTATED_VALUE_XYZ");
    items_b[17].val_len = strlen(vals_b[17]);

    // 2. Modify vector clock of user_record_042
    vclock_t modified_vc;
    vclock_init(&modified_vc);
    vclock_copy(&modified_vc, &vcs[42]);
    vclock_increment(&modified_vc, 2, 9999);
    items_b[42].vclock = &modified_vc;

    merkle_tree_t tree_a, tree_b;
    merkle_tree_init(&tree_a);
    merkle_tree_init(&tree_b);

    merkle_tree_build(&tree_a, items_a, NUM_ITEMS);
    merkle_tree_build(&tree_b, items_b, NUM_ITEMS);

    // Root hashes must differ!
    assert(merkle_tree_root_hash(&tree_a) != merkle_tree_root_hash(&tree_b));

    // Run Merkle Diff
    merkle_diff_t diff;
    int rc = merkle_tree_diff(&tree_a, &tree_b, &diff);
    assert(rc == 0);

    // Must find EXACTLY the 2 diverged keys!
    assert(diff.count == 2);

    bool found_17 = false;
    bool found_42 = false;
    for (size_t i = 0; i < diff.count; i++) {
        if (diff.key_lens[i] == strlen("user_record_017") &&
            memcmp(diff.keys[i], "user_record_017", diff.key_lens[i]) == 0) found_17 = true;
        if (diff.key_lens[i] == strlen("user_record_042") &&
            memcmp(diff.keys[i], "user_record_042", diff.key_lens[i]) == 0) found_42 = true;
    }
    assert(found_17 && found_42);

    merkle_diff_free(&diff);
    merkle_tree_free(&tree_a);
    merkle_tree_free(&tree_b);
    vclock_free(&modified_vc);

    for (int i = 0; i < NUM_ITEMS; i++) {
        vclock_free(&vcs[i]);
    }
    printf("  -> PASS (Precisely detected the 2 diverged keys!)\n");
}

static void test_disjoint_missing_keys(void) {
    printf("[TEST] test_disjoint_missing_keys (detecting missing keys)...\n");

    vclock_t vc;
    vclock_init(&vc);
    vclock_increment(&vc, 1, 100);

    merkle_item_t items_a[3] = {
        { .key = "k1", .key_len = 2, .val = "v1", .val_len = 2, .vclock = &vc },
        { .key = "k2", .key_len = 2, .val = "v2", .val_len = 2, .vclock = &vc },
        { .key = "k3", .key_len = 2, .val = "v3", .val_len = 2, .vclock = &vc }
    };

    merkle_item_t items_b[3] = {
        { .key = "k1", .key_len = 2, .val = "v1", .val_len = 2, .vclock = &vc },
        { .key = "k2", .key_len = 2, .val = "v2", .val_len = 2, .vclock = &vc },
        { .key = "k4", .key_len = 2, .val = "v4", .val_len = 2, .vclock = &vc }
    };

    merkle_tree_t tree_a, tree_b;
    merkle_tree_init(&tree_a);
    merkle_tree_init(&tree_b);

    merkle_tree_build(&tree_a, items_a, 3);
    merkle_tree_build(&tree_b, items_b, 3);

    merkle_diff_t diff;
    int rc = merkle_tree_diff(&tree_a, &tree_b, &diff);
    assert(rc == 0);

    // Should detect k3 and k4
    assert(diff.count == 2);
    bool found_k3 = false;
    bool found_k4 = false;
    for (size_t i = 0; i < diff.count; i++) {
        if (diff.key_lens[i] == 2 && memcmp(diff.keys[i], "k3", 2) == 0) found_k3 = true;
        if (diff.key_lens[i] == 2 && memcmp(diff.keys[i], "k4", 2) == 0) found_k4 = true;
    }
    assert(found_k3 && found_k4);

    merkle_diff_free(&diff);
    merkle_tree_free(&tree_a);
    merkle_tree_free(&tree_b);
    vclock_free(&vc);
    printf("  -> PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO MERKLE TREE TEST SUITE   \n");
    printf("=========================================\n");

    test_empty_and_single_item();
    test_deterministic_ordering();
    test_targeted_diff_detection();
    test_disjoint_missing_keys();

    printf("=========================================\n");
    printf(" ALL MERKLE TREE TESTS PASSED CLEANLY!   \n");
    printf("=========================================\n");
    return 0;
}

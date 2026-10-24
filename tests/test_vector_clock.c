#include "vector_clock.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_init_and_free(void) {
    printf("[TEST] test_init_and_free...\n");
    vclock_t vc;
    vclock_init(&vc);
    assert(vc.entries == NULL);
    assert(vc.count == 0);
    assert(vc.capacity == 0);
    assert(vclock_get_counter(&vc, 1) == 0);

    vclock_free(&vc);
    assert(vc.entries == NULL);
    printf("  -> PASS\n");
}

static void test_increment_and_sorted_order(void) {
    printf("[TEST] test_increment_and_sorted_order...\n");
    vclock_t vc;
    vclock_init(&vc);

    /* Insert out of order: Node 10, then Node 2, then Node 5 */
    assert(vclock_increment(&vc, 10, 1000) == 0);
    assert(vclock_increment(&vc, 2, 1001) == 0);
    assert(vclock_increment(&vc, 5, 1002) == 0);

    /* Must be sorted by node_id: [2, 5, 10] */
    assert(vc.count == 3);
    assert(vc.entries[0].node_id == 2 && vc.entries[0].counter == 1);
    assert(vc.entries[1].node_id == 5 && vc.entries[1].counter == 1);
    assert(vc.entries[2].node_id == 10 && vc.entries[2].counter == 1);

    /* Increment an existing node (Node 5) */
    assert(vclock_increment(&vc, 5, 1005) == 0);
    assert(vc.count == 3);
    assert(vc.entries[1].counter == 2);
    assert(vc.entries[1].timestamp == 1005);
    assert(vclock_get_counter(&vc, 5) == 2);
    assert(vclock_get_counter(&vc, 999) == 0);

    char buf[128];
    vclock_to_string(&vc, buf, sizeof(buf));
    printf("  Formatted: %s\n", buf);

    vclock_free(&vc);
    printf("  -> PASS\n");
}

static void test_causality_scenarios(void) {
    printf("[TEST] test_causality_scenarios...\n");
    vclock_t v1, v2;
    vclock_init(&v1);
    vclock_init(&v2);

    /* 1. Both empty -> EQUAL */
    assert(vclock_compare(&v1, &v2) == VC_EQUAL);

    /* 2. v1 has writes, v2 is empty -> DESCENDANT (v1 dominates v2) */
    vclock_increment(&v1, 1, 100);
    assert(vclock_compare(&v1, &v2) == VC_DESCENDANT);
    assert(vclock_compare(&v2, &v1) == VC_ANCESTOR);

    /* 3. Identical clocks -> EQUAL */
    vclock_increment(&v2, 1, 100);
    assert(vclock_compare(&v1, &v2) == VC_EQUAL);

    /* 4. v1 receives second write from Node 1 -> v1 dominates v2 */
    vclock_increment(&v1, 1, 200);
    assert(vclock_compare(&v1, &v2) == VC_DESCENDANT);
    assert(vclock_compare(&v2, &v1) == VC_ANCESTOR);

    /* 5. Causal inheritance: v2 receives update from Node 1, then Node 2 writes to v2 */
    vclock_increment(&v2, 1, 200); /* v2 catches up on Node 1 */
    vclock_increment(&v2, 2, 300); /* v2 gets write from Node 2 */
    /* v2: [(1, 2), (2, 1)], v1: [(1, 2)] -> v2 dominates v1 */
    assert(vclock_compare(&v1, &v2) == VC_ANCESTOR);
    assert(vclock_compare(&v2, &v1) == VC_DESCENDANT);

    /* 6. CONCURRENT (Conflict / Siblings):
     * v1 accepts write from Node 1: v1 = [(1, 3)]
     * v2 accepts write from Node 2: v2 = [(1, 2), (2, 2)]
     * v1 is higher on Node 1 (3 > 2), but v2 is higher on Node 2 (2 > 0).
     * SIBLINGS DETECTED!
     */
    vclock_increment(&v1, 1, 400);
    vclock_increment(&v2, 2, 401);
    assert(vclock_compare(&v1, &v2) == VC_CONCURRENT);
    assert(vclock_compare(&v2, &v1) == VC_CONCURRENT);

    vclock_free(&v1);
    vclock_free(&v2);
    printf("  -> PASS (All 4 causality states verified!)\n");
}

static void test_copy(void) {
    printf("[TEST] test_copy...\n");
    vclock_t src, dst;
    vclock_init(&src);
    vclock_init(&dst);

    vclock_increment(&src, 1, 10);
    vclock_increment(&src, 3, 20);
    vclock_increment(&src, 2, 30);

    assert(vclock_copy(&dst, &src) == 0);
    assert(vclock_compare(&src, &dst) == VC_EQUAL);

    /* Mutating dst must not affect src */
    vclock_increment(&dst, 4, 40);
    assert(src.count == 3);
    assert(dst.count == 4);
    assert(vclock_compare(&src, &dst) == VC_ANCESTOR);

    /* Self-copy safety */
    assert(vclock_copy(&dst, &dst) == 0);
    assert(dst.count == 4);

    vclock_free(&src);
    vclock_free(&dst);
    printf("  -> PASS\n");
}

static void test_prune(void) {
    printf("[TEST] test_prune...\n");
    vclock_t vc;
    vclock_init(&vc);

    /* Insert 4 entries with different timestamps */
    vclock_increment(&vc, 1, 500); // oldest
    vclock_increment(&vc, 2, 600);
    vclock_increment(&vc, 3, 700);
    vclock_increment(&vc, 4, 800);
    assert(vc.count == 4);

    /* Prune to max 3 entries -> Node 1 (ts=500) must be removed */
    vclock_prune(&vc, 3);
    assert(vc.count == 3);
    assert(vclock_get_counter(&vc, 1) == 0); /* Evicted! */
    assert(vclock_get_counter(&vc, 2) == 1);
    assert(vclock_get_counter(&vc, 3) == 1);
    assert(vclock_get_counter(&vc, 4) == 1);

    /* Verify array is still sorted: [2, 3, 4] */
    assert(vc.entries[0].node_id == 2);
    assert(vc.entries[1].node_id == 3);
    assert(vc.entries[2].node_id == 4);

    vclock_free(&vc);
    printf("  -> PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO VECTOR CLOCK TEST SUITE  \n");
    printf("=========================================\n");

    test_init_and_free();
    test_increment_and_sorted_order();
    test_causality_scenarios();
    test_copy();
    test_prune();

    printf("=========================================\n");
    printf(" ALL VECTOR CLOCK TESTS PASSED CLEANLY!  \n");
    printf("=========================================\n");
    return 0;
}

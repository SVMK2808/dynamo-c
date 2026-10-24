#include "storage.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_init_and_free(void) {
    printf("[TEST] test_init_and_free...\n");
    storage_engine_t engine;
    assert(storage_init(&engine, 16) == 0);
    assert(engine.total_keys == 0);
    assert(engine.num_buckets == 16);

    assert(storage_get(&engine, "non_existent", 12) == NULL);

    storage_free(&engine);
    assert(engine.buckets == NULL);
    assert(engine.total_keys == 0);
    printf("  -> PASS\n");
}

static void test_single_version_put_get(void) {
    printf("[TEST] test_single_version_put_get...\n");
    storage_engine_t engine;
    assert(storage_init(&engine, 16) == 0);

    vclock_t vc;
    vclock_init(&vc);
    vclock_increment(&vc, 1, 100); /* [(Node 1: 1)] */

    const char *key = "user:42";
    const char *val = "{\"name\": \"Alice\"}";

    assert(storage_put(&engine, key, strlen(key), val, strlen(val), &vc) == 0);
    assert(engine.total_keys == 1);

    const storage_entry_t *entry = storage_get(&engine, key, strlen(key));
    assert(entry != NULL);
    assert(entry->version_count == 1);
    assert(entry->versions[0].val_len == strlen(val));
    assert(memcmp(entry->versions[0].val, val, strlen(val)) == 0);
    assert(vclock_compare(&entry->versions[0].vclock, &vc) == VC_EQUAL);

    vclock_free(&vc);
    storage_free(&engine);
    printf("  -> PASS\n");
}

static void test_causal_dominance_overwrite(void) {
    printf("[TEST] test_causal_dominance_overwrite...\n");
    storage_engine_t engine;
    storage_init(&engine, 16);

    vclock_t v1, v2;
    vclock_init(&v1);
    vclock_init(&v2);

    /* V1: [(Node 1: 1)] */
    vclock_increment(&v1, 1, 100);
    /* V2: [(Node 1: 2)] -> Dominates V1 */
    vclock_increment(&v2, 1, 100);
    vclock_increment(&v2, 1, 200);

    const char *key = "cart:101";

    /* Write V1 */
    storage_put(&engine, key, strlen(key), "apple", 5, &v1);
    const storage_entry_t *e1 = storage_get(&engine, key, strlen(key));
    assert(e1->version_count == 1);
    assert(memcmp(e1->versions[0].val, "apple", 5) == 0);

    /* Write V2 (dominating clock) -> Should replace V1 */
    storage_put(&engine, key, strlen(key), "apple_banana", 12, &v2);
    const storage_entry_t *e2 = storage_get(&engine, key, strlen(key));
    assert(e2->version_count == 1);
    assert(memcmp(e2->versions[0].val, "apple_banana", 12) == 0);

    vclock_free(&v1);
    vclock_free(&v2);
    storage_free(&engine);
    printf("  -> PASS\n");
}

static void test_obsolete_write_ignored(void) {
    printf("[TEST] test_obsolete_write_ignored...\n");
    storage_engine_t engine;
    storage_init(&engine, 16);

    vclock_t v_old, v_new;
    vclock_init(&v_old);
    vclock_init(&v_new);

    vclock_increment(&v_old, 1, 100);

    vclock_increment(&v_new, 1, 100);
    vclock_increment(&v_new, 1, 200);

    const char *key = "counter";

    /* Write newer V_new first */
    storage_put(&engine, key, strlen(key), "10", 2, &v_new);

    /* Attempt to write older V_old -> Must be ignored! */
    storage_put(&engine, key, strlen(key), "5", 1, &v_old);

    const storage_entry_t *entry = storage_get(&engine, key, strlen(key));
    assert(entry->version_count == 1);
    assert(memcmp(entry->versions[0].val, "10", 2) == 0);

    vclock_free(&v_old);
    vclock_free(&v_new);
    storage_free(&engine);
    printf("  -> PASS\n");
}

static void test_concurrent_siblings_and_reconciliation(void) {
    printf("[TEST] test_concurrent_siblings_and_reconciliation...\n");
    storage_engine_t engine;
    storage_init(&engine, 16);

    /*
     * Simulate network partition:
     * Write A happens on Node 1: VC = [(1, 1)]
     * Write B happens on Node 2: VC = [(2, 1)]
     * These two clocks are CONCURRENT!
     */
    vclock_t vc_a, vc_b;
    vclock_init(&vc_a);
    vclock_init(&vc_b);

    vclock_increment(&vc_a, 1, 100);
    vclock_increment(&vc_b, 2, 101);

    const char *key = "cart:999";

    /* Write A */
    storage_put(&engine, key, strlen(key), "apple", 5, &vc_a);

    /* Write B -> Concurrent sibling! */
    storage_put(&engine, key, strlen(key), "banana", 6, &vc_b);

    /* CRITICAL DYNAMO INVARIANT: Key MUST now hold BOTH siblings! */
    const storage_entry_t *entry = storage_get(&engine, key, strlen(key));
    assert(entry != NULL);
    assert(entry->version_count == 2);
    printf("  Concurrent writes produced %zu siblings in local storage!\n", entry->version_count);

    /*
     * Client reads both siblings: "apple" and "banana".
     * Client application reconciles by merging: "apple,banana".
     * Client creates reconciled clock that dominates BOTH:
     * Reconciled VC = [(1, 2), (2, 1)]
     */
    vclock_t vc_reconciled;
    vclock_init(&vc_reconciled);
    vclock_increment(&vc_reconciled, 1, 100);
    vclock_increment(&vc_reconciled, 2, 101);
    vclock_increment(&vc_reconciled, 1, 200); /* Incremented by coordinator */

    /* Client writes back reconciled version */
    storage_put(&engine, key, strlen(key), "apple,banana", 12, &vc_reconciled);

    /* INVARIANT: Both previous siblings are now dominated and pruned! */
    const storage_entry_t *reconciled_entry = storage_get(&engine, key, strlen(key));
    assert(reconciled_entry->version_count == 1);
    assert(memcmp(reconciled_entry->versions[0].val, "apple,banana", 12) == 0);
    printf("  Reconciliation write successfully collapsed siblings back to 1 version!\n");

    vclock_free(&vc_a);
    vclock_free(&vc_b);
    vclock_free(&vc_reconciled);
    storage_free(&engine);
    printf("  -> PASS\n");
}

static void test_delete(void) {
    printf("[TEST] test_delete...\n");
    storage_engine_t engine;
    storage_init(&engine, 16);

    vclock_t vc;
    vclock_init(&vc);
    vclock_increment(&vc, 1, 1);

    storage_put(&engine, "k1", 2, "v1", 2, &vc);
    storage_put(&engine, "k2", 2, "v2", 2, &vc);
    assert(engine.total_keys == 2);

    assert(storage_delete(&engine, "k1", 2) == true);
    assert(engine.total_keys == 1);
    assert(storage_get(&engine, "k1", 2) == NULL);
    assert(storage_get(&engine, "k2", 2) != NULL);

    assert(storage_delete(&engine, "k1", 2) == false); /* Already deleted */

    vclock_free(&vc);
    storage_free(&engine);
    printf("  -> PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO STORAGE ENGINE TEST SUITE\n");
    printf("=========================================\n");

    test_init_and_free();
    test_single_version_put_get();
    test_causal_dominance_overwrite();
    test_obsolete_write_ignored();
    test_concurrent_siblings_and_reconciliation();
    test_delete();

    printf("=========================================\n");
    printf(" ALL STORAGE TESTS PASSED CLEANLY!       \n");
    printf("=========================================\n");
    return 0;
}

#include "../include/hint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_HINT_PORT 9302

typedef struct {
    size_t received_count;
    char last_key[64];
    char last_val[64];
} mock_node_state_t;

static void mock_receiver_handler(int client_fd, const msg_header_t *hdr, const void *payload, void *user_arg) {
    mock_node_state_t *state = (mock_node_state_t *)user_arg;

    if (hdr->msg_type == MSG_PUT_REQ) {
        // Verify that the message arrived with the HINTED_HANDOFF flag set
        assert((hdr->flags & FLAG_HINTED_HANDOFF) != 0);

        put_req_t req = {0};
        int rc = put_req_deserialise(&req, payload, hdr->payload_len);
        assert(rc >= 0);

        state->received_count++;
        snprintf(state->last_key, sizeof(state->last_key), "%.*s", (int)req.key_len, (const char *)req.key);
        snprintf(state->last_val, sizeof(state->last_val), "%.*s", (int)req.val_len, (const char *)req.val);

        vclock_free(&req.vclock);

        // Acknowledge with STATUS_OK
        net_send_msg(client_fd, MSG_PUT_RESP, hdr->correlation_id, STATUS_OK, FLAG_NONE, NULL, 0);
    }
}

static void test_hint_buffering_and_filtering(void) {
    printf("[TEST] test_hint_buffering_and_filtering...\n");

    hint_store_t store;
    int rc = hint_store_init(&store);
    assert(rc == 0);

    vclock_t vc;
    vclock_init(&vc);
    vclock_increment(&vc, 1, 100);

    // Add 3 hints for Node 2
    hint_store_add(&store, 2, "k1", 2, "v1", 2, &vc);
    hint_store_add(&store, 2, "k2", 2, "v2", 2, &vc);
    hint_store_add(&store, 2, "k3", 2, "v3", 2, &vc);

    // Add 2 hints for Node 3
    hint_store_add(&store, 3, "k4", 2, "v4", 2, &vc);
    hint_store_add(&store, 3, "k5", 2, "v5", 2, &vc);

    assert(hint_store_count(&store) == 5);
    assert(hint_store_count_for_target(&store, 2) == 3);
    assert(hint_store_count_for_target(&store, 3) == 2);
    assert(hint_store_count_for_target(&store, 4) == 0);

    vclock_free(&vc);
    hint_store_free(&store);
    printf("  -> PASS\n");
}

static void test_hint_tcp_flush_and_cleanup(void) {
    printf("[TEST] test_hint_tcp_flush_and_cleanup...\n");

    hint_store_t store;
    hint_store_init(&store);

    vclock_t vc;
    vclock_init(&vc);
    vclock_increment(&vc, 1, 100);

    // Buffer 3 hints for Node 2
    hint_store_add(&store, 2, "cart_item_1", 11, "apple_macbook", 13, &vc);
    hint_store_add(&store, 2, "cart_item_2", 11, "dell_xps", 8, &vc);
    hint_store_add(&store, 2, "cart_item_3", 11, "thinkpad_x1", 11, &vc);

    // Buffer 1 hint for Node 3
    hint_store_add(&store, 3, "other_node_item", 15, "monitor_4k", 10, &vc);

    // 1. Try flushing to Node 2 while it is still offline -> should fail (-1)
    int delivered = hint_store_flush_target(&store, 2, "127.0.0.1", TEST_HINT_PORT);
    assert(delivered == -1);
    assert(hint_store_count(&store) == 4); // All hints preserved!

    // 2. Now start Node 2's server
    mock_node_state_t state = {0};
    net_server_t server;
    int rc = net_server_start(&server, TEST_HINT_PORT, mock_receiver_handler, &state);
    assert(rc == 0);
    usleep(10000); // 10ms for server to bind

    // 3. Flush Node 2's hints
    delivered = hint_store_flush_target(&store, 2, "127.0.0.1", TEST_HINT_PORT);
    assert(delivered == 3);
    assert(state.received_count == 3);
    assert(strcmp(state.last_key, "cart_item_3") == 0);
    assert(strcmp(state.last_val, "thinkpad_x1") == 0);

    // Verify Node 2 hints are gone, but Node 3's hint remains
    assert(hint_store_count_for_target(&store, 2) == 0);
    assert(hint_store_count(&store) == 1);
    assert(hint_store_count_for_target(&store, 3) == 1);

    net_server_stop(&server);
    vclock_free(&vc);
    hint_store_free(&store);
    printf("  -> PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO HINT STORE TEST SUITE    \n");
    printf("=========================================\n");

    test_hint_buffering_and_filtering();
    test_hint_tcp_flush_and_cleanup();

    printf("=========================================\n");
    printf(" ALL HINT TESTS PASSED CLEANLY!          \n");
    printf("=========================================\n");
    return 0;
}

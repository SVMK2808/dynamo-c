#include "../include/coordinator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define CLUSTER_SIZE 3
#define BASE_PORT 9200

// Simulated Cluster Node Container
typedef struct {
    uint32_t node_id;
    uint16_t port;
    storage_engine_t storage;
    net_server_t server;
} test_cluster_node_t;

// Server handler running on each storage node to process peer requests
static void node_msg_handler(int client_fd, const msg_header_t *hdr, const void *payload, void *user_arg) {
    test_cluster_node_t *node = (test_cluster_node_t *)user_arg;

    if (hdr->msg_type == MSG_PUT_REQ) {
        put_req_t req;
        if (put_req_deserialise(&req, payload, hdr->payload_len) >= 0) {
            storage_put(&node->storage, req.key, req.key_len, req.val, req.val_len, &req.vclock);
            vclock_free(&req.vclock);
            net_send_msg(client_fd, MSG_PUT_RESP, hdr->correlation_id, STATUS_OK, FLAG_NONE, NULL, 0);
        } else {
            net_send_msg(client_fd, MSG_PUT_RESP, hdr->correlation_id, STATUS_ERROR, FLAG_NONE, NULL, 0);
        }
    } else if (hdr->msg_type == MSG_GET_REQ) {
        get_req_t req;
        if (get_req_deserialise(&req, payload, hdr->payload_len) >= 0) {
            const storage_entry_t *entry = storage_get(&node->storage, req.key, req.key_len);
            if (!entry || entry->version_count == 0) {
                net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_NOT_FOUND, FLAG_NONE, NULL, 0);
            } else {
                get_resp_t resp;
                resp.version_count = entry->version_count;
                resp.versions = calloc(entry->version_count, sizeof(proto_version_t));
                assert(resp.versions != NULL);

                for (size_t v = 0; v < entry->version_count; v++) {
                    resp.versions[v].val = entry->versions[v].val;
                    resp.versions[v].val_len = entry->versions[v].val_len;
                    vclock_copy(&resp.versions[v].vclock, &entry->versions[v].vclock);
                }

                size_t sz = get_resp_serialised_size(&resp);
                void *buf = malloc(sz);
                assert(buf != NULL);
                get_resp_serialise(&resp, buf, sz);

                net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_OK, FLAG_NONE, buf, sz);

                for (size_t v = 0; v < entry->version_count; v++) {
                    vclock_free(&resp.versions[v].vclock);
                }
                free(resp.versions);
                free(buf);
            }
        } else {
            net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_ERROR, FLAG_NONE, NULL, 0);
        }
    }
}

static void test_quorum_put_and_get(void) {
    printf("[TEST] test_quorum_put_and_get (N=3, W=2, R=2)...\n");

    hash_ring_t ring;
    ring_init(&ring);
    test_cluster_node_t nodes[CLUSTER_SIZE];
    node_info_t node_infos[CLUSTER_SIZE];

    for (uint32_t i = 0; i < CLUSTER_SIZE; i++) {
        uint32_t id = i + 1;
        nodes[i].node_id = id;
        nodes[i].port = BASE_PORT + id;
        storage_init(&nodes[i].storage, 64);
        ring_add_node(&ring, id, 16);

        node_infos[i].node_id = id;
        strcpy(node_infos[i].ip, "127.0.0.1");
        node_infos[i].port = nodes[i].port;

        int rc = net_server_start(&nodes[i].server, nodes[i].port, node_msg_handler, &nodes[i]);
        assert(rc == 0);
    }

    usleep(10000); // 10ms for listeners to bind

    // Node 1 acts as Coordinator
    coordinator_t coord;
    int rc = coordinator_init(&coord, 1, 3, 2, 2, &ring, &nodes[0].storage, node_infos, CLUSTER_SIZE);
    assert(rc == 0);

    const char *key = "item_42";
    const char *val = "apple_macbook_pro";
    vclock_t out_clock;
    vclock_init(&out_clock);

    // 1. Write via Coordinator
    int put_status = coordinator_put(&coord, key, strlen(key), val, strlen(val), NULL, &out_clock);
    assert(put_status == STATUS_OK);
    assert(out_clock.count > 0);

    // 2. Read via Coordinator
    get_resp_t get_resp;
    int get_status = coordinator_get(&coord, key, strlen(key), &get_resp);
    assert(get_status == STATUS_OK);
    assert(get_resp.version_count == 1);
    assert(memcmp(get_resp.versions[0].val, val, strlen(val)) == 0);

    get_resp_free(&get_resp);
    vclock_free(&out_clock);
    coordinator_free(&coord);

    for (uint32_t i = 0; i < CLUSTER_SIZE; i++) {
        net_server_stop(&nodes[i].server);
        storage_free(&nodes[i].storage);
    }
    ring_free(&ring);
    printf("  -> PASS\n");
}

static void test_causal_overwrite_across_quorum(void) {
    printf("[TEST] test_causal_overwrite_across_quorum (overwriting stale replicas)...\n");

    hash_ring_t ring;
    ring_init(&ring);
    test_cluster_node_t nodes[CLUSTER_SIZE];
    node_info_t node_infos[CLUSTER_SIZE];

    for (uint32_t i = 0; i < CLUSTER_SIZE; i++) {
        uint32_t id = i + 1;
        nodes[i].node_id = id;
        nodes[i].port = BASE_PORT + 10 + id;
        storage_init(&nodes[i].storage, 64);
        ring_add_node(&ring, id, 16);

        node_infos[i].node_id = id;
        strcpy(node_infos[i].ip, "127.0.0.1");
        node_infos[i].port = nodes[i].port;

        net_server_start(&nodes[i].server, nodes[i].port, node_msg_handler, &nodes[i]);
    }

    usleep(10000);

    coordinator_t coord;
    coordinator_init(&coord, 1, 3, 2, 2, &ring, &nodes[0].storage, node_infos, CLUSTER_SIZE);

    const char *key = "user_account_99";
    const char *val_v1 = "balance: $100";
    vclock_t clock_v1;
    vclock_init(&clock_v1);

    // Write v1
    int st = coordinator_put(&coord, key, strlen(key), val_v1, strlen(val_v1), NULL, &clock_v1);
    assert(st == STATUS_OK);

    // Write v2 causal update passing clock_v1
    const char *val_v2 = "balance: $250";
    vclock_t clock_v2;
    vclock_init(&clock_v2);
    st = coordinator_put(&coord, key, strlen(key), val_v2, strlen(val_v2), &clock_v1, &clock_v2);
    assert(st == STATUS_OK);

    // Read back: should return single v2 version because v2 dominates v1
    get_resp_t get_resp;
    st = coordinator_get(&coord, key, strlen(key), &get_resp);
    assert(st == STATUS_OK);
    assert(get_resp.version_count == 1);
    assert(memcmp(get_resp.versions[0].val, val_v2, strlen(val_v2)) == 0);

    get_resp_free(&get_resp);
    vclock_free(&clock_v1);
    vclock_free(&clock_v2);
    coordinator_free(&coord);

    for (uint32_t i = 0; i < CLUSTER_SIZE; i++) {
        net_server_stop(&nodes[i].server);
        storage_free(&nodes[i].storage);
    }
    ring_free(&ring);
    printf("  -> PASS\n");
}

static void test_quorum_failure_when_nodes_down(void) {
    printf("[TEST] test_quorum_failure_when_nodes_down (W=2 with only 1 alive)...\n");

    hash_ring_t ring;
    ring_init(&ring);
    test_cluster_node_t nodes[CLUSTER_SIZE];
    node_info_t node_infos[CLUSTER_SIZE];

    for (uint32_t i = 0; i < CLUSTER_SIZE; i++) {
        uint32_t id = i + 1;
        nodes[i].node_id = id;
        nodes[i].port = BASE_PORT + 20 + id;
        storage_init(&nodes[i].storage, 64);
        ring_add_node(&ring, id, 16);

        node_infos[i].node_id = id;
        strcpy(node_infos[i].ip, "127.0.0.1");
        node_infos[i].port = nodes[i].port;

        // ONLY start Node 1; leave Node 2 and Node 3 offline
        if (id == 1) {
            net_server_start(&nodes[i].server, nodes[i].port, node_msg_handler, &nodes[i]);
        }
    }

    usleep(10000);

    coordinator_t coord;
    // W=2 requires 2 acknowledgements, but only Node 1 is alive
    coordinator_init(&coord, 1, 3, 2, 2, &ring, &nodes[0].storage, node_infos, CLUSTER_SIZE);

    const char *key = "sensor_data";
    const char *val = "temperature: 42C";
    vclock_t out_clock;
    vclock_init(&out_clock);

    int st = coordinator_put(&coord, key, strlen(key), val, strlen(val), NULL, &out_clock);
    // MUST fail write quorum because only 1 ACK can be obtained!
    assert(st == STATUS_ERROR);

    vclock_free(&out_clock);
    coordinator_free(&coord);

    net_server_stop(&nodes[0].server);
    for (uint32_t i = 0; i < CLUSTER_SIZE; i++) {
        storage_free(&nodes[i].storage);
    }
    ring_free(&ring);
    printf("  -> PASS (Strict quorum enforced!)\n");
}

static void test_sloppy_quorum_write_fallback(void) {
    printf("[TEST] test_sloppy_quorum_write_fallback (Sloppy Quorum on node failure)...\n");

    #define SLOPPY_CLUSTER_SIZE 4
    hash_ring_t ring;
    ring_init(&ring);
    test_cluster_node_t nodes[SLOPPY_CLUSTER_SIZE];
    node_info_t node_infos[SLOPPY_CLUSTER_SIZE];

    for (uint32_t i = 0; i < SLOPPY_CLUSTER_SIZE; i++) {
        uint32_t id = i + 1;
        nodes[i].node_id = id;
        nodes[i].port = BASE_PORT + 30 + id;
        storage_init(&nodes[i].storage, 64);
        ring_add_node(&ring, id, 16);

        node_infos[i].node_id = id;
        strcpy(node_infos[i].ip, "127.0.0.1");
        node_infos[i].port = nodes[i].port;
    }

    // Node 1 (Coordinator) and Node 4 (Fallback) are started!
    // Node 2 and Node 3 (the other primary replicas) remain offline!
    net_server_start(&nodes[0].server, nodes[0].port, node_msg_handler, &nodes[0]);
    net_server_start(&nodes[3].server, nodes[3].port, node_msg_handler, &nodes[3]);

    usleep(10000);

    coordinator_t coord;
    // N=3, W=2, R=2 across 4 total cluster nodes
    coordinator_init(&coord, 1, 3, 2, 2, &ring, &nodes[0].storage, node_infos, SLOPPY_CLUSTER_SIZE);

    const char *key = "user_shopping_cart_10928";
    const char *val = "resilient_val";
    vclock_t out_clock;
    vclock_init(&out_clock);

    // 1. Strict quorum MUST fail because top-3 only has 1 healthy node (Node 1)
    int st_strict = coordinator_put(&coord, key, strlen(key), val, strlen(val), NULL, &out_clock);
    assert(st_strict == STATUS_ERROR);

    // 2. Sloppy quorum MUST succeed because it falls back to Node 4!
    int st_sloppy = coordinator_put_sloppy(&coord, key, strlen(key), val, strlen(val), NULL, &out_clock);
    assert(st_sloppy == STATUS_OK);
    assert(out_clock.count > 0);

    vclock_free(&out_clock);
    coordinator_free(&coord);

    net_server_stop(&nodes[0].server);
    net_server_stop(&nodes[3].server);

    for (uint32_t i = 0; i < SLOPPY_CLUSTER_SIZE; i++) {
        storage_free(&nodes[i].storage);
    }
    ring_free(&ring);
    printf("  -> PASS (Sloppy Quorum succeeded via fallback node!)\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO COORDINATOR TEST SUITE   \n");
    printf("=========================================\n");

    test_quorum_put_and_get();
    test_causal_overwrite_across_quorum();
    test_quorum_failure_when_nodes_down();
    test_sloppy_quorum_write_fallback();

    printf("=========================================\n");
    printf(" ALL COORDINATOR TESTS PASSED CLEANLY!   \n");
    printf("=========================================\n");
    return 0;
}

#include "../include/storage.h"
#include "../include/ring.h"
#include "../include/coordinator.h"
#include "../include/hint.h"
#include "../include/gossip.h"
#include "../include/network.h"
#include "../include/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#define MAX_CLUSTER_NODES 32

typedef struct {
    uint32_t            node_id;
    int                 port;
    storage_engine_t    storage;
    hash_ring_t         ring;
    hint_store_t        hints;
    gossip_table_t      gossip;
    coordinator_t       coord;
    net_server_t        server;
    volatile bool       running;
    pthread_t           gossip_thread;

    node_info_t         cluster_nodes[MAX_CLUSTER_NODES];
    size_t              cluster_node_count;
} dynamo_node_t;

static dynamo_node_t g_node;

static void handle_client_request(int client_fd, const msg_header_t *hdr, const void *payload, void *arg) {
    dynamo_node_t *node = (dynamo_node_t *)arg;

    // 1. PING -> PONG
    if (hdr->msg_type == MSG_PING) {
        net_send_msg(client_fd, MSG_PONG, hdr->correlation_id, STATUS_OK, FLAG_NONE, NULL, 0);
        return;
    }

    // 2. PUT_REQ
    if (hdr->msg_type == MSG_PUT_REQ) {
        put_req_t req;
        vclock_init(&req.vclock);

        int rc = put_req_deserialise(&req, payload, hdr->payload_len);
        if (rc < 0) {
            net_send_msg(client_fd, MSG_PUT_RESP, hdr->correlation_id, STATUS_ERROR, FLAG_NONE, NULL, 0);
            vclock_free(&req.vclock);
            return;
        }

        // Case A: Replica write or delivered hint
        if (hdr->correlation_id == 1001 || hdr->correlation_id == 3001 || hdr->correlation_id == 8888 || (hdr->flags & FLAG_HINTED_HANDOFF)) {
            if (hdr->correlation_id == 8888) {
                // Delivered hint intended for this node! Apply directly to storage engine
                storage_put(&node->storage, req.key, req.key_len, req.val, req.val_len, &req.vclock);
                printf("[NODE %u] Successfully applied delivered hint for key '%.*s'!\n",
                       node->node_id, (int)req.key_len, (char *)req.key);
            } else if (hdr->flags & FLAG_HINTED_HANDOFF) {
                // Buffer as a hint on behalf of an offline peer
                hint_store_add(&node->hints, 0, req.key, req.key_len, req.val, req.val_len, &req.vclock);
                printf("[NODE %u] Buffered hinted handoff on behalf of offline peer\n", node->node_id);
            } else {
                storage_put(&node->storage, req.key, req.key_len, req.val, req.val_len, &req.vclock);
            }
            net_send_msg(client_fd, MSG_PUT_RESP, hdr->correlation_id, STATUS_OK, FLAG_NONE, NULL, 0);
            vclock_free(&req.vclock);
            return;
        }

        // Case B: Client write -> local node acts as Coordinator
        vclock_t out_clock;
        vclock_init(&out_clock);

        int status = coordinator_put_sloppy(
            &node->coord,
            req.key, req.key_len,
            req.val, req.val_len,
            &req.vclock, &out_clock
        );

        net_send_msg(client_fd, MSG_PUT_RESP, hdr->correlation_id, (uint32_t)status, FLAG_NONE, NULL, 0);
        vclock_free(&out_clock);
        vclock_free(&req.vclock);
        return;
    }

    // 3. GET_REQ
    if (hdr->msg_type == MSG_GET_REQ) {
        get_req_t req;
        int rc = get_req_deserialise(&req, payload, hdr->payload_len);
        if (rc < 0) {
            net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_ERROR, FLAG_NONE, NULL, 0);
            return;
        }

        // Case A: Replica read from a remote coordinator (correlation_id 2001)
        if (hdr->correlation_id == 2001) {
            const storage_entry_t *entry = storage_get(&node->storage, req.key, req.key_len);
            if (!entry || entry->version_count == 0) {
                net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_NOT_FOUND, FLAG_NONE, NULL, 0);
            } else {
                get_resp_t resp;
                resp.version_count = entry->version_count;
                resp.versions = calloc(entry->version_count, sizeof(proto_version_t));
                if (resp.versions) {
                    for (size_t v = 0; v < entry->version_count; v++) {
                        resp.versions[v].val = entry->versions[v].val;
                        resp.versions[v].val_len = entry->versions[v].val_len;
                        vclock_init(&resp.versions[v].vclock);
                        vclock_copy(&resp.versions[v].vclock, &entry->versions[v].vclock);
                    }

                    size_t sz = get_resp_serialised_size(&resp);
                    void *buf = malloc(sz);
                    if (buf) {
                        get_resp_serialise(&resp, buf, sz);
                        net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_OK, FLAG_NONE, buf, sz);
                        free(buf);
                    }
                    get_resp_free(&resp);
                } else {
                    net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_ERROR, FLAG_NONE, NULL, 0);
                }
            }
            return;
        }

        // Case B: Client read -> local node acts as Coordinator (N=3, R=2)
        get_resp_t get_resp;
        memset(&get_resp, 0, sizeof(get_resp));

        int status = coordinator_get(&node->coord, req.key, req.key_len, &get_resp);

        if (status == STATUS_OK) {
            size_t resp_len = get_resp_serialised_size(&get_resp);
            uint8_t *resp_buf = malloc(resp_len);
            if (resp_buf) {
                get_resp_serialise(&get_resp, resp_buf, resp_len);
                net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_OK, FLAG_NONE, resp_buf, resp_len);
                free(resp_buf);
            } else {
                net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_ERROR, FLAG_NONE, NULL, 0);
            }
            get_resp_free(&get_resp);
        } else if (status == STATUS_NOT_FOUND) {
            net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_NOT_FOUND, FLAG_NONE, NULL, 0);
        } else {
            net_send_msg(client_fd, MSG_GET_RESP, hdr->correlation_id, STATUS_ERROR, FLAG_NONE, NULL, 0);
        }
        return;
    }
}

static void *gossip_worker_thread(void *arg) {
    dynamo_node_t *node = (dynamo_node_t *)arg;

    while (node->running) {
        sleep(1);
        if (!node->running) break;

        // 1. Advance local clock & failure detection
        gossip_table_tick(&node->gossip);

        // 2. Select a random peer to gossip with
        gossip_member_t peer;
        if (gossip_table_select_peer(&node->gossip, &peer) == 0) {
            int peer_fd = net_connect(peer.host, peer.port);
            if (peer_fd >= 0) {
                // Heartbeat ping
                net_send_msg(peer_fd, MSG_PING, 9999, STATUS_OK, FLAG_NONE, NULL, 0);

                // Drain & flush any buffered hints for this recovered peer!
                int flushed = hint_store_flush_target(&node->hints, peer.node_id, peer.host, peer.port);
                if (flushed > 0) {
                    printf("[NODE %u] Gossip detected recovered peer %u! Flushed %d hint(s) to %s:%d\n",
                           node->node_id, peer.node_id, flushed, peer.host, peer.port);
                }
                close(peer_fd);
            }
        }

        // 3. Actively drain any buffered hints if target peers have recovered
        for (size_t i = 0; i < node->cluster_node_count; i++) {
            uint32_t peer_id = node->cluster_nodes[i].node_id;
            if (peer_id != node->node_id && hint_store_count_for_target(&node->hints, peer_id) > 0) {
                int flushed = hint_store_flush_target(&node->hints, peer_id,
                                                      node->cluster_nodes[i].ip,
                                                      node->cluster_nodes[i].port);
                if (flushed > 0) {
                    printf("[NODE %u] Flushed %d hint(s) to recovered peer %u (%s:%d)!\n",
                           node->node_id, flushed, peer_id,
                           node->cluster_nodes[i].ip, node->cluster_nodes[i].port);
                }
            }
        }
    }
    return NULL;
}

static void sigint_handler(int sig) {
    (void)sig;
    printf("\n[NODE %u] Received shutdown signal...\n", g_node.node_id);
    g_node.running = false;
    net_server_stop(&g_node.server);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <node_id> <port> [seed_node_id:seed_port ...]\n", argv[0]);
        return 1;
    }

    uint32_t node_id = (uint32_t)atoi(argv[1]);
    int port = atoi(argv[2]);

    printf("=========================================\n");
    printf(" STARTING DYNAMO NODE %u ON PORT %d\n", node_id, port);
    printf("=========================================\n");

    memset(&g_node, 0, sizeof(g_node));
    g_node.node_id = node_id;
    g_node.port = port;
    g_node.running = true;

    signal(SIGINT, sigint_handler);

    // 1. Initialize Storage
    storage_init(&g_node.storage, 1024);

    // 2. Initialize Hash Ring (32 vnodes per node)
    ring_init(&g_node.ring);
    ring_add_node(&g_node.ring, node_id, 32);

    // 3. Register self in cluster routing table
    g_node.cluster_nodes[0].node_id = node_id;
    strncpy(g_node.cluster_nodes[0].ip, "127.0.0.1", sizeof(g_node.cluster_nodes[0].ip) - 1);
    g_node.cluster_nodes[0].port = (uint16_t)port;
    g_node.cluster_node_count = 1;

    // 4. Initialize Hint Store
    hint_store_init(&g_node.hints);

    // 5. Initialize Gossip Table
    gossip_table_init(&g_node.gossip, node_id, "127.0.0.1", port, 3, 6);

    // 6. Register seed peers
    for (int i = 3; i < argc; i++) {
        char seed_str[128];
        strncpy(seed_str, argv[i], sizeof(seed_str) - 1);
        char *colon = strchr(seed_str, ':');
        if (colon) {
            *colon = '\0';
            uint32_t seed_id = (uint32_t)atoi(seed_str);
            int seed_port = atoi(colon + 1);

            ring_add_node(&g_node.ring, seed_id, 32);
            gossip_table_add_seed(&g_node.gossip, seed_id, "127.0.0.1", seed_port);

            if (g_node.cluster_node_count < MAX_CLUSTER_NODES) {
                node_info_t *info = &g_node.cluster_nodes[g_node.cluster_node_count++];
                info->node_id = seed_id;
                strncpy(info->ip, "127.0.0.1", sizeof(info->ip) - 1);
                info->port = (uint16_t)seed_port;
            }
            printf("Registered cluster peer node %u at 127.0.0.1:%d\n", seed_id, seed_port);
        }
    }

    // 7. Initialize Quorum Coordinator (N=3, R=2, W=2)
    uint16_t N = (uint16_t)(g_node.cluster_node_count < 3 ? g_node.cluster_node_count : 3);
    uint16_t W = (uint16_t)(N > 1 ? 2 : 1);
    uint16_t R = (uint16_t)(N > 1 ? 2 : 1);

    coordinator_init(
        &g_node.coord,
        node_id,
        N, R, W,
        &g_node.ring,
        &g_node.storage,
        g_node.cluster_nodes,
        g_node.cluster_node_count
    );
    g_node.coord.hints = (struct hint_store *)&g_node.hints;
    printf("[NODE %u] Coordinator active: N=%u, W=%u, R=%u across %zu nodes\n",
           node_id, N, W, R, g_node.cluster_node_count);

    // 8. Start Background Gossip & Hint Flush Thread
    pthread_create(&g_node.gossip_thread, NULL, gossip_worker_thread, &g_node);

    // 9. Start Network Server
    printf("[NODE %u] Listening for TCP connections on 127.0.0.1:%d...\n", node_id, port);
    net_server_start(&g_node.server, port, handle_client_request, &g_node);

    // Wait for shutdown
    pthread_join(g_node.gossip_thread, NULL);

    // Graceful Cleanup
    coordinator_free(&g_node.coord);
    gossip_table_free(&g_node.gossip);
    hint_store_free(&g_node.hints);
    ring_free(&g_node.ring);
    storage_free(&g_node.storage);

    printf("[NODE %u] Clean shutdown complete.\n", node_id);
    return 0;
}

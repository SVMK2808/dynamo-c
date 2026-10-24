#include "../include/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#define TEST_PORT_BASE 9100

// Helper handler that echoes back received messages with an ACK or response
static void echo_handler(int client_fd, const msg_header_t *hdr, const void *payload, void *user_arg) {
    (void)user_arg;
    uint16_t resp_type = (hdr->msg_type == MSG_GOSSIP_PING) ? MSG_GOSSIP_ACK : (hdr->msg_type + 1);

    int rc = net_send_msg(client_fd, resp_type, hdr->correlation_id, STATUS_OK, hdr->flags, payload, hdr->payload_len);
    assert(rc == 0);
}

static void test_loopback_ping_pong(void) {
    printf("[TEST] test_loopback_ping_pong...\n");

    net_server_t server;
    int rc = net_server_start(&server, TEST_PORT_BASE + 1, echo_handler, NULL);
    assert(rc == 0);

    // Give server thread a fraction of a millisecond to bind
    usleep(10000);

    int client_fd = net_connect("127.0.0.1", TEST_PORT_BASE + 1);
    assert(client_fd >= 0);

    const char *payload_text = "hello_dynamo_cluster";
    size_t payload_len = strlen(payload_text) + 1;
    uint64_t correlation_id = 0x12345678AABBCCDDULL;

    rc = net_send_msg(client_fd, MSG_GOSSIP_PING, correlation_id, STATUS_OK, FLAG_NONE, payload_text, payload_len);
    assert(rc == 0);

    msg_header_t resp_hdr;
    void *resp_payload = NULL;
    rc = net_recv_msg(client_fd, &resp_hdr, &resp_payload);
    assert(rc == 1);
    assert(resp_hdr.magic == DYNAMO_MAGIC);
    assert(resp_hdr.msg_type == MSG_GOSSIP_ACK);
    assert(resp_hdr.correlation_id == correlation_id);
    assert(resp_hdr.status == STATUS_OK);
    assert(resp_hdr.payload_len == payload_len);
    assert(strcmp((const char *)resp_payload, payload_text) == 0);

    free(resp_payload);
    close(client_fd);

    net_server_stop(&server);
    printf("  -> PASS\n");
}

static void test_large_binary_payload(void) {
    printf("[TEST] test_large_binary_payload (4KB payload)...\n");

    net_server_t server;
    int rc = net_server_start(&server, TEST_PORT_BASE + 2, echo_handler, NULL);
    assert(rc == 0);

    usleep(10000);

    int client_fd = net_connect("127.0.0.1", TEST_PORT_BASE + 2);
    assert(client_fd >= 0);

    size_t payload_len = 4096;
    uint8_t *large_buf = malloc(payload_len);
    assert(large_buf != NULL);
    for (size_t i = 0; i < payload_len; i++) {
        large_buf[i] = (uint8_t)(i & 0xFF);
    }

    rc = net_send_msg(client_fd, MSG_PUT_REQ, 9999, STATUS_OK, FLAG_NONE, large_buf, payload_len);
    assert(rc == 0);

    msg_header_t resp_hdr;
    void *resp_payload = NULL;
    rc = net_recv_msg(client_fd, &resp_hdr, &resp_payload);
    assert(rc == 1);
    assert(resp_hdr.payload_len == payload_len);
    assert(memcmp(resp_payload, large_buf, payload_len) == 0);

    free(large_buf);
    free(resp_payload);
    close(client_fd);

    net_server_stop(&server);
    printf("  -> PASS\n");
}

static void test_multiple_sequential_requests(void) {
    printf("[TEST] test_multiple_sequential_requests...\n");

    net_server_t server;
    int rc = net_server_start(&server, TEST_PORT_BASE + 3, echo_handler, NULL);
    assert(rc == 0);

    usleep(10000);

    int client_fd = net_connect("127.0.0.1", TEST_PORT_BASE + 3);
    assert(client_fd >= 0);

    for (int i = 0; i < 20; i++) {
        char req_msg[32];
        snprintf(req_msg, sizeof(req_msg), "req_counter_%d", i);
        size_t len = strlen(req_msg) + 1;

        rc = net_send_msg(client_fd, MSG_GET_REQ, (uint64_t)i, STATUS_OK, FLAG_NONE, req_msg, len);
        assert(rc == 0);

        msg_header_t resp_hdr;
        void *resp_payload = NULL;
        rc = net_recv_msg(client_fd, &resp_hdr, &resp_payload);
        assert(rc == 1);
        assert(resp_hdr.correlation_id == (uint64_t)i);
        assert(strcmp((char *)resp_payload, req_msg) == 0);
        free(resp_payload);
    }

    close(client_fd);
    net_server_stop(&server);
    printf("  -> PASS\n");
}

typedef struct {
    uint16_t port;
    int client_id;
} concurrent_client_arg_t;

static void *concurrent_client_thread(void *arg) {
    concurrent_client_arg_t *carg = (concurrent_client_arg_t *)arg;
    int client_fd = net_connect("127.0.0.1", carg->port);
    assert(client_fd >= 0);

    for (int i = 0; i < 10; i++) {
        uint64_t corr = ((uint64_t)carg->client_id << 32) | (uint64_t)i;
        char buf[32];
        snprintf(buf, sizeof(buf), "client_%d_msg_%d", carg->client_id, i);
        size_t len = strlen(buf) + 1;

        int rc = net_send_msg(client_fd, MSG_GOSSIP_PING, corr, STATUS_OK, FLAG_NONE, buf, len);
        assert(rc == 0);

        msg_header_t resp_hdr;
        void *resp_payload = NULL;
        rc = net_recv_msg(client_fd, &resp_hdr, &resp_payload);
        assert(rc == 1);
        assert(resp_hdr.correlation_id == corr);
        assert(strcmp((char *)resp_payload, buf) == 0);
        free(resp_payload);
    }

    close(client_fd);
    free(carg);
    return NULL;
}

static void test_concurrent_clients(void) {
    printf("[TEST] test_concurrent_clients (5 worker threads)...\n");

    net_server_t server;
    int rc = net_server_start(&server, TEST_PORT_BASE + 4, echo_handler, NULL);
    assert(rc == 0);

    usleep(10000);

    #define NUM_CLIENTS 5
    pthread_t threads[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++) {
        concurrent_client_arg_t *arg = malloc(sizeof(concurrent_client_arg_t));
        assert(arg != NULL);
        arg->port = TEST_PORT_BASE + 4;
        arg->client_id = i;
        rc = pthread_create(&threads[i], NULL, concurrent_client_thread, arg);
        assert(rc == 0);
    }

    for (int i = 0; i < NUM_CLIENTS; i++) {
        pthread_join(threads[i], NULL);
    }

    net_server_stop(&server);
    printf("  -> PASS\n");
}

static void test_stop_and_immediate_restart(void) {
    printf("[TEST] test_stop_and_immediate_restart (SO_REUSEADDR verification)...\n");

    net_server_t server;
    int rc = net_server_start(&server, TEST_PORT_BASE + 5, echo_handler, NULL);
    assert(rc == 0);
    net_server_stop(&server);

    // Immediately start again on exact same port to verify no EADDRINUSE
    rc = net_server_start(&server, TEST_PORT_BASE + 5, echo_handler, NULL);
    assert(rc == 0);
    net_server_stop(&server);

    printf("  -> PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO NETWORK TRANSPORT TESTS  \n");
    printf("=========================================\n");

    test_loopback_ping_pong();
    test_large_binary_payload();
    test_multiple_sequential_requests();
    test_concurrent_clients();
    test_stop_and_immediate_restart();

    printf("=========================================\n");
    printf(" ALL NETWORK TESTS PASSED CLEANLY!       \n");
    printf("=========================================\n");
    return 0;
}

#include "../../include/builtin.h"
#include "../../../include/protocol.h"
#include "../../../include/network.h"
#include "../../../include/vector_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Default cluster target node
static char g_dynamo_host[64] = "127.0.0.1";
static int  g_dynamo_port = 9001;

/*
 * connect <host> <port> — Sets target Dynamo node and verifies reachability
 */
int builtin_connect(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: connect <host> <port>\n");
        printf("Current target: %s:%d\n", g_dynamo_host, g_dynamo_port);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);

    // Test reachability
    int fd = net_connect(host, port);
    if (fd < 0) {
        printf("[WARNING] Failed to connect to %s:%d (node may currently be offline)\n", host, port);
    } else {
        net_send_msg(fd, MSG_PING, 1, STATUS_OK, FLAG_NONE, NULL, 0);
        msg_header_t resp;
        void *payload = NULL;
        int rc = net_recv_msg(fd, &resp, &payload);
        if (rc >= 0 && resp.msg_type == MSG_PONG) {
            printf("[CONNECTED] Target Dynamo node set to %s:%d (health check PASSED)\n", host, port);
        } else {
            printf("[CONNECTED] Target set to %s:%d, but node sent unexpected ping response\n", host, port);
        }
        if (payload) free(payload);
        close(fd);
    }

    strncpy(g_dynamo_host, host, sizeof(g_dynamo_host) - 1);
    g_dynamo_port = port;
    return 0;
}

/*
 * ping [host] [port] — Health check for connected or specified Dynamo node
 */
int builtin_ping(int argc, char **argv) {
    const char *host = g_dynamo_host;
    int port = g_dynamo_port;

    if (argc >= 3) {
        host = argv[1];
        port = atoi(argv[2]);
    }

    int fd = net_connect(host, port);
    if (fd < 0) {
        printf("[ERROR] Failed to connect to Dynamo node at %s:%d\n", host, port);
        return 1;
    }

    net_send_msg(fd, MSG_PING, 1, STATUS_OK, FLAG_NONE, NULL, 0);

    msg_header_t resp;
    void *payload = NULL;
    int rc = net_recv_msg(fd, &resp, &payload);
    if (rc >= 0 && resp.msg_type == MSG_PONG) {
        printf("[PONG] Node at %s:%d is healthy and responding!\n", host, port);
    } else {
        printf("[ERROR] Node at %s:%d failed health check (msg_type=%u)\n", host, port, resp.msg_type);
    }

    if (payload) free(payload);
    close(fd);
    return 0;
}

/*
 * put <key> <value> — Issues a quorum write across the cluster
 */
int builtin_put(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: put <key> <value>\n");
        return 1;
    }

    const char *key = argv[1];
    const char *val = argv[2];

    int fd = net_connect(g_dynamo_host, g_dynamo_port);
    if (fd < 0) {
        printf("[ERROR] Cannot connect to coordinator at %s:%d (run 'connect <host> <port>')\n",
               g_dynamo_host, g_dynamo_port);
        return 1;
    }

    put_req_t put_req = {
        .key = key,
        .key_len = strlen(key),
        .val = val,
        .val_len = strlen(val)
    };
    vclock_init(&put_req.vclock);

    size_t payload_len = put_req_serialised_size(&put_req);
    uint8_t *payload = malloc(payload_len);
    if (!payload) {
        vclock_free(&put_req.vclock);
        close(fd);
        return 1;
    }
    put_req_serialise(&put_req, payload, payload_len);

    net_send_msg(fd, MSG_PUT_REQ, 1, STATUS_OK, FLAG_NONE, payload, payload_len);
    free(payload);
    vclock_free(&put_req.vclock);

    msg_header_t resp;
    void *resp_payload = NULL;
    net_recv_msg(fd, &resp, &resp_payload);

    if (resp.status == STATUS_OK) {
        printf("[SUCCESS] Quorum write succeeded: '%s' => '%s' (via %s:%d)\n",
               key, val, g_dynamo_host, g_dynamo_port);
    } else {
        printf("[FAILED] Quorum write failed for key '%s' (status=%u)\n", key, resp.status);
    }

    if (resp_payload) free(resp_payload);
    close(fd);
    fflush(stdout);
    return (resp.status == STATUS_OK) ? 0 : 1;
}

/*
 * get <key> — Issues a quorum read across the cluster and reconciles siblings
 */
int builtin_get(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: get <key>\n");
        return 1;
    }

    const char *key = argv[1];

    int fd = net_connect(g_dynamo_host, g_dynamo_port);
    if (fd < 0) {
        printf("[ERROR] Cannot connect to coordinator at %s:%d (run 'connect <host> <port>')\n",
               g_dynamo_host, g_dynamo_port);
        return 1;
    }

    get_req_t get_req = {
        .key = key,
        .key_len = strlen(key)
    };

    size_t payload_len = get_req_serialised_size(&get_req);
    uint8_t *payload = malloc(payload_len);
    if (!payload) {
        close(fd);
        return 1;
    }
    get_req_serialise(&get_req, payload, payload_len);

    net_send_msg(fd, MSG_GET_REQ, 2, STATUS_OK, FLAG_NONE, payload, payload_len);
    free(payload);

    msg_header_t resp;
    void *resp_payload = NULL;
    net_recv_msg(fd, &resp, &resp_payload);

    if (resp.status == STATUS_NOT_FOUND) {
        printf("[NOT FOUND] Key '%s' does not exist in cluster.\n", key);
        close(fd);
        return 1;
    }

    if (resp.status == STATUS_OK && resp_payload) {
        get_resp_t get_resp;
        memset(&get_resp, 0, sizeof(get_resp));
        get_resp_deserialise(&get_resp, resp_payload, resp.payload_len);

        printf("[FOUND] Key '%s' has %zu version(s):\n", key, get_resp.version_count);
        for (size_t i = 0; i < get_resp.version_count; i++) {
            char val_str[512];
            size_t cpy_len = get_resp.versions[i].val_len < sizeof(val_str) - 1 ? get_resp.versions[i].val_len : sizeof(val_str) - 1;
            memcpy(val_str, get_resp.versions[i].val, cpy_len);
            val_str[cpy_len] = '\0';

            char vc_str[256];
            vclock_to_string(&get_resp.versions[i].vclock, vc_str, sizeof(vc_str));
            printf("  Version %zu: Value='%s' | VectorClock=%s\n", i + 1, val_str, vc_str);
        }
        get_resp_free(&get_resp);
    } else {
        printf("[ERROR] Read failed (status=%u)\n", resp.status);
    }

    if (resp_payload) free(resp_payload);
    close(fd);
    fflush(stdout);
    return 0;
}

#include "../include/protocol.h"
#include "../include/network.h"
#include "../include/vector_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "Dynamo Distributed Key-Value Store CLI\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s ping <host> <port>\n", prog);
    fprintf(stderr, "  %s put  <host> <port> <key> <value>\n", prog);
    fprintf(stderr, "  %s get  <host> <port> <key>\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd  = argv[1];
    const char *host = argv[2];
    int port = atoi(argv[3]);

    int fd = net_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to connect to Dynamo node at %s:%d\n", host, port);
        return 1;
    }

    // 1. PING Command
    if (strcmp(cmd, "ping") == 0) {
        net_send_msg(fd, MSG_PING, 1, STATUS_OK, FLAG_NONE, NULL, 0);

        msg_header_t resp;
        void *payload = NULL;
        int rc = net_recv_msg(fd, &resp, &payload);
        if (rc >= 0 && resp.msg_type == MSG_PONG) {
            printf("[PONG] Node at %s:%d is healthy and responding!\n", host, port);
        } else {
            printf("[ERROR] Node at %s:%d failed to respond properly (msg_type=%u)\n", host, port, resp.msg_type);
        }
        if (payload) free(payload);
        close(fd);
        return 0;
    }

    // 2. PUT Command
    if (strcmp(cmd, "put") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: %s put <host> <port> <key> <value>\n", argv[0]);
            close(fd);
            return 1;
        }

        const char *key = argv[4];
        const char *val = argv[5];

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
            printf("[SUCCESS] Quorum write succeeded for key '%s' => '%s'\n", key, val);
        } else {
            printf("[FAILED] Quorum write failed for key '%s' (status=%u)\n", key, resp.status);
        }

        if (resp_payload) free(resp_payload);
        close(fd);
        return 0;
    }

    // 3. GET Command
    if (strcmp(cmd, "get") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s get <host> <port> <key>\n", argv[0]);
            close(fd);
            return 1;
        }

        const char *key = argv[4];
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
        } else if (resp.status == STATUS_OK && resp_payload) {
            get_resp_t get_resp;
            memset(&get_resp, 0, sizeof(get_resp));
            get_resp_deserialise(&get_resp, resp_payload, resp.payload_len);

            printf("[FOUND] Key '%s' has %zu version(s):\n", key, get_resp.version_count);
            for (size_t i = 0; i < get_resp.version_count; i++) {
                char val_str[256];
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
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage(argv[0]);
    close(fd);
    return 1;
}

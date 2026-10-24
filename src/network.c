#include "../include/network.h"
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int net_send_all(int fd, const void *buf, size_t len){
    if(fd < 0 || !buf) return -1;

    const uint8_t *p = (const uint8_t *)buf;
    size_t total_sent = 0;

    while(total_sent < len){
        ssize_t n = send(fd, p + total_sent, len - total_sent, MSG_NOSIGNAL);
        if(n < 0){
            if(errno == EINTR){
                continue;
            }
            return -1;
        }
        total_sent += (size_t)n;
    }
    return (int)total_sent;
}

int net_recv_all(int fd, void *buf, size_t len){
    if(fd < 0 || !buf) return -1;
    
    uint8_t *p = (uint8_t *)buf;
    size_t total_received = 0;

    while(total_received < len){
        ssize_t n = recv(fd, p + total_received, len - total_received, 0);
        if(n < 0){
            if(errno == EINTR){
                continue;
            }
            return -1;
        }
        if(n == 0){
            return (total_received == 0) ? 0 : -1;
        }
        total_received += (size_t)n;
    }

    return (int)total_received;
}

int net_send_msg(int fd, uint16_t msg_type, uint64_t correlation_id, uint32_t status, uint16_t flags, const void *payload, size_t payload_len){
    if(fd < 0) return -1;
    if(payload_len > 0 && !payload) return -1;

    msg_header_t hdr = {
        .magic = DYNAMO_MAGIC,
        .msg_type = msg_type,
        .flags = flags,
        .correlation_id = correlation_id,
        .payload_len = (uint32_t) payload_len,
        .status = status
    };

    uint8_t hdr_buf[24];
    if(header_serialise(&hdr, hdr_buf, sizeof(hdr_buf)) != 24){
        return -1;
    }

    if(net_send_all(fd, hdr_buf, 24) != 24){
        return -1;
    }

    if(payload_len > 0){
        if(net_send_all(fd, payload, payload_len) != (int)payload_len){
            return -1;
        }
    }

    return 0;
}

int net_recv_msg(int fd, msg_header_t *out_hdr, void **out_payload){
    if(fd < 0 || !out_hdr || !out_payload) return -1;

    uint8_t hdr_buf[24];
    int n = net_recv_all(fd, hdr_buf, 24);
    if(n == 0){
        return 0; //Clean EOF: peer closed connection before sending the message
    }

    if(n != 24){
        return -1; //Connection dropped mid - header or socket error
    }

    if(header_deserialise(out_hdr, hdr_buf, 24) != 24){
        return -1; //Bad magic byte or corrupted header
    }

    if(out_hdr -> payload_len == 0){
        *out_payload = NULL;
        return 1;   // Message received with empty payload
    }

    void *payload = malloc(out_hdr -> payload_len);
    if(!payload){
        return -1; //Out of memory
    }

    if(net_recv_all(fd, payload, out_hdr -> payload_len) != (int)out_hdr -> payload_len){
        free(payload);
        *out_payload = NULL;
        return -1; // Peer disconnected mid-payload
    }

    *out_payload = payload;
    return 1; //Full message received successfully
}

int net_connect(const char *ip, uint16_t port){
    if(!ip || port == 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        return -1;
    }

    #ifdef SO_NOSIGPIPE
        int set = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
    #endif

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if(inet_pton(AF_INET, ip, &addr.sin_addr) <= 0){
            close(fd);
            return -1;
        }

        if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0){
            close(fd);
            return -1;
        }

        return fd;
}

typedef struct {
    int client_fd;
    net_server_t *server;
} client_worker_arg_t;

static void *client_worker_thread(void *arg){
    client_worker_arg_t *carg = (client_worker_arg_t *)arg;
    int client_fd = carg -> client_fd;
    net_server_t *server = carg -> server;

    while(server -> is_running){
        msg_header_t hdr;
        void *payload = NULL;
        int rc = net_recv_msg(client_fd, &hdr, &payload);
        if(rc <= 0){
            // rc == 0 : client closed connection cleanly
            // rc < 0 : network error or malformed frame
            break;
        }

        if(server -> handler){
            server -> handler(client_fd, &hdr, payload, server -> user_arg);
        }

        if(payload){
            free(payload);
        }
    }

    close(client_fd);
    free(carg);
    return NULL;
}

static void *accept_thread_fn(void *arg){
    net_server_t *server = (net_server_t *) arg;
    while(server -> is_running){
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server -> server_fd, (struct sockaddr *) &client_addr, &addr_len);

        if(client_fd < 0){
            if(!server -> is_running){
                break; // Server was stopped intentionally
            }

            if(errno == EINTR){
                continue;
            }

            break;
        }

    #ifdef SO_NOSIGPIPE
        int set = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
    #endif

        client_worker_arg_t *carg = malloc(sizeof(client_worker_arg_t));
        if(!carg){
            close(client_fd);
            continue;
        }

        carg -> client_fd = client_fd;
        carg -> server = server;

        pthread_t tid;
        if(pthread_create(&tid, NULL, client_worker_thread, carg) != 0){
            close(client_fd);
            free(carg);
            continue;
        }

        pthread_detach(tid);
    }

    return NULL;
}

int net_server_start(net_server_t *server, uint16_t port, net_msg_handler_fn handler, void *user_arg){
    if(!server || port == 0 || !handler) return -1;

    memset(server, 0, sizeof(net_server_t));
    server -> port = port;
    server -> handler = handler;
    server -> user_arg = user_arg;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &opt, sizeof(opt));

#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *) &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0){
        close(fd);
        return -1;
    }

    if(listen(fd, 128) < 0){
        close(fd);
        return -1;
    }

    server -> server_fd = fd;
    server -> is_running = true;

    if(pthread_create(&server -> accept_thread, NULL, accept_thread_fn, server) != 0){
        server -> is_running = false;
        close(fd);
        server -> server_fd = -1;
        return -1;
    }

    return 0;
}

void net_server_stop(net_server_t *server){
    if(!server || !server -> is_running) return;

    server -> is_running = false;

    if(server -> server_fd >= 0){
        shutdown(server -> server_fd, SHUT_RDWR);
        close(server -> server_fd);
        server -> server_fd = -1;
    }

    pthread_join(server -> accept_thread, NULL);
}
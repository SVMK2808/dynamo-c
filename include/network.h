#ifndef DYNAMO_NETWORK_H
#define DYNAMO_NETWORK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "protocol.h"

/**
 * Message handler callback invoked by worker threads when a full message is received.
 * client_fd: Socket descriptor of the connected client (used to send replies).
 * hdr: Deserialised 24-byte message header (host byte order).
 * payload: Heap-allocated payload buffer of length hdr->payload_len (or NULL if payload_len == 0).
 * user_arg: Context pointer passed into net_server_start().
 */
typedef void (*net_msg_handler_fn)(int client_fd, const msg_header_t *hdr, const void *payload, void *user_arg);

/**
 * Server instance state.
 */
typedef struct {
    int server_fd;
    uint16_t port;
    volatile bool is_running;
    pthread_t accept_thread;
    net_msg_handler_fn handler;
    void *user_arg;
} net_server_t;

/**
 * 1. Low-level TCP Stream Primitives
 */

/**
 * Sends exact 'len' bytes across socket fd.
 * Loops over partial writes and retries on EINTR.
 * Returns (int)len on success, or -1 on socket error.
 */
int net_send_all(int fd, const void *buf, size_t len);

/**
 * Reads exact 'len' bytes from socket fd into buf.
 * Loops over partial reads and retries on EINTR.
 * Returns (int)len on success, 0 on clean EOF / client disconnect, or -1 on socket error.
 */
int net_recv_all(int fd, void *buf, size_t len);

/**
 * 2. High-Level Framed Message Codec over Sockets
 */

/**
 * Serialises a 24-byte header and sends it followed by payload (if payload_len > 0).
 * Returns 0 on success, or -1 on transmission failure.
 */
int net_send_msg(int fd, uint16_t msg_type, uint64_t correlation_id, uint32_t status, uint16_t flags, const void *payload, size_t payload_len);

/**
 * Reads a 24-byte header, deserialises it, and reads exact payload_len bytes into *out_payload.
 * If out_hdr->payload_len > 0, *out_payload is allocated with malloc() and must be freed by caller.
 * If out_hdr->payload_len == 0, *out_payload is set to NULL.
 * Returns 1 on success, 0 on clean EOF, or -1 on socket error / malformed packet / magic mismatch.
 */
int net_recv_msg(int fd, msg_header_t *out_hdr, void **out_payload);

/**
 * 3. Client Connection
 */

/**
 * Establishes a blocking TCP connection to ip:port.
 * Returns connected socket file descriptor on success, or -1 on error.
 */
int net_connect(const char *ip, uint16_t port);

/**
 * 4. Server Lifecycle (Option 1: Thread-Per-Connection)
 */

/**
 * Binds and listens on port, then launches a background accept thread.
 * Each accepted client connection spawns a dedicated worker thread running net_recv_msg loop.
 * Returns 0 on success, or -1 on bind/listen/thread creation error.
 */
int net_server_start(net_server_t *server, uint16_t port, net_msg_handler_fn handler, void *user_arg);

/**
 * Stops server: sets is_running to false, closes server_fd to unblock accept(),
 * and joins the accept thread.
 */
void net_server_stop(net_server_t *server);

#endif // DYNAMO_NETWORK_H

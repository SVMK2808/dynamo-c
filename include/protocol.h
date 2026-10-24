#ifndef DYNAMO_PROTOCOL_H
#define DYNAMO_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vector_clock.h"

//Magic bytes - 'D', 'Y', 'N', 'A'
#define DYNAMO_MAGIC 0x44594E41

//Protocol Status Codes
typedef enum {
    STATUS_OK           = 0,
    STATUS_NOT_FOUND    = 1,
    STATUS_ERROR        = 2,
    STATUS_REDIRECT     = 3 //When node is not in preference list

} proto_status_t;

//Flags
typedef enum {
    FLAG_NONE           = 0x00,
    FLAG_HINTED_HANDOFF = 0x01, // This write is a temporary hint
    FLAG_READ_REPAIR    = 0x02  // Background read repair payload
} proto_flags_t;

//Message Types
typedef enum {
    MSG_GET_REQ         = 1,
    MSG_GET_RESP        = 2,
    MSG_PUT_REQ         = 3, 
    MSG_PUT_RESP        = 4, 
    MSG_GOSSIP_PING     = 5,
    MSG_GOSSIP_ACK      = 6,
    MSG_PING            = 7,
    MSG_PONG            = 8
} proto_msg_type_t;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             // DYNAMO_MAGIC (0x44594E41)
    uint16_t msg_type;          // proto_msg_type_t
    uint16_t flags;             // proto_flags_t
    uint64_t correlation_id;    // Unique request ID matching async replies
    uint32_t payload_len;       // Exact byte length of data following header
    uint32_t status;            // proto_status_t
} msg_header_t;
#pragma pack(pop)

//PUT response payload
typedef struct{
    const void *key;
    size_t key_len;
    const void *val;
    size_t val_len;
    vclock_t vclock;
} put_req_t;

// PUT response Payload
typedef struct {
    vclock_t vclock;
} put_resp_t;

// GET Request Payload
typedef struct {
    const void *key;
    size_t key_len;
} get_req_t;

// A single Sibling version in GET Response
typedef struct {
    void *val;
    size_t val_len;
    vclock_t vclock;
} proto_version_t;

// GET Response Payload (Holding 1 or More Siblings)
typedef struct {
    proto_version_t *versions;
    size_t version_count;
} get_resp_t;

//Get the size of the PUT - REQ
size_t put_req_serialised_size(const put_req_t *req);

//Get the serialised PUT - request
int put_req_serialise(const put_req_t *req, void *buf, size_t buflen);

//Get the deserialised PUT - request
int put_req_deserialise(put_req_t *req, const void *buf, size_t buflen);
/**
 * Calculate the exact number of bytes needed to serialise a vector clock
 */

 size_t vclock_serialised_size(const vclock_t *vc);

 /**
  * Serialise a vector clock into a contiguous binary buffer in network byte order
  * Returns bytes written on success, or -1 if buffer is insufficient.
  */

 int vclock_serialise(const vclock_t *vc, void *buf, size_t buflen);

 /**
  * Deserialise a binary buffer into a vector clock struct
  * Returns bytes consumed on success, or -1 if buffer is malformed / truncated.
  */
 int vclock_deserialise(vclock_t *vc, const void *buf, size_t buflen);

 /**
  * Serialises a msg_header_t into a 24-byte buffer in network byte order
  * Returns 24 on success, or -1 if buflen < 24 
  */

 int header_serialise(const msg_header_t *hdr, void *buf, size_t buflen);

 /**
  * Deserialises a 24 - byte buffer into a msg_header_t in host byte order.
  * Validates that magic matches DYNAMO_MAGIC
  * Returns 24 on success, or -1 if malformed/invalid magic
  */

int header_deserialise(msg_header_t *hdr, const void *buf, size_t buflen);

size_t get_req_serialised_size(const get_req_t *req);
int get_req_serialise(const get_req_t *req, void *buf, size_t buflen);
int get_req_deserialise(get_req_t *req, const void *buf, size_t buflen);

size_t get_resp_serialised_size(const get_resp_t *resp);
int get_resp_serialise(const get_resp_t *resp, void *buf, size_t buflen);
int get_resp_deserialise(get_resp_t *resp, const void *buf, size_t buflen);
void get_resp_free(get_resp_t *resp);


// Check at the compile - time that header is exactly 24 bytes
_Static_assert(sizeof(msg_header_t) == 24, "msg_header_t must be exactly 24 bytes!");

#endif
#include "../include/protocol.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

static inline uint64_t hton64(uint64_t val){
    #if defined (__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        return val;
    #else
        return __builtin_bswap64(val);
    #endif
}

#define ntoh64(val) hton64(val)

size_t vclock_serialised_size(const vclock_t *vc){
    if(!vc) return 2; // Just the 2B count field
    // 2B of count plus 20B of entry (4B node_id + 8B counter + 8B timestamp)
    return 2 + (vc -> count * 20);
}

int vclock_serialise(const vclock_t *vc, void *buf, size_t buflen){
    size_t needed = vclock_serialised_size(vc);
    if(!buf || buflen < needed) return -1;

    uint8_t *p = (uint8_t *) buf;
    uint16_t count = vc ? (uint16_t) vc -> count : 0;
    uint16_t net_count = htons(count);
    memcpy(p, &net_count, sizeof(uint16_t));
    p += sizeof(uint16_t);

    for(size_t i = 0; i < count; i++){
        uint32_t net_node_id = htonl(vc -> entries[i].node_id);
        memcpy(p, &net_node_id, sizeof(uint32_t));
        p += sizeof(uint32_t);

        uint64_t net_counter = hton64(vc -> entries[i].counter);
        memcpy(p, &net_counter, sizeof(uint64_t));
        p += sizeof(uint64_t);
        
        uint64_t net_ts = hton64(vc -> entries[i].timestamp);
        memcpy(p, &net_ts, sizeof(uint64_t));
        p += sizeof(uint64_t);
    }

    return (int)needed;
}

int vclock_deserialise(vclock_t *vc, const void *buf, size_t buflen){
    if(!vc || !buf || buflen < 2) return -1;

    const uint8_t *p = (const uint8_t *) buf;
    uint16_t net_count;
    memcpy(&net_count, p, sizeof(uint16_t));
    p += sizeof(uint16_t);
    uint16_t count = ntohs(net_count);
    
    size_t expected_len = 2 + ((size_t)count * 20);
    if(buflen < expected_len) return -1;

    vclock_free(vc); vclock_init(vc);
    if(count == 0) return 2;

    vc -> entries = (vclock_entry_t *)malloc(count * sizeof(vclock_entry_t));
    if(!vc -> entries) return -1;
    vc -> count = count;
    vc -> capacity = count;
    
    for(size_t i = 0; i < count ; i++){
        // Read 4B for node_id
        uint32_t net_node_id;
        memcpy(&net_node_id, p, sizeof(uint32_t));
        p += sizeof(uint32_t);
        vc -> entries[i].node_id = ntohl(net_node_id);

        //Read 8B for counter
        uint64_t net_counter;
        memcpy(&net_counter, p, sizeof(uint64_t));
        p += sizeof(uint64_t);
        vc -> entries[i].counter = ntoh64(net_counter);

        //Read 8B of timestamp
        uint64_t net_ts;
        memcpy(&net_ts, p, sizeof(uint64_t));
        p += sizeof(uint64_t);
        vc -> entries[i].timestamp = ntoh64(net_ts);
    }

    return (int)expected_len;
}

int header_serialise(const msg_header_t *hdr, void *buf, size_t buflen){
    if(!hdr || !buf || buflen < sizeof(msg_header_t)) return -1;

    msg_header_t net_hdr;
    net_hdr.magic           =  htonl(hdr -> magic);
    net_hdr.msg_type        =  htons(hdr -> msg_type);
    net_hdr.flags           =  htons(hdr -> flags);
    net_hdr.correlation_id  =  hton64(hdr -> correlation_id);
    net_hdr.payload_len     =  htonl(hdr -> payload_len);
    net_hdr.status          =  htonl(hdr -> status);

    memcpy(buf, &net_hdr, sizeof(msg_header_t));
    return sizeof(msg_header_t);
}

int header_deserialise(msg_header_t *hdr, const void *buf, size_t buflen){
    if(!hdr || !buf || buflen < sizeof(msg_header_t)) return -1;

    msg_header_t net_hdr;
    memcpy(&net_hdr, buf, sizeof(msg_header_t));

    hdr -> magic            = ntohl(net_hdr.magic);
    if(hdr -> magic != DYNAMO_MAGIC) return -1;
 
    hdr -> msg_type        = ntohs(net_hdr.msg_type);
    hdr -> flags           = ntohs(net_hdr.flags);
    hdr -> correlation_id  = ntoh64(net_hdr.correlation_id);
    hdr -> payload_len     = ntohl(net_hdr.payload_len);
    hdr -> status          = ntohl(net_hdr.status);

    return sizeof(msg_header_t);
}

size_t put_req_serialised_size(const put_req_t *req){
    if(!req) return 0;

    return sizeof(uint32_t) + req -> key_len + 
           sizeof(uint32_t) + req -> val_len + 
           vclock_serialised_size(&req -> vclock);
}

int put_req_serialise(const put_req_t *req, void *buf, size_t buflen){
    if(!req || !buf) return -1;
    size_t needed = put_req_serialised_size(req);
    if(buflen < needed) return -1;

    uint8_t *p = (uint8_t *) buf;

    //1. Write key_len and key
    uint32_t net_klen = htonl((uint32_t)req -> key_len);
    memcpy(p, &net_klen, sizeof(uint32_t));
    p += sizeof(uint32_t);

    if(req -> key_len > 0 && req -> key){
        memcpy(p, req -> key, req -> key_len);
        p += req -> key_len;
    }

    //2. Write val_len and val
    uint32_t net_vlen = htonl((uint32_t)req -> val_len);
    memcpy(p, &net_vlen, sizeof(uint32_t));
    p += sizeof(uint32_t);

    if(req -> val_len > 0 && req -> val){
        memcpy(p, req -> val, req -> val_len);
        p += req -> val_len;
    }

    //3. Write vector clock
    size_t remaining = buflen - (size_t)(p - (uint8_t *) buf);
    int vc_bytes = vclock_serialise(&req -> vclock, p, remaining);
    if(vc_bytes < 0) return -1;
    p += vc_bytes;

    return (int)(p - (uint8_t *) buf);
}

int put_req_deserialise(put_req_t *req, const void *buf, size_t buflen){
    if(!req || !buf || buflen < 8) return -1;

    const uint8_t *p = (const uint8_t *) buf;

    //1. Read key_len
    uint32_t net_klen;
    memcpy(&net_klen, p, sizeof(uint32_t));
    p += sizeof(uint32_t);
    uint32_t klen = ntohl(net_klen);

    //Guard against buffer overrun
    if((size_t)(p - (const uint8_t *)buf) + klen > buflen) return -1;
    req -> key = p;
    req -> key_len = klen;
    p += klen;

    //2. Read val_len
    if((size_t)(p - (const uint8_t *)buf) + sizeof(uint32_t) > buflen) return -1;
    uint32_t net_vlen;
    memcpy(&net_vlen, p, sizeof(uint32_t));
    p += sizeof(uint32_t);
    size_t vlen = ntohl(net_vlen);

    //Guard against buffer overrun
    if((size_t)(p - (const uint8_t *)buf) + vlen > buflen) return -1;
    req -> val = p;
    req -> val_len = vlen;
    p += vlen;

    //3. Read vector clock
    size_t remaining = buflen - (size_t)(p - (const uint8_t *)buf);
    int vc_bytes = vclock_deserialise(&req -> vclock, p, remaining);
    if(vc_bytes < 0) return -1;
    p += vc_bytes;

    return (int)(p - (const uint8_t *)buf);
}

size_t get_req_serialised_size(const get_req_t *req){
    if(!req) return 0;
    return sizeof(uint32_t) + req -> key_len;
}

int get_req_serialise(const get_req_t *req, void *buf, size_t buflen){
    if(!req || !buf) return -1;

    size_t needed = get_req_serialised_size(req);
    if(needed > buflen) return -1;

    uint8_t *p = (uint8_t *)buf;
    uint32_t net_klen = htonl((uint32_t)req -> key_len);
    memcpy(p, &net_klen, sizeof(uint32_t));
    p += sizeof(uint32_t);

    if(req -> key_len > 0 && req -> key){
        memcpy(p, req -> key, req -> key_len);
        p += req -> key_len;
    }

    return (int)(p - (uint8_t *)buf);
}

int get_req_deserialise(get_req_t *req, const void *buf, size_t buflen){
    if(!req || !buf || buflen < sizeof(uint32_t)) return -1;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t net_klen;
    memcpy(&net_klen, p, sizeof(uint32_t));
    p += sizeof(uint32_t);
    size_t klen = ntohl(net_klen);

    if((size_t)(p - (const uint8_t *)buf) + klen > buflen) return -1;

    req -> key = p;
    req -> key_len = klen;
    p += klen;

    return (int)(p - (const uint8_t *)buf);
}

size_t get_resp_serialised_size(const get_resp_t *resp){
    if(!resp) return sizeof(uint16_t);
    size_t total = sizeof(uint16_t);
    for(size_t i = 0; i < resp -> version_count; i++){
        total += sizeof(uint32_t) + resp -> versions[i].val_len;
        total += vclock_serialised_size(&resp -> versions[i].vclock);
    }
    return total;
}

int get_resp_serialise(const get_resp_t *resp, void *buf, size_t buflen){
    if(!resp || !buf) return -1;

    size_t needed = get_resp_serialised_size(resp);
    if(buflen < needed) return -1;

    uint8_t *p = (uint8_t *) buf;

    uint16_t net_version_count = htons(resp -> version_count);
    memcpy(p, &net_version_count, sizeof(uint16_t));
    p += sizeof(uint16_t);

    for(size_t i = 0; i < resp -> version_count; i++){
        uint32_t net_vlen = htonl((uint32_t)resp -> versions[i].val_len);
        memcpy(p, &net_vlen, sizeof(uint32_t));
        p += sizeof(uint32_t);


        if(resp -> versions[i].val_len > 0 && resp -> versions[i].val){
            memcpy(p, resp -> versions[i].val, resp -> versions[i].val_len);
            p += resp -> versions[i].val_len;
        }

        size_t remaining = buflen - (size_t)(p - (uint8_t *) buf);
        int vc_bytes = vclock_serialise(&resp -> versions[i].vclock, p, remaining);
        if(vc_bytes < 0) return -1;
        p += vc_bytes;
    }

    return (int)(p - (uint8_t *) buf);
}

int get_resp_deserialise(get_resp_t *resp, const void *buf, size_t buflen){
    if(!resp || !buf || buflen < sizeof(uint16_t)) return -1;

    const uint8_t *p = (uint8_t *) buf;

    uint16_t net_count;
    memcpy(&net_count, p, sizeof(uint16_t));
    p += sizeof(uint16_t);
    size_t count = ntohs(net_count);

    if(count == 0){
        resp -> versions = NULL;
        resp -> version_count = 0;
        return sizeof(uint16_t);
    }

    resp -> versions = (proto_version_t *) calloc(count, sizeof(proto_version_t));
    if(!resp -> versions) return -1;
    resp -> version_count = count;

    for(size_t i = 0; i < count; i++){
        if((size_t)(p - (const uint8_t *)buf) + sizeof(uint32_t) > buflen) goto fail;

        uint32_t net_vlen;
        memcpy(&net_vlen, p, sizeof(uint32_t));
        p += sizeof(uint32_t);
        size_t vlen = ntohl(net_vlen);

        if((size_t)(p - (const uint8_t *)buf) + vlen > buflen) goto fail;
        resp -> versions[i].val = (void *)p;
        resp -> versions[i].val_len = vlen;
        p += vlen;

        size_t remaining = buflen - (size_t)(p - (const uint8_t *) buf);
        vclock_init(&resp -> versions[i].vclock);
        int vc_bytes = vclock_deserialise(&resp -> versions[i].vclock, p, remaining);
        if(vc_bytes < 0) goto fail;
        p += vc_bytes;
    }

    return (int)(p - (const uint8_t *) buf);

fail:
    free(resp -> versions);
    resp -> versions = NULL;
    resp -> version_count = 0;
    return -1;
}

void get_resp_free(get_resp_t *resp){
    if(!resp || !resp -> versions) return;
    for(size_t i = 0; i < resp -> version_count; i++){
        vclock_free(&resp -> versions[i].vclock);
    }

    free(resp -> versions);
    resp -> versions = NULL;
    resp -> version_count = 0;
}
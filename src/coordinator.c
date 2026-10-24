#include "../include/coordinator.h"
#include "../include/hint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const node_info_t *find_node(const coordinator_t *coord, uint32_t node_id){
    if(!coord || !coord -> cluster_nodes) return NULL;
    for(size_t i = 0; i < coord -> cluster_node_count; i++){
        if(coord -> cluster_nodes[i].node_id == node_id){
            return &coord -> cluster_nodes[i];
        }
    }
    return NULL;
}

int coordinator_init(coordinator_t *coord, uint32_t local_node_id, uint16_t N, uint16_t R, uint16_t W, hash_ring_t *ring, storage_engine_t *storage, const node_info_t *nodes, size_t node_count){
    if(!coord || !ring || !storage || !nodes || node_count == 0) return -1;
    if(N == 0 || R == 0 || W == 0) return -1;
    if(W > N || R > N) return -1;

    memset(coord, 0, sizeof(coordinator_t));
    coord -> local_node_id = local_node_id;
    coord -> N = N;
    coord -> R = R;
    coord -> W = W;
    coord -> ring = ring;
    coord -> storage = storage;

    coord -> cluster_nodes = malloc(sizeof(node_info_t) * node_count);
    if(!coord -> cluster_nodes){
        return -1;
    }

    memcpy(coord -> cluster_nodes, nodes, sizeof(node_info_t) * node_count);
    coord -> cluster_node_count = node_count;

    return 0;
}

void coordinator_free(coordinator_t *coord){
    if(!coord) return;
    if(coord -> cluster_nodes){
        free(coord -> cluster_nodes);
        coord -> cluster_nodes = NULL;
    }

    coord -> cluster_node_count = 0;
}

int coordinator_put(coordinator_t *coord, 
                    const void *key, size_t key_len,
                    const void *val, size_t val_len,
                    const vclock_t *context_clock, 
                    vclock_t *out_clock){
                        if(!coord || !key || key_len == 0 || !val || val_len == 0) return STATUS_ERROR;

                        uint32_t *pref_list = malloc(sizeof(uint32_t) * coord -> N);
                        if(!pref_list) return STATUS_ERROR;

                        size_t n_nodes = ring_get_preference_list(coord -> ring, key, key_len, coord -> N, pref_list, coord -> N);

                        if(n_nodes < coord -> W){
                            free(pref_list);
                            return STATUS_ERROR;
                        }

                        //1. Advance causality via vector clock
                        vclock_t new_vc;
                        if(context_clock && context_clock -> count > 0){
                            vclock_copy(&new_vc, context_clock);
                        } else {
                            const storage_entry_t *existing = storage_get(coord -> storage, key, key_len);
                            if(existing && existing -> version_count > 0){
                                vclock_init(&new_vc);
                                vclock_copy(&new_vc, &existing -> versions[0].vclock);
                            } else {
                                vclock_init(&new_vc);
                            }
                        }

                        vclock_increment(&new_vc, coord -> local_node_id, (uint64_t)time(NULL));

                        uint64_t acks = 0;

                        //2. Dispatch write to all N replicas in preference list
                        for(size_t i = 0; i < n_nodes; i++){
                            uint32_t target_id = pref_list[i];

                            if(target_id == coord -> local_node_id){
                                //Write directly to local storage engine
                                if(storage_put(coord -> storage, key, key_len, val, val_len, &new_vc) == 0){
                                    acks++;
                                }
                            }else {
                                const node_info_t *node = find_node(coord, target_id);
                                if(!node) continue;

                                int fd = net_connect(node -> ip, node -> port);
                                if(fd < 0) continue; // Transient failure (node temporarily unreachable)

                                put_req_t req = {
                                    .key = key,
                                    .key_len = key_len,
                                    .val = val,
                                    .val_len = val_len,
                                    .vclock = new_vc
                                };

                                size_t payload_len = put_req_serialised_size(&req);
                                void *payload_buf = malloc(payload_len);
                                if(payload_buf){
                                    if(put_req_serialise(&req, payload_buf, payload_len) == (int)payload_len){
                                        if(net_send_msg(fd, MSG_PUT_REQ, 1001, STATUS_OK, FLAG_NONE, payload_buf, payload_len) == 0){
                                            msg_header_t resp_hdr;
                                            void *resp_payload = NULL;
                                            if(net_recv_msg(fd, &resp_hdr, &resp_payload) > 0){
                                                if(resp_hdr.msg_type == MSG_PUT_RESP && resp_hdr.status == STATUS_OK){
                                                    acks++;
                                                }
                                                if(resp_payload) free(resp_payload);
                                            }
                                        }
                                    }
                                    free(payload_buf);
                                }
                                close(fd);
                            }
                        }

                        free(pref_list);

                        if(out_clock){
                            vclock_copy(out_clock, &new_vc);
                        }

                        vclock_free(&new_vc);

                        return (acks >= coord -> W) ? STATUS_OK : STATUS_ERROR; 
                    }

static void add_version_reconciled(get_resp_t *resp, const void *val, size_t val_len, const vclock_t *vc){
    if(!resp || !val || !vc) return;

    for(size_t i = 0; i < resp -> version_count; i++){
        vclock_rel_t rel = vclock_compare(vc, &resp -> versions[i].vclock);

        if(rel == VC_EQUAL){
            // Identical version already present, deduplicate.
            return;
        }

        if(rel == VC_ANCESTOR){
            //Incoming version is older than existing version i, discard it.
            return;
        }

        if(rel == VC_DESCENDANT){
            //Incoming version dominates existing version i; prune the obsolete version
            free(resp -> versions[i].val);
            vclock_free(&resp -> versions[i].vclock);

            memmove(&resp -> versions[i], &resp -> versions[i + 1], sizeof(proto_version_t) * (resp -> version_count - 1 - i));
            resp -> version_count--;
            i--;
        }
    }

    proto_version_t *new_arr = realloc(resp -> versions, sizeof(proto_version_t) * (resp -> version_count + 1));
    if(!new_arr) return;
    resp -> versions = new_arr;

    proto_version_t *entry = &resp -> versions[resp -> version_count];
    entry -> val = malloc(val_len);
    if(!entry -> val) return;
    memcpy(entry -> val, val, val_len);
    entry -> val_len = val_len;

    vclock_init(&entry -> vclock);
    vclock_copy(&entry -> vclock, vc);

    resp -> version_count++;
}

int coordinator_get(coordinator_t *coord, const void *key, size_t key_len, get_resp_t *out_resp){
    if(!coord || !key || key_len == 0 || !out_resp) return STATUS_ERROR;

    memset(out_resp, 0, sizeof(get_resp_t));

    uint32_t *pref_list = malloc(sizeof(uint32_t) * coord -> N);
    if(!pref_list) return STATUS_ERROR;

    size_t n_nodes = ring_get_preference_list(coord -> ring, key, key_len, coord -> N, pref_list, coord -> N);
    if(n_nodes < coord -> R){
        free(pref_list);
        return STATUS_ERROR;
    }

    uint16_t responses = 0;

    for(size_t i = 0; i < n_nodes; i++){
        uint32_t target_id = pref_list[i];

        if(target_id == coord -> local_node_id){
            // Read from local storage engine
            const storage_entry_t *entry = storage_get(coord -> storage, key, key_len);
            responses++;
            if(entry && entry -> version_count > 0){
                for(size_t v = 0; v < entry -> version_count; v++){
                    add_version_reconciled(out_resp, 
                                           entry -> versions[v].val,
                                           entry -> versions[v].val_len,
                                           &entry -> versions[v].vclock);
                }
            }
        }else {
            // Read from remote replica over TCP
            const node_info_t *node = find_node(coord, target_id);
            if(!node) continue;

            int fd = net_connect(node -> ip, node -> port);
            if(fd < 0) continue;

            get_req_t req = { .key = key, .key_len = key_len };
            size_t payload_len = get_req_serialised_size(&req);
            void *payload_buf = malloc(payload_len);
            if(payload_buf){
                if(get_req_serialise(&req, payload_buf, payload_len) == (int) payload_len){
                    if(net_send_msg(fd, MSG_GET_REQ, 2001, STATUS_OK, FLAG_NONE, payload_buf, payload_len) == 0){
                        msg_header_t resp_hdr;
                        void *resp_payload = NULL;
                        if(net_recv_msg(fd, &resp_hdr, &resp_payload) > 0){
                            if(resp_hdr.msg_type == MSG_GET_RESP){
                                responses++;
                                if(resp_hdr.status == STATUS_OK && resp_payload){
                                    get_resp_t remote_resp;
                                    if(get_resp_deserialise(&remote_resp, resp_payload, resp_hdr.payload_len) >= 0){
                                        for(size_t v = 0; v < remote_resp.version_count; v++){
                                            add_version_reconciled(out_resp, remote_resp.versions[v].val, remote_resp.versions[v].val_len, &remote_resp.versions[v].vclock);
                                        }
                                        get_resp_free(&remote_resp);
                                    }
                                }
                            }
                            if(resp_payload) free(resp_payload);
                        }
                    }
                }
                free(payload_buf);
            }
            close(fd);
        }
    }
    free(pref_list);

        if(responses < coord -> R){
            get_resp_free(out_resp);
            return STATUS_ERROR;
        }

        return (out_resp -> version_count > 0) ? STATUS_OK : STATUS_NOT_FOUND;
}

int coordinator_put_sloppy(coordinator_t *coord, const void *key, size_t key_len, const void *val, size_t val_len, const vclock_t *context_clock, vclock_t *out_clock){
    if(!coord || !key || key_len == 0 || !val || val_len == 0) return STATUS_ERROR;

    size_t max_nodes = coord -> cluster_node_count;
    uint32_t *pref_list = malloc(sizeof(uint32_t) * max_nodes);
    if(!pref_list) return STATUS_ERROR;

    size_t n_nodes = ring_get_preference_list(coord -> ring, key, key_len, max_nodes, pref_list, max_nodes);
    if(n_nodes < coord -> W){
        free(pref_list);
        return STATUS_ERROR;
    }

    //1. Advance causality via vector clock.
    vclock_t new_vc;
    if(context_clock && context_clock -> count > 0){
        vclock_copy(&new_vc, context_clock);
    } else {
        const storage_entry_t *existing = storage_get(coord -> storage, key, key_len);
        if(existing && existing -> version_count > 0){
            vclock_init(&new_vc);
            vclock_copy(&new_vc, &existing -> versions[0].vclock);
        } else {
            vclock_init(&new_vc);
        }
    }

    vclock_increment(&new_vc, coord -> local_node_id, (uint64_t)time(NULL));

    uint16_t acks = 0;
    size_t primary_count = (coord -> N < n_nodes) ? coord -> N : n_nodes;

    //2. Phase 1: Try Primary top - N nodes
    for(size_t i = 0; i < primary_count; i++){
        uint32_t target_id = pref_list[i];

        if(target_id == coord -> local_node_id){
            if(storage_put(coord -> storage, key, key_len, val, val_len, &new_vc) == 0){
                acks++;
            }
        }else {
            const node_info_t *node = find_node(coord, target_id);
            if(!node) continue;

            int fd = net_connect(node -> ip, node -> port);
            if(fd < 0) {
                // Primary node is down/unreachable -> buffer hinted handoff
                if(coord -> hints){
                    hint_store_add((hint_store_t *)coord -> hints, target_id, key, key_len, val, val_len, &new_vc);
                    printf("[NODE %u] Primary replica %u is unreachable -> buffered hinted handoff\n",
                           coord -> local_node_id, target_id);
                }
                continue;
            }

            put_req_t req = {
                .key = key,
                .key_len = key_len,
                .val = val,
                .val_len = val_len,
                .vclock = new_vc 
            };

            size_t payload_len = put_req_serialised_size(&req);
            void *payload_buf = malloc(payload_len);
            if(payload_buf){
                if(put_req_serialise(&req, payload_buf, payload_len) == (int)payload_len){
                    if(net_send_msg(fd, MSG_PUT_REQ, 3001, STATUS_OK, FLAG_NONE, payload_buf, payload_len) == 0){
                        msg_header_t resp_hdr;
                        void *resp_payload = NULL;
                        if(net_recv_msg(fd, &resp_hdr, &resp_payload) > 0){
                            if(resp_hdr.msg_type == MSG_PUT_RESP && resp_hdr.status == STATUS_OK){
                                acks++;
                            }
                            if(resp_payload) free(resp_payload);
                        }
                    }
                }
                free(payload_buf);
            }
            close(fd);
        }
    }

    //3. Phase 2: Sloppy Quorum Fallback (if acks < W, try nodes beyond top - N)
    if(acks < coord -> W){
        for(size_t i = primary_count; i < n_nodes; i++){
            uint32_t fallback_id = pref_list[i];

            if(fallback_id == coord -> local_node_id){
                //Local node acting as a fallback
                if(storage_put(coord -> storage, key, key_len, val, val_len, &new_vc) == 0){
                    acks++;
                }
            }else{
                const node_info_t *node = find_node(coord, fallback_id);
                if(!node) continue;

                int fd = net_connect(node -> ip, node -> port);
                if(fd < 0) continue;

                put_req_t req = {
                    .key = key,
                    .key_len = key_len,
                    .val = val,
                    .val_len = val_len,
                    .vclock = new_vc
                };

                size_t payload_len = put_req_serialised_size(&req);
                void *payload_buf = malloc(payload_len);
                if(payload_buf){
                    if(put_req_serialise(&req, payload_buf, payload_len) == (int)payload_len){
                        //Send with flag FLAG_HINTED_HANDOFF
                        if(net_send_msg(fd, MSG_PUT_REQ, 3002, STATUS_OK, FLAG_HINTED_HANDOFF, payload_buf, payload_len) == 0){
                            msg_header_t resp_hdr;
                            void *resp_payload = NULL;
                            if(net_recv_msg(fd, &resp_hdr, &resp_payload) > 0){
                                if(resp_hdr.msg_type == MSG_PUT_RESP && resp_hdr.status == STATUS_OK){
                                    acks++;
                                }
                                if(resp_payload) free(resp_payload);
                            }
                        }
                    }
                    free(payload_buf);
                }
                close(fd);
            }

            if(acks >= coord -> W){
                break; //Quorum statisfied via fallback!
            }
        }
    }

    free(pref_list);

    if(out_clock){
        vclock_copy(out_clock, &new_vc);
    }

    vclock_free(&new_vc);

    return (acks >= coord -> W) ? STATUS_OK : STATUS_ERROR;
}

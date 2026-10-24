#include "protocol.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_header_codec(void) {
    printf("[TEST] test_header_codec...\n");
    msg_header_t original_hdr = {
        .magic = DYNAMO_MAGIC,
        .msg_type = MSG_PUT_REQ,
        .flags = FLAG_HINTED_HANDOFF,
        .correlation_id = 0x123456789ABCDEF0ULL,
        .payload_len = 1024,
        .status = STATUS_OK
    };

    uint8_t buf[sizeof(msg_header_t)];
    assert(header_serialise(&original_hdr, buf, sizeof(buf)) == sizeof(msg_header_t));

    msg_header_t decoded_hdr;
    assert(header_deserialise(&decoded_hdr, buf, sizeof(buf)) == sizeof(msg_header_t));

    assert(decoded_hdr.magic == original_hdr.magic);
    assert(decoded_hdr.msg_type == original_hdr.msg_type);
    assert(decoded_hdr.flags == original_hdr.flags);
    assert(decoded_hdr.correlation_id == original_hdr.correlation_id);
    assert(decoded_hdr.payload_len == original_hdr.payload_len);
    assert(decoded_hdr.status == original_hdr.status);

    /* Test rogue/corrupted magic byte rejection */
    buf[0] = 'X'; /* Corrupt 'D' to 'X' */
    assert(header_deserialise(&decoded_hdr, buf, sizeof(buf)) == -1);

    printf("  -> PASS\n");
}

static void test_vclock_wire_codec(void) {
    printf("[TEST] test_vclock_wire_codec...\n");
    vclock_t original_vc;
    vclock_init(&original_vc);

    /* Test 1: Empty clock */
    uint8_t buf[256];
    size_t sz = vclock_serialised_size(&original_vc);
    assert(sz == 2);
    assert(vclock_serialise(&original_vc, buf, sizeof(buf)) == 2);

    vclock_t decoded_vc;
    vclock_init(&decoded_vc);
    assert(vclock_deserialise(&decoded_vc, buf, 2) == 2);
    assert(decoded_vc.count == 0);
    assert(vclock_compare(&original_vc, &decoded_vc) == VC_EQUAL);

    /* Test 2: Clock with 3 entries */
    vclock_increment(&original_vc, 1, 1000);
    vclock_increment(&original_vc, 3, 3000);
    vclock_increment(&original_vc, 2, 2000);
    assert(original_vc.count == 3);

    sz = vclock_serialised_size(&original_vc);
    assert(sz == 2 + (3 * 20)); /* 62 bytes */
    assert(vclock_serialise(&original_vc, buf, sizeof(buf)) == (int)sz);

    assert(vclock_deserialise(&decoded_vc, buf, sz) == (int)sz);
    assert(decoded_vc.count == 3);
    assert(vclock_compare(&original_vc, &decoded_vc) == VC_EQUAL);

    vclock_free(&original_vc);
    vclock_free(&decoded_vc);
    printf("  -> PASS\n");
}

static void test_put_req_codec(void) {
    printf("[TEST] test_put_req_codec...\n");
    put_req_t req;
    req.key = "user_cart_101";
    req.key_len = strlen((const char *)req.key);
    req.val = "{\"items\": [\"apple\", \"banana\"]}";
    req.val_len = strlen((const char *)req.val);

    vclock_init(&req.vclock);
    vclock_increment(&req.vclock, 1, 500);

    uint8_t buf[512];
    size_t needed = put_req_serialised_size(&req);
    int written = put_req_serialise(&req, buf, sizeof(buf));
    assert(written == (int)needed);

    put_req_t decoded;
    vclock_init(&decoded.vclock);
    int read_bytes = put_req_deserialise(&decoded, buf, written);
    assert(read_bytes == written);

    assert(decoded.key_len == req.key_len);
    assert(memcmp(decoded.key, req.key, req.key_len) == 0);
    assert(decoded.val_len == req.val_len);
    assert(memcmp(decoded.val, req.val, req.val_len) == 0);
    assert(vclock_compare(&decoded.vclock, &req.vclock) == VC_EQUAL);

    vclock_free(&req.vclock);
    vclock_free(&decoded.vclock);
    printf("  -> PASS\n");
}

static void test_get_resp_siblings_codec(void) {
    printf("[TEST] test_get_resp_siblings_codec...\n");
    /* Simulate a GET response returning 2 concurrent sibling versions */
    proto_version_t siblings[2];

    /* Sibling 1 */
    siblings[0].val = "apple";
    siblings[0].val_len = 5;
    vclock_init(&siblings[0].vclock);
    vclock_increment(&siblings[0].vclock, 1, 100);

    /* Sibling 2 */
    siblings[1].val = "banana";
    siblings[1].val_len = 6;
    vclock_init(&siblings[1].vclock);
    vclock_increment(&siblings[1].vclock, 2, 200);

    get_resp_t resp = {
        .versions = siblings,
        .version_count = 2
    };

    uint8_t buf[512];
    size_t sz = get_resp_serialised_size(&resp);
    int written = get_resp_serialise(&resp, buf, sizeof(buf));
    assert(written == (int)sz);

    get_resp_t decoded;
    int read_bytes = get_resp_deserialise(&decoded, buf, written);
    assert(read_bytes == written);
    assert(decoded.version_count == 2);

    assert(decoded.versions[0].val_len == 5);
    assert(memcmp(decoded.versions[0].val, "apple", 5) == 0);
    assert(vclock_compare(&decoded.versions[0].vclock, &siblings[0].vclock) == VC_EQUAL);

    assert(decoded.versions[1].val_len == 6);
    assert(memcmp(decoded.versions[1].val, "banana", 6) == 0);
    assert(vclock_compare(&decoded.versions[1].vclock, &siblings[1].vclock) == VC_EQUAL);

    vclock_free(&siblings[0].vclock);
    vclock_free(&siblings[1].vclock);
    get_resp_free(&decoded);
    printf("  -> PASS (Wire transport of siblings verified!)\n");
}

int main(void) {
    printf("=========================================\n");
    printf(" RUNNING DYNAMO WIRE PROTOCOL TEST SUITE \n");
    printf("=========================================\n");

    test_header_codec();
    test_vclock_wire_codec();
    test_put_req_codec();
    test_get_resp_siblings_codec();

    printf("=========================================\n");
    printf(" ALL PROTOCOL TESTS PASSED CLEANLY!      \n");
    printf("=========================================\n");
    return 0;
}

// Host-side unit tests for proto_frame against golden bytes from the
// reference library. Build & run: tools/test_host.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pb_decode.h"
#include "polo.pb.h"
#include "proto_frame.h"

#include "golden.inc"

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

// --- capture transport: accumulates writes into a buffer ---
static uint8_t cap_buf[1024];
static size_t cap_len;

static int cap_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    memcpy(cap_buf + cap_len, buf, len);
    cap_len += len;
    return 0;
}

// --- dribble transport: serves a buffer 1 byte per read (worst-case
// partial reads), then clean close ---
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} dribble_t;

static int dribble_read(void *ctx, uint8_t *buf, size_t len)
{
    dribble_t *d = ctx;
    (void)len;
    if (d->pos >= d->len) {
        return 0;
    }
    buf[0] = d->data[d->pos++];
    return 1;
}

static void test_varint(void)
{
    uint8_t out[5];
    CHECK(frame_varint_encode(0, out) == 1 && out[0] == 0x00);
    CHECK(frame_varint_encode(1, out) == 1 && out[0] == 0x01);
    CHECK(frame_varint_encode(127, out) == 1 && out[0] == 0x7f);
    CHECK(frame_varint_encode(128, out) == 2 && out[0] == 0x80 && out[1] == 0x01);
    CHECK(frame_varint_encode(300, out) == 2 && out[0] == 0xac && out[1] == 0x02);
    CHECK(frame_varint_encode(16384, out) == 3);
}

static void test_send_matches_golden(void)
{
    polo_wire_protobuf_OuterMessage msg = polo_wire_protobuf_OuterMessage_init_zero;
    msg.protocol_version = 2;
    msg.status = polo_wire_protobuf_OuterMessage_Status_STATUS_OK;
    msg.has_pairing_request = true;
    strcpy(msg.pairing_request.service_name, "atvremote");
    msg.pairing_request.has_client_name = true;
    strcpy(msg.pairing_request.client_name, "esp32-androidtv");

    cap_len = 0;
    CHECK(frame_send(polo_wire_protobuf_OuterMessage_fields, &msg, cap_write, NULL) == 0);
    CHECK(cap_len == sizeof(golden_pairing_request));
    CHECK(memcmp(cap_buf, golden_pairing_request, cap_len) == 0);

    polo_wire_protobuf_OuterMessage msg2 = polo_wire_protobuf_OuterMessage_init_zero;
    msg2.protocol_version = 2;
    msg2.status = polo_wire_protobuf_OuterMessage_Status_STATUS_OK;
    msg2.has_secret = true;
    msg2.secret.secret.size = 32;
    for (int i = 0; i < 32; i++) {
        msg2.secret.secret.bytes[i] = (uint8_t)i;
    }
    cap_len = 0;
    CHECK(frame_send(polo_wire_protobuf_OuterMessage_fields, &msg2, cap_write, NULL) == 0);
    CHECK(cap_len == sizeof(golden_secret));
    CHECK(memcmp(cap_buf, golden_secret, cap_len) == 0);
}

static void test_recv_partial_reads_and_backtoback(void)
{
    // Two golden frames back-to-back, delivered one byte at a time.
    uint8_t stream[sizeof(golden_pairing_request) + sizeof(golden_secret)];
    memcpy(stream, golden_pairing_request, sizeof(golden_pairing_request));
    memcpy(stream + sizeof(golden_pairing_request), golden_secret, sizeof(golden_secret));
    dribble_t d = {stream, sizeof(stream), 0};

    uint8_t buf[FRAME_MAX_MSG];
    int len = frame_recv(buf, sizeof(buf), dribble_read, &d);
    CHECK(len == (int)sizeof(golden_pairing_request) - 1);  // 1-byte varint prefix

    polo_wire_protobuf_OuterMessage decoded = polo_wire_protobuf_OuterMessage_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, (size_t)len);
    CHECK(pb_decode(&is, polo_wire_protobuf_OuterMessage_fields, &decoded));
    CHECK(decoded.protocol_version == 2);
    CHECK(decoded.status == polo_wire_protobuf_OuterMessage_Status_STATUS_OK);
    CHECK(decoded.has_pairing_request);
    CHECK(strcmp(decoded.pairing_request.service_name, "atvremote") == 0);
    CHECK(strcmp(decoded.pairing_request.client_name, "esp32-androidtv") == 0);

    len = frame_recv(buf, sizeof(buf), dribble_read, &d);
    CHECK(len == (int)sizeof(golden_secret) - 1);
    polo_wire_protobuf_OuterMessage decoded2 = polo_wire_protobuf_OuterMessage_init_zero;
    pb_istream_t is2 = pb_istream_from_buffer(buf, (size_t)len);
    CHECK(pb_decode(&is2, polo_wire_protobuf_OuterMessage_fields, &decoded2));
    CHECK(decoded2.has_secret && decoded2.secret.secret.size == 32);
    CHECK(decoded2.secret.secret.bytes[31] == 31);

    // Stream exhausted: next recv reports clean close.
    CHECK(frame_recv(buf, sizeof(buf), dribble_read, &d) == FRAME_ERR_CLOSED);
}

static void test_recv_multibyte_varint(void)
{
    // Synthetic 300-byte frame: varint 0xac 0x02 + payload.
    uint8_t stream[2 + 300];
    stream[0] = 0xac;
    stream[1] = 0x02;
    for (int i = 0; i < 300; i++) {
        stream[2 + i] = (uint8_t)i;
    }
    dribble_t d = {stream, sizeof(stream), 0};
    uint8_t buf[FRAME_MAX_MSG];
    int len = frame_recv(buf, sizeof(buf), dribble_read, &d);
    CHECK(len == 300);
    CHECK(buf[0] == 0 && buf[299] == (uint8_t)299);
}

static void test_recv_errors(void)
{
    uint8_t buf[FRAME_MAX_MSG];

    // Truncated mid-payload: claims 10 bytes, delivers 3.
    uint8_t trunc[] = {0x0a, 0x01, 0x02, 0x03};
    dribble_t d = {trunc, sizeof(trunc), 0};
    CHECK(frame_recv(buf, sizeof(buf), dribble_read, &d) == FRAME_ERR_CLOSED);

    // Length exceeding FRAME_MAX_MSG (0xffff = 65535).
    uint8_t big[] = {0xff, 0xff, 0x03};
    dribble_t d2 = {big, sizeof(big), 0};
    CHECK(frame_recv(buf, sizeof(buf), dribble_read, &d2) == FRAME_ERR_TOOBIG);

    // Malformed varint (6 continuation bytes).
    uint8_t bad[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    dribble_t d3 = {bad, sizeof(bad), 0};
    CHECK(frame_recv(buf, sizeof(buf), dribble_read, &d3) == FRAME_ERR_VARINT);
}

int main(void)
{
    test_varint();
    test_send_matches_golden();
    test_recv_partial_reads_and_backtoback();
    test_recv_multibyte_varint();
    test_recv_errors();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all framing tests passed\n");
    return 0;
}

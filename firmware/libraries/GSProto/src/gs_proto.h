/**
 * gs_proto.h - AutoLink V2.2 portable frame API.
 *
 * Protocol-visible constants are generated from spec/autolink_spec.py.
 * Parser statuses below are local results and MUST NOT be emitted as wire
 * exception codes.
 */
#ifndef GS_PROTO_H
#define GS_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include "generated/autolink_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GS_PARSE_OK = 0,
    GS_PARSE_NULL = 1,
    GS_PARSE_TOO_SHORT = 2,
    GS_PARSE_BAD_VERSION = 3,
    GS_PARSE_BAD_LENGTH = 4,
    GS_PARSE_BAD_CRC = 5,
    GS_PARSE_BAD_VALUE = 6
} gs_parse_status_t;

typedef struct {
    uint8_t ver;
    uint8_t addr;
    uint8_t seq;
    uint8_t func;
    uint8_t len;
    uint8_t data[GS_DATA_MAX_LEN];
} gs_frame_t;

typedef struct {
    uint8_t id;
    int16_t raw;
} gs_channel_t;

/* Portable implementation capacity, not a Wire allocation or enum limit. */
#ifndef GS_CODEC_CHANNEL_CAPACITY
#define GS_CODEC_CHANNEL_CAPACITY 8u
#endif
#define GS_CH_MAX GS_CODEC_CHANNEL_CAPACITY

typedef struct {
    uint8_t id;
    uint8_t value_len;
    uint8_t value[4];
} gs_sample_channel_t;

typedef struct {
    uint8_t sensor_type;
    uint8_t channel_count;
    uint16_t schema_rev;
    gs_sample_channel_t channels[GS_CODEC_CHANNEL_CAPACITY];
    uint8_t status;
} gs_sample_t;

typedef struct {
    uint8_t ch_id;
    uint8_t quantity_id;
    uint8_t data_type;
    uint8_t unit;
    int8_t exp;
    uint8_t flags;
} gs_chdesc_t;

uint32_t gs_murmur3_x86_32(const void *key, size_t len, uint32_t seed);
uint16_t gs_crc16(const uint8_t *buf, uint16_t len);
uint16_t gs_frame_build(uint8_t *buf, const gs_frame_t *f);
/** Return GS_PARSE_OK or a negative gs_parse_status_t value. */
int gs_frame_parse(const uint8_t *buf, uint16_t len, gs_frame_t *f);

uint16_t gs_get_u16be(const uint8_t *p);
int16_t gs_get_i16be(const uint8_t *p);
void gs_put_u16be(uint8_t *p, uint16_t v);
void gs_put_i16be(uint8_t *p, int16_t v);
uint32_t gs_get_u32be(const uint8_t *p);
void gs_put_u32be(uint8_t *p, uint32_t v);

int gs_parse_sample(const uint8_t *data, uint8_t len,
                    uint8_t *type, gs_channel_t *ch, uint8_t *nch,
                    uint8_t *status);
uint8_t gs_build_sample_v22(uint8_t *data, const gs_sample_t *sample);
int gs_parse_sample_v22(const uint8_t *data, uint8_t len, gs_sample_t *sample);
uint8_t gs_build_chdesc(uint8_t *out, const gs_chdesc_t *d);
int gs_parse_chdesc(const uint8_t *p, uint8_t len, gs_chdesc_t *d);

#ifdef __cplusplus
}
#endif
#endif /* GS_PROTO_H */

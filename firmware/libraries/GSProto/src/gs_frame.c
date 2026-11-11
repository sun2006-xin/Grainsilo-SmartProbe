/**
 * gs_frame.c - 帧组包/解析与数据编解码（AutoLink V2.0 线格式 0x01）
 * 帧格式：[VER:1][ADDR:1][SEQ:1][FUNC:1][LEN:1][DATA:LEN][CRC16:2 低字节在前]
 * V2.0：帧头 +SEQ；LEN 上限 247（总帧 254）；0x04 通道描述符 TLV 编解码。
 */
#include "gs_proto.h"

/* ---------------- 帧编解码 ---------------- */

uint16_t gs_frame_build(uint8_t *buf, const gs_frame_t *f)
{
    uint16_t total;

    if (buf == NULL || f == NULL || f->len > GS_DATA_MAX_LEN) {
        return 0;
    }
    total = (uint16_t)5u + f->len + 2u;

    buf[0] = GS_WIRE_VER;              /* 线格式版本自动填充 */
    buf[1] = f->addr;
    buf[2] = f->seq;
    buf[3] = f->func;
    buf[4] = f->len;
    for (uint16_t i = 0; i < f->len; i++) {
        buf[5u + i] = f->data[i];
    }
    {
        uint16_t crc = gs_crc16(buf, 5u + f->len);
        buf[5u + f->len]     = (uint8_t)(crc & 0xFF);        /* 低字节在前 */
        buf[5u + f->len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    }
    return total;
}

int gs_frame_parse(const uint8_t *buf, uint16_t len, gs_frame_t *f)
{
    uint16_t crc;
    uint16_t recv_crc;
    uint8_t  data_len;

    if (buf == NULL || f == NULL) {
        return -GS_PARSE_NULL;
    }
    if (len < 7) {                        /* 最小帧：5 头 + 2 CRC */
        return -GS_PARSE_TOO_SHORT;
    }
    if (buf[0] != GS_WIRE_VER) {          /* 线格式版本不匹配：防不同版本固件混用 */
        return -GS_PARSE_BAD_VERSION;
    }
    data_len = buf[4];
    if (data_len > GS_DATA_MAX_LEN) {
        return -GS_PARSE_BAD_LENGTH;
    }
    if (len != (uint16_t)5u + data_len + 2u) {
        return -GS_PARSE_BAD_LENGTH;
    }

    crc = gs_crc16(buf, 5u + data_len);
    recv_crc = (uint16_t)buf[5u + data_len] | ((uint16_t)buf[5u + data_len + 1] << 8);
    if (crc != recv_crc) {
        return -GS_PARSE_BAD_CRC;
    }

    f->ver  = buf[0];
    f->addr = buf[1];
    f->seq  = buf[2];
    f->func = buf[3];
    f->len  = data_len;
    for (uint16_t i = 0; i < data_len; i++) {
        f->data[i] = buf[5u + i];
    }
    return 0;
}

/* ---------------- 大端字节序工具 ---------------- */

uint16_t gs_get_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int16_t gs_get_i16be(const uint8_t *p)
{
    return (int16_t)gs_get_u16be(p);
}

void gs_put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

void gs_put_i16be(uint8_t *p, int16_t v)
{
    gs_put_u16be(p, (uint16_t)v);
}

uint32_t gs_get_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

void gs_put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

/* ---------------- 0x03 测量数据编解码（通道化） ---------------- */

int gs_parse_sample(const uint8_t *data, uint8_t len,
                    uint8_t *type, gs_channel_t *ch, uint8_t *nch, uint8_t *status)
{
    gs_sample_t sample;
    uint8_t i;
    int parsed;

    if (data == NULL || ch == NULL || nch == NULL) {
        return -GS_PARSE_NULL;
    }
    parsed = gs_parse_sample_v22(data, len, &sample);
    if (parsed != GS_PARSE_OK) return parsed;
    for (i = 0; i < sample.channel_count; ++i) {
        if (sample.channels[i].value_len != 2u) return -GS_PARSE_BAD_VALUE;
        ch[i].id = sample.channels[i].id;
        ch[i].raw = gs_get_i16be(sample.channels[i].value);
    }
    if (type != NULL) *type = sample.sensor_type;
    *nch = sample.channel_count;
    if (status != NULL) *status = sample.status;
    return GS_PARSE_OK;
}

uint8_t gs_build_sample_v22(uint8_t *data, const gs_sample_t *sample)
{
    uint16_t pos = 4u;
    uint8_t i;
    if (data == NULL || sample == NULL ||
        sample->channel_count > GS_CODEC_CHANNEL_CAPACITY ||
        !gs_sensor_type_valid(sample->sensor_type) ||
        (sample->status & (0xFFu ^ GS_STATUS_ALLOWED_MASK)) != 0u) return 0u;
    data[0] = sample->sensor_type;
    data[1] = sample->channel_count;
    gs_put_u16be(data + 2u, sample->schema_rev);
    for (i = 0u; i < sample->channel_count; ++i) {
        const gs_sample_channel_t *channel = &sample->channels[i];
        uint8_t j;
        if (channel->id == 0u || channel->value_len == 0u ||
            channel->value_len > sizeof(channel->value) ||
            pos + 2u + channel->value_len + 1u > GS_DATA_MAX_LEN) return 0u;
        for (j = 0u; j < i; ++j)
            if (sample->channels[j].id == channel->id) return 0u;
        data[pos++] = channel->id;
        data[pos++] = channel->value_len;
        for (j = 0u; j < channel->value_len; ++j) data[pos++] = channel->value[j];
    }
    data[pos++] = sample->status;
    return (uint8_t)pos;
}

int gs_parse_sample_v22(const uint8_t *data, uint8_t len, gs_sample_t *sample)
{
    uint16_t pos = 4u;
    uint8_t i;
    if (data == NULL || sample == NULL) return -GS_PARSE_NULL;
    if (len < 5u) return -GS_PARSE_TOO_SHORT;
    sample->sensor_type = data[0];
    sample->channel_count = data[1];
    sample->schema_rev = gs_get_u16be(data + 2u);
    if (!gs_sensor_type_valid(sample->sensor_type) ||
        sample->channel_count > GS_CODEC_CHANNEL_CAPACITY)
        return -GS_PARSE_BAD_VALUE;
    for (i = 0u; i < sample->channel_count; ++i) {
        gs_sample_channel_t *channel = &sample->channels[i];
        uint8_t j;
        if (pos + 2u >= len) return -GS_PARSE_BAD_LENGTH;
        channel->id = data[pos++];
        channel->value_len = data[pos++];
        if (channel->id == 0u || channel->value_len == 0u ||
            channel->value_len > sizeof(channel->value) ||
             pos + channel->value_len >= len) return -GS_PARSE_BAD_VALUE;
        for (j = 0u; j < i; ++j)
            if (sample->channels[j].id == channel->id)
                return -GS_PARSE_BAD_VALUE;
        for (j = 0u; j < channel->value_len; ++j) channel->value[j] = data[pos++];
    }
    if (pos + 1u != len) return -GS_PARSE_BAD_LENGTH;
    sample->status = data[pos];
    if ((sample->status & (0xFFu ^ GS_STATUS_ALLOWED_MASK)) != 0u)
        return -GS_PARSE_BAD_VALUE;
    return GS_PARSE_OK;
}

/* ---------------- 0x04 通道描述符 TLV 编解码 ---------------- */

uint8_t gs_build_chdesc(uint8_t *out, const gs_chdesc_t *d)
{
    if (out == NULL || d == NULL || d->ch_id == 0u ||
        !gs_quantity_id_valid(d->quantity_id) ||
        !gs_data_type_valid(d->data_type) ||
        !gs_unit_id_valid(d->unit) ||
        !gs_descriptor_flags_valid(d->flags)) {
        return 0;
    }
    out[0] = GS_DESC_TLV_CHANNEL;
    out[1] = 6;                            /* VALUE 长度 */
    out[2] = d->ch_id;
    out[3] = d->quantity_id;
    out[4] = d->data_type;
    out[5] = d->unit;
    out[6] = (uint8_t)d->exp;              /* int8 保持位模式 */
    out[7] = d->flags;
    return 8;
}

int gs_parse_chdesc(const uint8_t *p, uint8_t len, gs_chdesc_t *d)
{
    if (p == NULL || d == NULL) {
        return -GS_PARSE_NULL;
    }
    if (len < 8) {
        return -GS_PARSE_TOO_SHORT;
    }
    if (p[0] != GS_DESC_TLV_CHANNEL || p[1] != 6) {
        return -GS_PARSE_BAD_VALUE;
    }
    if (p[2] == 0u || !gs_quantity_id_valid(p[3]) ||
        !gs_data_type_valid(p[4]) || !gs_unit_id_valid(p[5]) ||
        !gs_descriptor_flags_valid(p[7]))
        return -GS_PARSE_BAD_VALUE;
    d->ch_id     = p[2];
    d->quantity_id = p[3];
    d->data_type = p[4];
    d->unit      = p[5];
    d->exp       = (int8_t)p[6];
    d->flags     = p[7];
    return 0;
}

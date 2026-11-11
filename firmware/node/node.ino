/** AutoLink V2.2 SC2 ESP32 node adapter.
 *
 * Wire validation, state transitions, persistence ordering, response layouts,
 * duplicate handling and timers are owned by AutoLinkNodeEngine.  This file
 * adapts UART, Preferences and the physical sensor only.
 */
#include <autolink_node_engine.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_timer.h>
#include "../integration/esp32_journal_storage.h"
#include "../integration/rs485_stream_decoder.h"
#include "sensors.h"

#define PIN_485_TX 6
#define PIN_485_RX 7
#define PIN_485_DE 10
#define PIN_SDA 4
#define PIN_SCL 5
#define TX_SETTLE_US 1200
#define GS_SHT31_I2C_TIMEOUT_MS 25u

static al_node_engine_t s_engine;
static uint8_t s_uid[GS_UID_LEN];
static uint8_t s_rx[GS_FRAME_MAX_LEN];
static uint16_t s_rx_len;
static al_time_us_t s_rx_last_us;
static Preferences s_preferences;
/* Temporary low-rate USB diagnostics for the missing-0x01 investigation. */
static uint32_t s_diag_rx_bytes;
static uint32_t s_diag_rx_frames;
static uint32_t s_diag_handled_frames;
static uint32_t s_diag_ignored_frames;
static uint32_t s_diag_handled_read_addr_frames;
static uint32_t s_diag_handled_read_sample_frames;
static uint32_t s_diag_tx_frames;
static uint32_t s_diag_parse_errors;
static uint32_t s_diag_dropped_bytes;
static uint32_t s_diag_sensor_reads;
static uint32_t s_diag_sensor_retries;
static uint32_t s_diag_sensor_errors;
static int s_diag_last_parse_status;
static uint8_t s_diag_last_rx_addr;
static uint8_t s_diag_last_rx_seq;
static uint8_t s_diag_last_rx_func;
static uint8_t s_diag_last_rx_action;
static uint8_t s_diag_last_rx_error;
static uint8_t s_diag_last_tx_addr;
static uint8_t s_diag_last_tx_func;
static uint8_t s_diag_last_tx_len;
static uint32_t s_diag_last_report_ms;

static void diagnostic_status(uint32_t now_ms)
{
    if (!Serial || now_ms - s_diag_last_report_ms < 5000u) return;
    s_diag_last_report_ms = now_ms;
    al_node_platform_status_t platform = {};
    (void)al_node_engine_platform_status(&s_engine, &platform);
    Serial.printf(
        "[C3-STAT] build=GS-20261010-NODE-I2C1 up=%lu baud=%lu "
        "sensor=%u rxB=%lu rxF=%lu handled/ignored=%lu/%lu "
        "handledAddr/sample=%lu/%lu "
        "lastRx=%02X/%u/%02X/a%u/e%02X lastTx=%02X/%02X/%u txF=%lu "
        "parseErr=%lu dropB=%lu sensorRead/retry/err=%lu/%lu/%lu\n",
        (unsigned long)now_ms, (unsigned long)platform.active_baud,
        (unsigned)sensor_current_type(),
        (unsigned long)s_diag_rx_bytes, (unsigned long)s_diag_rx_frames,
        (unsigned long)s_diag_handled_frames,
        (unsigned long)s_diag_ignored_frames,
        (unsigned long)s_diag_handled_read_addr_frames,
        (unsigned long)s_diag_handled_read_sample_frames,
        (unsigned)s_diag_last_rx_addr, (unsigned)s_diag_last_rx_seq,
        (unsigned)s_diag_last_rx_func, (unsigned)s_diag_last_rx_action,
        (unsigned)s_diag_last_rx_error,
        (unsigned)s_diag_last_tx_addr, (unsigned)s_diag_last_tx_func,
        (unsigned)s_diag_last_tx_len,
        (unsigned long)s_diag_tx_frames,
        (unsigned long)s_diag_parse_errors,
        (unsigned long)s_diag_dropped_bytes,
        (unsigned long)s_diag_sensor_reads,
        (unsigned long)s_diag_sensor_retries,
        (unsigned long)s_diag_sensor_errors);
}

static uint32_t node_lifecycle_ms(al_time_us_t monotonic_us)
{
    return (uint32_t)(monotonic_us / UINT64_C(1000));
}

static void make_uid(uint8_t *uid, uint64_t value)
{
    for (uint8_t i = 0u; i < GS_UID_LEN; ++i)
        uid[i] = (uint8_t)(value >> (8u * (GS_UID_LEN - 1u - i)));
}

static bool storage_read(void *, uint8_t slot, void *data, size_t length)
{
    return grainsilo_nvs_journal_read(s_preferences, slot, data, length,
                                      "al_slot0", "al_slot1");
}

static bool storage_write(void *, uint8_t slot, const void *data, size_t length)
{
    return grainsilo_nvs_journal_write(s_preferences, slot, data, length,
                                       "al_slot0", "al_slot1");
}

static bool read_sample(void *, gs_sample_t *sample)
{
    gs_channel_t channels[GS_CODEC_CHANNEL_CAPACITY];
    uint8_t count = 0u;
    uint8_t status = 0u;
    ++s_diag_sensor_reads;
    if (!sensor_read(channels, &count, &status)) {
        ++s_diag_sensor_retries;
        sensor_auto_detect();
        /* A retry is a new measurement attempt; do not carry error bits or
         * channel metadata from the failed first read into a valid retry. */
        count = 0u;
        status = 0u;
        if (!sensor_read(channels, &count, &status)) {
            ++s_diag_sensor_errors;
            status |= GS_STATUS_SENSOR_ERR;
            memset(sample, 0, sizeof(*sample));
            sample->sensor_type = sensor_current_type();
            sample->status = status;
            return false;
        }
    }
    memset(sample, 0, sizeof(*sample));
    sample->sensor_type = sensor_current_type();
    sample->channel_count = count;
    sample->status = status;
    for (uint8_t i = 0u; i < count; ++i) {
        sample->channels[i].id = channels[i].id;
        sample->channels[i].value_len = 2u;
        gs_put_i16be(sample->channels[i].value, channels[i].raw);
    }
    return true;
}

static bool read_device_info(void *, al_device_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->firmware_version = 0x0102u;
    info->sensor_type = sensor_current_type();
    info->capabilities = GS_CAP_CONFIGURABLE | GS_CAP_SLEEP | GS_CAP_DIAGNOSTIC;
    if (info->sensor_type == GS_SENSOR_SHT31) {
        gs_chdesc_t *temperature = &info->descriptors[0];
        gs_chdesc_t *humidity = &info->descriptors[1];
        temperature->ch_id = GS_CH_TEMP;
        temperature->quantity_id = GS_QTY_TEMPERATURE;
        temperature->data_type = GS_DESC_DT_INT16;
        temperature->unit = GS_DESC_UNIT_CELSIUS;
        temperature->exp = -2;
        temperature->flags = GS_DESC_FLAG_RO;
        humidity->ch_id = GS_CH_RH;
        humidity->quantity_id = GS_QTY_RELATIVE_HUMIDITY;
        humidity->data_type = GS_DESC_DT_UINT16;
        humidity->unit = GS_DESC_UNIT_RH;
        humidity->exp = -2;
        humidity->flags = GS_DESC_FLAG_RO;
        info->descriptor_count = 2u;
    }
    return true;
}

static al_transport_timestamp_t send_frame(const gs_frame_t *frame)
{
    uint8_t bytes[GS_FRAME_MAX_LEN];
    uint16_t length = gs_frame_build(bytes, frame);
    if (length == 0u)
        return al_transport_timestamp(AL_BUS_EVENT_TX_CRC_STOP, 0u,
                                      UINT32_MAX, false);
    digitalWrite(PIN_485_DE, HIGH);
    Serial1.write(bytes, length);
    Serial1.flush();
    ++s_diag_tx_frames;
    s_diag_last_tx_addr = frame->addr;
    s_diag_last_tx_func = frame->func;
    s_diag_last_tx_len = (uint8_t)frame->len;
    al_time_us_t tx_crc_stop_us = (al_time_us_t)esp_timer_get_time();
    delayMicroseconds(TX_SETTLE_US);
    digitalWrite(PIN_485_DE, LOW);
    while (Serial1.available() > 0) (void)Serial1.read();
    /* Serial.flush completion is an adapter observation.  No oscilloscope or
     * UART-event calibration is available in this package, so the physical
     * CRC-stop uncertainty is explicitly unbounded. */
    return al_transport_timestamp(AL_BUS_EVENT_TX_CRC_STOP,
                                  tx_crc_stop_us, UINT32_MAX, false);
}

static void emit_action(const gs_frame_t *input, const al_action_t *action)
{
    gs_frame_t output;
    if (action->kind != AL_ACTION_ERROR &&
        action->kind != AL_ACTION_IMMEDIATE_RESPONSE &&
        action->kind != AL_ACTION_SCHEDULED_RESPONSE) return;
    if (action->kind == AL_ACTION_SCHEDULED_RESPONSE) {
        while ((int64_t)(action->scheduled_start_bit_us -
                         (al_time_us_t)esp_timer_get_time()) > 50)
            delayMicroseconds(25u);
        while ((int64_t)(action->scheduled_start_bit_us -
                         (al_time_us_t)esp_timer_get_time()) > 0) { }
    }
    memset(&output, 0, sizeof(output));
    output.addr = action->response_addr;
    output.seq = input->seq;
    output.func = action->response_func;
    output.len = action->response_len;
    memcpy(output.data, action->response_data, output.len);
    al_transport_timestamp_t delivery_end = send_frame(&output);
    if (action->kind == AL_ACTION_SCHEDULED_RESPONSE)
        (void)al_node_engine_on_delivery(&s_engine, action,
                                         AL_DELIVERY_DELIVERED,
                                         node_lifecycle_ms(delivery_end.at_us));
}

static bool apply_due_time_effect(const al_action_t *action)
{
    if (!al_node_engine_effect_is_due(&s_engine, action)) return false;
    if (action->effect == AL_EFFECT_SET_BAUD) {
        Serial1.updateBaudRate(action->effect_value);
        return true;
    }
    if (action->effect == AL_EFFECT_ENTER_SLEEP) {
        esp_sleep_enable_timer_wakeup(
            (uint64_t)action->effect_value * 1000ULL);
        esp_deep_sleep_start();
    }
    return false;
}

static bool advance_node_time(uint32_t now_ms)
{
    for (;;) {
        al_action_t action;
        if (!al_node_engine_advance(&s_engine, now_ms, &action)) return false;
        if (action.effect == AL_EFFECT_NONE) return true;
        if (!apply_due_time_effect(&action)) return false;
    }
}

static void process_frame(const gs_frame_t *frame, al_time_us_t crc_stop_us)
{
    ++s_diag_rx_frames;
    s_diag_last_rx_addr = frame->addr;
    s_diag_last_rx_seq = frame->seq;
    s_diag_last_rx_func = frame->func;
    s_diag_last_rx_error = 0u;
    al_request_t request = {.addr = frame->addr, .seq = frame->seq,
                            .func = frame->func, .len = frame->len,
                            .data = frame->data,
                            .now_ms = node_lifecycle_ms(crc_stop_us),
                            .request_crc_stop = al_transport_timestamp(
                                AL_BUS_EVENT_RX_CRC_STOP, crc_stop_us,
                                UINT32_MAX, false)};
    for (;;) {
        al_action_t action = al_node_engine_execute(&s_engine, &request);
        s_diag_last_rx_action = (uint8_t)action.kind;
        s_diag_last_rx_error = action.wire_error;
        if (!al_node_engine_effect_is_due(&s_engine, &action)) {
            if (action.kind == AL_ACTION_IGNORE) {
                ++s_diag_ignored_frames;
            } else {
                ++s_diag_handled_frames;
                if (frame->func == GS_FUNC_READ_ADDR)
                    ++s_diag_handled_read_addr_frames;
                if (frame->func == GS_FUNC_READ_SAMPLE)
                    ++s_diag_handled_read_sample_frames;
            }
            emit_action(frame, &action);
            if (Serial && (frame->func == GS_FUNC_READ_ADDR ||
                           action.kind == AL_ACTION_ERROR)) {
                Serial.printf(
                    "[C3-FRAME] rx_addr=0x%02X seq=%u func=0x%02X len=%u "
                    "response_addr=0x%02X action=%u err=0x%02X tx_addr=0x%02X "
                    "tx_func=0x%02X tx_len=%u data0=0x%02X\n",
                    (unsigned)frame->addr, (unsigned)frame->seq,
                    (unsigned)frame->func, (unsigned)frame->len,
                    (unsigned)action.response_addr, (unsigned)action.kind,
                    (unsigned)action.wire_error,
                    (unsigned)action.response_addr,
                    (unsigned)action.response_func,
                    (unsigned)action.response_len,
                    action.response_len > 0u ?
                        (unsigned)action.response_data[0] : 0u);
            }
            return;
        }
        if (!apply_due_time_effect(&action)) return;
    }
}

static void poll_uart(void)
{
    while (Serial1.available() > 0) {
        uint8_t byte = (uint8_t)Serial1.read();
        ++s_diag_rx_bytes;
        s_rx_last_us = (al_time_us_t)esp_timer_get_time();
        if (s_rx_len >= GS_FRAME_MAX_LEN) {
            memmove(s_rx, s_rx + 1u, GS_FRAME_MAX_LEN - 1u);
            --s_rx_len;
        }
        s_rx[s_rx_len++] = byte;

        for (;;) {
            gs_frame_t frame;
            uint16_t discarded_prefix = 0u;
            int parse_status = 0;
            if (!grainsilo_rs485_stream_take_frame(
                    s_rx, &s_rx_len, &frame, &discarded_prefix,
                    &parse_status)) {
                if (parse_status != 0) {
                    ++s_diag_parse_errors;
                    s_diag_last_parse_status = parse_status;
                }
                break;
            }
            if (discarded_prefix > 0u)
                s_diag_dropped_bytes += discarded_prefix;
            process_frame(&frame, s_rx_last_us);
        }
    }
    if (s_rx_len > 0u && (al_time_us_t)esp_timer_get_time() - s_rx_last_us >=
        (al_time_us_t)GS_TIMING_T_IFG_MS * 1000u) {
        s_diag_dropped_bytes += s_rx_len;
        if (Serial) {
            Serial.printf("[C3-DROP] bytes=%u parse=%d\n",
                          (unsigned)s_rx_len,
                          s_diag_last_parse_status);
        }
        s_rx_len = 0u;
        s_diag_last_parse_status = 0;
    }
}

static void apply_due_effects(void)
{
    (void)advance_node_time(
        node_lifecycle_ms((al_time_us_t)esp_timer_get_time()));
}

void setup(void)
{
    al_storage_port_t storage = {NULL, storage_read, storage_write};
    al_node_application_port_t application = {NULL, read_sample, read_device_info};
    al_node_platform_status_t platform;
    Serial.begin(115200);
    make_uid(s_uid, ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL);
    s_preferences.begin("autolink", false);
    al_node_engine_init(&s_engine, s_uid, storage);
    (void)al_node_engine_restore(&s_engine);
    al_node_engine_set_application_port(&s_engine, application);
    pinMode(PIN_485_DE, OUTPUT);
    digitalWrite(PIN_485_DE, LOW);
    if (!al_node_engine_platform_status(&s_engine, &platform)) return;
    Serial1.begin(platform.active_baud, SERIAL_8N1, PIN_485_RX, PIN_485_TX);
    Wire.begin(PIN_SDA, PIN_SCL, 100000);
    /* Bound each bus transaction. The SHT31 conversion delay is handled
     * separately by the driver, so a stuck I2C operation must not consume
     * AutoLink's entire response deadline before the node can answer. */
    Wire.setTimeOut(GS_SHT31_I2C_TIMEOUT_MS);
    sensor_auto_detect();
    (void)al_node_engine_sync_schema(&s_engine);
    diagnostic_status(millis());
}

void loop(void)
{
    poll_uart();
    apply_due_effects();
    diagnostic_status(millis());
}

/* AUTO-GENERATED; DO NOT EDIT. */
#ifndef AUTOLINK_WIRE_GENERATED_H
#define AUTOLINK_WIRE_GENERATED_H
#include <stdint.h>

#define GS_WIRE_VER                          0x01u
#define GS_PROTO_VER                         512u
#define GS_ADDR_BROADCAST                    0x00u
#define GS_ADDR_MIN                          0x01u
#define GS_ADDR_MAX                          0xEFu
#define GS_ADDR_SYSTEM                       0xF0u
#define GS_ADDR_UNASSIGNED                   0xF8u
#define GS_ADDR_RESERVED                     0xF9u
#define GS_ADDR_INVALID                      0xFFu
#define GS_FUNC_READ_SAMPLE                  0x03u
#define GS_FUNC_READ_INFO                    0x04u
#define GS_FUNC_DISCOVER                     0x05u
#define GS_FUNC_WRITE_CFG                    0x06u
#define GS_FUNC_RECONCILE                    0x07u
#define GS_FUNC_DIAG_LOOP                    0x08u
#define GS_FUNC_REG_REQ                      0x10u
#define GS_FUNC_ASSIGN_ADDR                  0x11u
#define GS_FUNC_REG_CONFIRM                  0x12u
#define GS_FUNC_READ_ADDR                    0x13u
#define GS_FUNC_SLEEP                        0x17u
#define GS_FUNC_EVENT_PULL                   0x1Au
#define GS_FUNC_NET_MGMT                     0x1Cu
#define GS_FUNC_READDRESS                    0x1Du
#define GS_FUNC_RECOVER                      0x1Eu
#define GS_FUNC_ERR_BASE                     0x80u
#define GS_REPORT_REGISTER                   0x01u
#define GS_REPORT_RECONCILE                  0x02u
#define GS_REPORT_RECOVER                    0x03u
#define GS_NET_OP_PREPARE                    0x01u
#define GS_NET_OP_COMMIT                     0x02u
#define GS_NET_OP_QUERY                      0x03u
#define GS_QUERY_READY                       0x00u
#define GS_QUERY_REJECT                      0x01u
#define GS_QUERY_REASON_BUSY                 0x01u
#define GS_QUERY_REASON_NO_PREPARE           0x02u
#define GS_QUERY_REASON_UNSUPPORTED_BAUD     0x03u
#define GS_WIRE_ERR_ILLEGAL_FUNC             0x01u
#define GS_WIRE_ERR_BAD_PAYLOAD_LEN          0x02u
#define GS_WIRE_ERR_INVALID_VALUE            0x03u
#define GS_WIRE_ERR_SENSOR                   0x04u
#define GS_WIRE_ERR_BUSY                     0x05u
#define GS_WIRE_ERR_PROTO_FEATURE            0x07u
#define GS_WIRE_ERR_NET_ID                   0x08u
#define GS_WIRE_ERR_TXN_CONFLICT             0x09u
#define GS_WIRE_ERR_PERSIST_FAILED           0x0Au
#define GS_INFO_SUB_BASE                     0x00u
#define GS_INFO_SUB_CHDESC                   0x01u
#define GS_SENSOR_GENERIC                    0x00u
#define GS_SENSOR_SHT31                      0x01u
#define GS_CAP_CONFIGURABLE                  0x01u
#define GS_CAP_SLEEP                         0x04u
#define GS_CAP_DIAGNOSTIC                    0x20u
#define GS_CH_TEMP                           0x01u
#define GS_CH_RH                             0x02u
#define GS_QTY_TEMPERATURE                   0x01u
#define GS_QTY_RELATIVE_HUMIDITY             0x02u
#define GS_QTY_CO2_CONCENTRATION             0x03u
#define GS_QTY_MOISTURE_CONTENT              0x04u
#define GS_QTY_BATTERY_SOC                   0x05u
#define GS_QTY_BATTERY_VOLTAGE               0x06u
#define GS_QTY_PRESSURE                      0x07u
#define GS_QTY_FLOW                          0x08u
#define GS_DESC_DT_INT16                     0x01u
#define GS_DESC_DT_UINT16                    0x02u
#define GS_DESC_DT_UINT32                    0x03u
#define GS_DESC_UNIT_CELSIUS                 0x01u
#define GS_DESC_UNIT_RH                      0x02u
#define GS_DESC_UNIT_PPM                     0x03u
#define GS_DESC_UNIT_PCT                     0x04u
#define GS_DESC_UNIT_METER                   0x05u
#define GS_DESC_UNIT_VOLT                    0x06u
#define GS_DESC_UNIT_MA                      0x07u
#define GS_STATUS_SENSOR_ERR                 0x01u
#define GS_STATUS_LOW_BAT                    0x02u
#define GS_STATUS_CFG_ERR                    0x04u
#define GS_STATUS_ALLOWED_MASK               0x07u
#define GS_DESC_TLV_CHANNEL                  0x01u
#define GS_DESC_FLAG_RO                      0x01u
#define GS_SENSOR_MIN                        0x00u
#define GS_SENSOR_MAX                        0x01u
#define GS_QUANTITY_MIN                      0x01u
#define GS_QUANTITY_MAX                      0x08u
#define GS_DATA_TYPE_MIN                     0x01u
#define GS_DATA_TYPE_MAX                     0x03u
#define GS_UNIT_MIN                          0x01u
#define GS_UNIT_MAX                          0x07u
#define GS_CAP_ALLOWED_MASK                  0x25u
#define GS_DESC_FLAGS_ALLOWED_MASK           0x01u
#define GS_UID_LEN                           0x06u
#define GS_NET_ID_LEN                        0x08u
#define GS_SEED_LEN                          0x04u
#define GS_FRAME_MAX_LEN                     0xFEu
#define GS_DATA_MAX_LEN                      0xF7u
#define GS_SLOTS                             0x20u
#define GS_COLLIDE_FUSE                      0x02u
#define GS_BAUD_DEFAULT                      9600u
#define GS_BAUD_9600                         9600u
#define GS_BAUD_19200                        19200u
#define GS_BAUD_57600                        57600u
#define GS_BAUD_115200                       115200u
#define GS_TIMING_T_IFG_MS                   0x04u
#define GS_TIMING_T_JITTER_MS                0x01u
#define GS_TIMING_SCAN_INTERVAL_MS           5000u
#define GS_TIMING_GRACE_PERIOD_MS            15000u
#define GS_TIMING_RESP_TIMEOUT_MS            300u
#define GS_TIMING_RESP_TIMEOUT_DIAG_MS       350u
#define GS_TIMING_REG_TIMEOUT_MS             10000u
#define GS_TIMING_ASSIGN_CONFIRM_TIMEOUT_MS  20000u
#define GS_TIMING_DUP_CACHE_TTL_MS           60000u
#define GS_TIMING_PREPARE_TIMEOUT_MS         90000u
#define GS_TIMING_SLEEP_ARMED_WINDOW_MS      2000u
#define GS_TIMING_COMMIT_COPY_GAP_MS         0x14u
#define GS_TIMING_COMMIT_REPLAY_WINDOW_MS    300u
#define GS_TIMING_SWITCH_AFTER_MS            0xC8u
#define GS_TIMING_SWITCH_TIMER_GUARD_MS      0x05u
#define GS_TIMING_MASTER_SILENCE_MS          325u
#define GS_TIMING_MAX_SWITCH_SKEW_BOUND_MS   0x64u
#define GS_TIMING_T_SLOT_DISCOVER_MS         0x14u
#define GS_TIMING_T_SLOT_RECONCILE_MS        0x15u
#define GS_TIMING_T_SLOT_RECOVER_MS          0x1Eu
#define GS_TRANSACTION_COMMIT_COPIES         0x03u
#define GS_TRANSACTION_RETRY_MAX             0x02u
#define GS_TRANSACTION_OFFLINE_THRESHOLD     0x03u
#define GS_Q_BACKOFF_1_MS                    5000u
#define GS_Q_BACKOFF_2_MS                    15000u
#define GS_Q_BACKOFF_3_MS                    60000u
#define GS_Q_BACKOFF_MAX_MS                  300000u

static inline int gs_sensor_type_valid(uint8_t value)
{
    return value <= 1u;
}

static inline int gs_quantity_id_valid(uint8_t value)
{
    return value >= 1u && value <= 8u;
}

static inline int gs_data_type_valid(uint8_t value)
{
    return value >= 1u && value <= 3u;
}

static inline int gs_unit_id_valid(uint8_t value)
{
    return value >= 1u && value <= 7u;
}

static inline int gs_capabilities_valid(uint8_t value)
{
    return (value & (0xFFu ^ GS_CAP_ALLOWED_MASK)) == 0u;
}

static inline int gs_descriptor_flags_valid(uint8_t value)
{
    return (value & (0xFFu ^ GS_DESC_FLAGS_ALLOWED_MASK)) == 0u;
}

#endif

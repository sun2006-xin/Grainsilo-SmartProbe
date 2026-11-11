/**
 * gs_crc16.c - Modbus CRC-16 实现
 * 多项式 0xA001（反射 0x8005），初值 0xFFFF，输出低字节在前
 * 标准验证向量："123456789" -> 0x4B37
 */
#include "gs_proto.h"

uint16_t gs_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

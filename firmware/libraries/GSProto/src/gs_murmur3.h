/**
 * gs_murmur3.h - MurmurHash3_x86_32（AutoLink V2.1 Hash Slot，ADR-0021）
 *
 * V2.1 职责分离（PROTOCOL.md §9）：CRC16 只做帧检错，Hash Slot 改用
 * MurmurHash3_x86_32——slot = gs_murmur3_x86_32(UID[6], seed=SEED4) % GS_SLOTS。
 * 理由：CRC16 是 GF(2) 线性映射，约 3.1% 的 UID 对构成"恒等对"（任意
 * NONCE/SEED 同槽，确定性哈希无法拆散）；Murmur3 无此结构性缺陷，
 * 任何 UID 对在每轮新 SEED 下同槽概率 = 1/SLOTS（纯统计碰撞，可重排）。
 *
 * 与 tools/sim_bus.py 的 murmur3_x86_32 同构（Python 参考实现 + 测试向量）。
 * 纯 C 零依赖，无平台头文件——与 gs_proto.h 同一约束。
 */
#ifndef GS_MURMUR3_H
#define GS_MURMUR3_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MurmurHash3_x86_32（标准算法，seed 32bit，输出 32bit） */
uint32_t gs_murmur3_x86_32(const void *key, size_t len, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif /* GS_MURMUR3_H */

/**
 * gs_murmur3.c - MurmurHash3_x86_32 实现
 *
 * 标准 MurmurHash3 算法（Austin Appleby，CC0/public domain），
 * 与 tools/sim_bus.py 的 murmur3_x86_32 逐字节同构。
 * 测试向量（seed=0）：空输入 -> 0x00000000；"abc" -> 0xB3DD93FA
 * （MurmurHash3 官方测试集，tools/test_gsproto.py 已覆盖）。
 */
#include "gs_murmur3.h"

uint32_t gs_murmur3_x86_32(const void *key, size_t len, uint32_t seed)
{
    const uint8_t *data = (const uint8_t *)key;
    const int nblocks = (int)(len / 4);
    const uint32_t c1 = 0xCC9E2D51u;
    const uint32_t c2 = 0x1B873593u;
    uint32_t h1 = seed;
    int i;

    /* 每 4 字节一个块：k1 混入（c1 乘 -> 左旋 15 -> c2 乘 -> 异或进 h1 -> 左旋 13 -> 乘 5 加 0xE6546B64） */
    for (i = 0; i < nblocks; i++) {
        uint32_t k1 = (uint32_t)data[i * 4]
                    | ((uint32_t)data[i * 4 + 1] << 8)
                    | ((uint32_t)data[i * 4 + 2] << 16)
                    | ((uint32_t)data[i * 4 + 3] << 24);
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5 + 0xE6546B64u;
    }

    /* 尾部 1-3 字节（小端拼接，完整 k1 才做混入） */
    {
        const uint8_t *tail = data + (size_t)nblocks * 4;
        uint32_t k1 = 0;
        switch (len & 3) {
        case 3: k1 ^= (uint32_t)tail[2] << 16;  /* fall through */
        case 2: k1 ^= (uint32_t)tail[1] << 8;   /* fall through */
        case 1:
            k1 ^= tail[0];
            k1 *= c1;
            k1 = (k1 << 15) | (k1 >> 17);
            k1 *= c2;
            h1 ^= k1;
            break;
        default:
            break;
        }
    }

    /* finalization：长度混入 + 三次 fmix（0x85EBCA6B / 0xC2B2AE35） */
    h1 ^= (uint32_t)len;
    h1 ^= h1 >> 16;
    h1 *= 0x85EBCA6Bu;
    h1 ^= h1 >> 13;
    h1 *= 0xC2B2AE35u;
    h1 ^= h1 >> 16;
    return h1;
}

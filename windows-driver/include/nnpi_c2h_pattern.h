/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_C2H_PATTERN_H
#define NNPI_C2H_PATTERN_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
typedef u64 nnpi_pattern_u64;
typedef u32 nnpi_pattern_u32;
typedef u8 nnpi_pattern_u8;
#else
#include <stdint.h>
#include <string.h>
typedef uint64_t nnpi_pattern_u64;
typedef uint32_t nnpi_pattern_u32;
typedef uint8_t nnpi_pattern_u8;
#endif
/* Non-secret, endian-independent per-word pattern. seed+(word+1)*GOLD is
 * injective over 2^64 (GOLD odd): the 2M words of a 16MiB window never repeat,
 * so every absolute byte position has a distinct value. Not cryptographic
 * authentication; dropping the two fmix multiplies lets nnpi_c2h_fill advance
 * by one add per word instead of three multiplies. */
#define NNPI_C2H_GOLD 0x9e3779b97f4a7c15ULL
static inline nnpi_pattern_u64 nnpi_c2h_word(nnpi_pattern_u64 run,
 nnpi_pattern_u32 generation,nnpi_pattern_u32 word)
{
 return (run ^ ((nnpi_pattern_u64)generation<<32))
        + (nnpi_pattern_u64)(word+1u)*NNPI_C2H_GOLD;
}
static inline nnpi_pattern_u8 nnpi_c2h_byte(nnpi_pattern_u64 run,
 nnpi_pattern_u32 generation,nnpi_pattern_u32 offset)
{ return (nnpi_pattern_u8)(nnpi_c2h_word(run,generation,offset>>3)>>(8u*(offset&7u))); }
static inline void nnpi_c2h_fill(nnpi_pattern_u8 *p,nnpi_pattern_u32 bytes,
 nnpi_pattern_u64 run,nnpi_pattern_u32 generation,nnpi_pattern_u32 offset,int poison)
{
 nnpi_pattern_u32 i=0,j;
 nnpi_pattern_u64 v=nnpi_c2h_word(run,generation,offset>>3);
 nnpi_pattern_u64 mask=poison?0xffffffffffffffffULL:0ull;
 nnpi_pattern_u32 h=offset&7u;
 if(h) {
  nnpi_pattern_u32 n=(8u-h<bytes)?8u-h:bytes;
  for(j=0;j<n;j++) p[j]=(nnpi_pattern_u8)((v>>(8u*(h+j)))^(poison?255u:0u));
  i=n; v+=NNPI_C2H_GOLD;
 }
 for(;i+8u<=bytes;i+=8u) {
  nnpi_pattern_u64 w=v^mask;
  /* 8-byte store: byte order of nnpi_c2h_byte is little-endian, and both the
   * card (Ice Lake) and host (x64) are LE, so this matches the 8 scalar stores
   * it replaces while advancing one add per word. */
  memcpy(p+i,&w,8);
  v+=NNPI_C2H_GOLD;
 }
 for(j=0;i<bytes;i++,j++) p[i]=(nnpi_pattern_u8)((v>>(8u*j))^(poison?255u:0u));
}
#endif

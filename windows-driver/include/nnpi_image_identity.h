/* Reviewed offline image identity; private image bytes are never packaged. */
#ifndef NNPI_IMAGE_IDENTITY_H
#define NNPI_IMAGE_IDENTITY_H
#include <stdint.h>
#include <string.h>
/* Three reviewed images are allowed:
 *   936f4888...  vendor bulk-1150-hostwin-x11-v4-0319 image (known good)
 *   61879001...  Alpine generic build v3 (NBD-capable)
 *   2d27de4e...  Mint RAM-root image v2: rawtun + tmpfs root + writeback.
 * All are exactly 188,744,192 bytes. */
#define NNPI_APPROVED_IMAGE_CONFIGURED 1
#define NNPI_APPROVED_IMAGE_COUNT 3
#define NNPI_APPROVED_IMAGE_BYTES UINT64_C(188744192)
#define NNPI_APPROVED_IMAGE_SHA256 { \
 "936f4888a0501117cd9307de720c3caadc91e05ddecbd064cebfb6d14d4841b5", \
 "6187900152a6b6876e2128fe14cb84bcde2df02532a1699d7dc1dbc20214037e", \
 "2d27de4e1363250a04de52e68efcb35f719839045aea032e651b27c25e2da55e" }
#define NNPI_APPROVED_IMAGE_DIGEST { \
 {0x93, 0x6f, 0x48, 0x88, 0xa0, 0x50, 0x11, 0x17, 0xcd, 0x93, 0x07, 0xde, 0x72, 0x0c, 0x3c, 0xaa, 0xdc, 0x91, 0xe0, 0x5d, 0xde, 0xcb, 0xd0, 0x64, 0xce, 0xbf, 0xb6, 0xd1, 0x4d, 0x48, 0x41, 0xb5}, \
 {0x61, 0x87, 0x90, 0x01, 0x52, 0xa6, 0xb6, 0x87, 0x6e, 0x21, 0x28, 0xfe, 0x14, 0xcb, 0x84, 0xbc, 0xde, 0x2d, 0xf0, 0x25, 0x32, 0xa1, 0x69, 0x9d, 0x7d, 0xc1, 0xdb, 0xc2, 0x02, 0x14, 0x03, 0x7e}, \
 {0x2d, 0x27, 0xde, 0x4e, 0x13, 0x63, 0x25, 0x0a, 0x04, 0xde, 0x52, 0xe6, 0x8e, 0xfc, 0xb3, 0x5f, 0x71, 0x98, 0x39, 0x04, 0x5a, 0xea, 0x03, 0x2e, 0x65, 0x1b, 0x27, 0xc2, 0x5e, 0x2d, 0xa5, 0x5e} }
static inline int nnpi_image_identity_matches(const uint8_t digest[32],uint64_t bytes)
{
#if NNPI_APPROVED_IMAGE_CONFIGURED
 static const uint8_t approved[NNPI_APPROVED_IMAGE_COUNT][32]=
  NNPI_APPROVED_IMAGE_DIGEST;
 unsigned i;
 if (!digest || bytes!=NNPI_APPROVED_IMAGE_BYTES) return 0;
 for (i=0;i<NNPI_APPROVED_IMAGE_COUNT;++i)
  if (!memcmp(digest,approved[i],32)) return 1;
 return 0;
#else
 (void)digest;(void)bytes;return 0;
#endif
}
static inline int nnpi_image_identity_hex_matches(const char *hex)
{
#if NNPI_APPROVED_IMAGE_CONFIGURED
 static const char *approved[NNPI_APPROVED_IMAGE_COUNT]=
  NNPI_APPROVED_IMAGE_SHA256;
 unsigned i,j;
 if (!hex) return 0;
 for (i=0;i<NNPI_APPROVED_IMAGE_COUNT;++i) {
  for (j=0;j<64;++j) {
   char a=hex[j],b=approved[i][j];
   if (a>='A' && a<='F') a=(char)(a-'A'+'a');
   if (a!=b) break;
  }
  if (j==64 && hex[64]=='\0') return 1;
 }
 return 0;
#else
 (void)hex;return 0;
#endif
}
#endif
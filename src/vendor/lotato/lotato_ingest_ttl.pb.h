/* nanopb header for Lotato ingest TTL rows (LoDB ingest_ttl table). */
#ifndef PB_LOTATO_INGEST_TTL_PB_H_INCLUDED
#define PB_LOTATO_INGEST_TTL_PB_H_INCLUDED
#include <pb.h>

#if PB_PROTO_HEADER_VERSION != 40
#error Regenerate this file with the current version of nanopb generator.
#endif

typedef PB_BYTES_ARRAY_T(4) LotatoIngestTtl_prefix_t;

typedef struct _LotatoIngestTtl {
  LotatoIngestTtl_prefix_t pub_key_prefix;
  uint32_t last_posted_unix;
  uint32_t reserved;
} LotatoIngestTtl;

#ifdef __cplusplus
extern "C" {
#endif

#define LotatoIngestTtl_init_default {{4, {0}}, 0, 0}
#define LotatoIngestTtl_init_zero {{0, {0}}, 0, 0}

#define LotatoIngestTtl_pub_key_prefix_tag 1
#define LotatoIngestTtl_last_posted_unix_tag 2
#define LotatoIngestTtl_reserved_tag 3

#define LotatoIngestTtl_FIELDLIST(X, a) \
  X(a, STATIC, SINGULAR, BYTES, pub_key_prefix, 1) \
  X(a, STATIC, SINGULAR, UINT32, last_posted_unix, 2) \
  X(a, STATIC, SINGULAR, UINT32, reserved, 3)
#define LotatoIngestTtl_CALLBACK NULL
#define LotatoIngestTtl_DEFAULT NULL

extern const pb_msgdesc_t LotatoIngestTtl_msg;
#define LotatoIngestTtl_fields &LotatoIngestTtl_msg
#define LotatoIngestTtl_size 32

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

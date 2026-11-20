/*
 * Signing for the tests (t-code itself only checks): an index made while a test runs gets its
 * index.json.sig from the test key, whose seed is 00 01 .. 1f; $TCODE_REGISTRY_KEY = TEST_REGISTRY_KEY
 * makes t-code check with it. Includes plugins/ed25519.c for its field, point and scalar code, with its
 * one public function renamed so that it can sit beside t-code's own copy.
 */
#pragma once

#define ed25519_verify test_ed25519_verify
#include "plugins/ed25519.c"
#undef ed25519_verify

#define TEST_REGISTRY_KEY "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg="

static void test_sha512(guint8 *digest, const guint8 *a, gsize a_length, const guint8 *b, gsize b_length,
                        const guint8 *c, gsize c_length)
{
  GChecksum *sha512 = g_checksum_new(G_CHECKSUM_SHA512);
  gsize length = 64;

  g_checksum_update(sha512, a, a_length);
  if (b_length)
    g_checksum_update(sha512, b, b_length);
  if (c_length)
    g_checksum_update(sha512, c, c_length);
  g_checksum_get_digest(sha512, digest, &length);
  g_checksum_free(sha512);
}

/* The base64 signature of the test key over text (RFC 8032, 5.1.6). Newly allocated. */
static G_GNUC_UNUSED char *test_sign(const char *text, gsize length)
{
  guint8 seed[32], expanded[64], key[32], digest[64], r[32], k[32], signature[64];
  gint64 x[64];
  Field p[4];

  for (int i = 0; i < 32; i++)
    seed[i] = i;
  test_sha512(expanded, seed, 32, NULL, 0, NULL, 0);
  expanded[0] &= 248;
  expanded[31] &= 127;
  expanded[31] |= 64;
  point_multiply_base(p, expanded);
  point_pack(key, p);

  test_sha512(digest, expanded + 32, 32, (const guint8 *)text, length, NULL, 0);
  for (int i = 0; i < 64; i++)
    x[i] = digest[i];
  scalar_reduce(r, x);
  point_multiply_base(p, r);
  point_pack(signature, p); /* R */

  test_sha512(digest, signature, 32, key, 32, (const guint8 *)text, length);
  for (int i = 0; i < 64; i++)
    x[i] = digest[i];
  scalar_reduce(k, x);
  for (int i = 0; i < 64; i++)
    x[i] = i < 32 ? r[i] : 0;
  for (int i = 0; i < 32; i++)
    for (int j = 0; j < 32; j++)
      x[i + j] += (gint64)k[i] * expanded[j];
  scalar_reduce(signature + 32, x); /* S = r + k a */
  return g_base64_encode(signature, 64);
}

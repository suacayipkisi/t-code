/*
 * Ed25519 signature checks (see ed25519.h), after TweetNaCl's crypto_sign_open (public domain): field
 * elements are 16 limbs of 16 bits, points extended coordinates (X, Y, Z, T). SHA-512 is GLib's. Nothing
 * here is secret, so nothing needs to run in constant time.
 */
#include "plugins/ed25519.h"

#include <string.h>

typedef gint64 Field[16];

/* clang-format off */
static const Field field_zero = { 0 }, field_one = { 1 };
static const Field curve_d = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                               0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 };
static const Field curve_d2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                                0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
static const Field base_x = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                              0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 };
static const Field base_y = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                              0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 };
static const Field sqrt_minus_one = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                                      0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83 };
/* The group's order L, little-endian. */
static const gint64 order[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2,
                                  0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
/* clang-format on */

/* The field --------------------------------------------------------------------------------------- */

static void field_set(Field out, const Field in)
{
  memcpy(out, in, sizeof(Field));
}

/* Carries each limb into the next (the top one, times 38, into the first). Multiplications instead of left
 * shifts: limbs may be negative. */
static void field_carry(Field o)
{
  for (int i = 0; i < 16; i++) {
    o[i] += 1LL << 16;
    gint64 c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c * 65536;
  }
}

/* Swaps p and q when bit is 1. */
static void field_swap(Field p, Field q, int bit)
{
  gint64 mask = ~((gint64)bit - 1);

  for (int i = 0; i < 16; i++) {
    gint64 t = mask & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

/* The 32 bytes of n, fully reduced. */
static void field_pack(guint8 *out, const Field n)
{
  Field m, t;

  field_set(t, n);
  field_carry(t);
  field_carry(t);
  field_carry(t);
  for (int j = 0; j < 2; j++) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; i++) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    int borrow = (m[15] >> 16) & 1;
    m[14] &= 0xffff;
    field_swap(t, m, 1 - borrow);
  }
  for (int i = 0; i < 16; i++) {
    out[2 * i] = t[i] & 0xff;
    out[2 * i + 1] = t[i] >> 8;
  }
}

static gboolean field_differ(const Field a, const Field b)
{
  guint8 x[32], y[32];

  field_pack(x, a);
  field_pack(y, b);
  return memcmp(x, y, 32) != 0;
}

static int field_parity(const Field a)
{
  guint8 d[32];

  field_pack(d, a);
  return d[0] & 1;
}

static void field_unpack(Field o, const guint8 *n)
{
  for (int i = 0; i < 16; i++)
    o[i] = n[2 * i] + ((gint64)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}

static void field_add(Field o, const Field a, const Field b)
{
  for (int i = 0; i < 16; i++)
    o[i] = a[i] + b[i];
}

static void field_sub(Field o, const Field a, const Field b)
{
  for (int i = 0; i < 16; i++)
    o[i] = a[i] - b[i];
}

static void field_mul(Field o, const Field a, const Field b)
{
  gint64 t[31] = { 0 };

  for (int i = 0; i < 16; i++)
    for (int j = 0; j < 16; j++)
      t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; i++)
    t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; i++)
    o[i] = t[i];
  field_carry(o);
  field_carry(o);
}

static void field_square(Field o, const Field a)
{
  field_mul(o, a, a);
}

/* i to the power 2^252 - 3: the square root's exponent. */
static void field_pow2523(Field o, const Field i)
{
  Field c;

  field_set(c, i);
  for (int a = 250; a >= 0; a--) {
    field_square(c, c);
    if (a != 1)
      field_mul(c, c, i);
  }
  field_set(o, c);
}

/* 1 / i (i to the power p - 2). */
static void field_invert(Field o, const Field i)
{
  Field c;

  field_set(c, i);
  for (int a = 253; a >= 0; a--) {
    field_square(c, c);
    if (a != 2 && a != 4)
      field_mul(c, c, i);
  }
  field_set(o, c);
}

/* Points ------------------------------------------------------------------------------------------ */

/* p += q */
static void point_add(Field p[4], Field q[4])
{
  Field a, b, c, d, t, e, f, g, h;

  field_sub(a, p[1], p[0]);
  field_sub(t, q[1], q[0]);
  field_mul(a, a, t);
  field_add(b, p[0], p[1]);
  field_add(t, q[0], q[1]);
  field_mul(b, b, t);
  field_mul(c, p[3], q[3]);
  field_mul(c, c, curve_d2);
  field_mul(d, p[2], q[2]);
  field_add(d, d, d);
  field_sub(e, b, a);
  field_sub(f, d, c);
  field_add(g, d, c);
  field_add(h, b, a);
  field_mul(p[0], e, f);
  field_mul(p[1], h, g);
  field_mul(p[2], g, f);
  field_mul(p[3], e, h);
}

static void point_swap(Field p[4], Field q[4], int bit)
{
  for (int i = 0; i < 4; i++)
    field_swap(p[i], q[i], bit);
}

static void point_pack(guint8 *out, Field p[4])
{
  Field tx, ty, zi;

  field_invert(zi, p[2]);
  field_mul(tx, p[0], zi);
  field_mul(ty, p[1], zi);
  field_pack(out, ty);
  out[31] ^= field_parity(tx) << 7;
}

/* p = s q (s: 32 bytes, little-endian); q is used up. */
static void point_multiply(Field p[4], Field q[4], const guint8 *s)
{
  field_set(p[0], field_zero);
  field_set(p[1], field_one);
  field_set(p[2], field_one);
  field_set(p[3], field_zero);
  for (int i = 255; i >= 0; i--) {
    int bit = (s[i / 8] >> (i & 7)) & 1;
    point_swap(p, q, bit);
    point_add(q, p);
    point_add(p, p);
    point_swap(p, q, bit);
  }
}

/* p = s B */
static void point_multiply_base(Field p[4], const guint8 *s)
{
  Field q[4];

  field_set(q[0], base_x);
  field_set(q[1], base_y);
  field_set(q[2], field_one);
  field_mul(q[3], base_x, base_y);
  point_multiply(p, q, s);
}

/* Whether the y packed in 32 bytes (the top bit is x's sign) is below p = 2^255 - 19: y and y + p would
 * otherwise be two spellings of one key. */
static gboolean field_canonical(const guint8 *packed)
{
  if ((packed[31] & 0x7f) != 0x7f)
    return TRUE;
  for (int i = 30; i > 0; i--) {
    if (packed[i] != 0xff)
      return TRUE;
  }
  return packed[0] < 0xed;
}

/* r = -(the point packed in 32 bytes); FALSE when they are not a point, or not its one spelling (RFC 8032
 * 5.1.3: y below p, and no sign bit on x = 0). */
static gboolean point_unpack_negated(Field r[4], const guint8 *packed)
{
  Field t, check, num, den, den2, den4, den6;

  if (!field_canonical(packed))
    return FALSE;
  field_set(r[2], field_one);
  field_unpack(r[1], packed);
  field_square(num, r[1]);
  field_mul(den, num, curve_d);
  field_sub(num, num, r[2]);
  field_add(den, r[2], den);
  field_square(den2, den);
  field_square(den4, den2);
  field_mul(den6, den4, den2);
  field_mul(t, den6, num);
  field_mul(t, t, den);
  field_pow2523(t, t);
  field_mul(t, t, num);
  field_mul(t, t, den);
  field_mul(t, t, den);
  field_mul(r[0], t, den);
  field_square(check, r[0]);
  field_mul(check, check, den);
  if (field_differ(check, num))
    field_mul(r[0], r[0], sqrt_minus_one);
  field_square(check, r[0]);
  field_mul(check, check, den);
  if (field_differ(check, num))
    return FALSE;
  if (!field_differ(r[0], field_zero) && packed[31] >> 7)
    return FALSE;
  if (field_parity(r[0]) == (packed[31] >> 7))
    field_sub(r[0], field_zero, r[0]);
  field_mul(r[3], r[0], r[1]);
  return TRUE;
}

/* Scalars modulo L ------------------------------------------------------------------------------- */

/* r = x mod L (x: 64 limbs of one byte each, used up). */
static void scalar_reduce(guint8 *r, gint64 x[64])
{
  gint64 carry;
  int i, j;

  for (i = 63; i >= 32; i--) {
    carry = 0;
    for (j = i - 32; j < i - 12; j++) {
      x[j] += carry - 16 * x[i] * order[j - (i - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j] += carry;
    x[i] = 0;
  }
  carry = 0;
  for (j = 0; j < 32; j++) {
    x[j] += carry - (x[31] >> 4) * order[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  for (j = 0; j < 32; j++)
    x[j] -= carry * order[j];
  for (i = 0; i < 32; i++) {
    x[i + 1] += x[i] >> 8;
    r[i] = x[i] & 255;
  }
}

/* Whether s (32 bytes, little-endian) is below L. */
static gboolean scalar_canonical(const guint8 *s)
{
  for (int i = 31; i >= 0; i--) {
    if (s[i] != order[i])
      return s[i] < order[i];
  }
  return FALSE; /* s == L */
}

/* The check ---------------------------------------------------------------------------------------- */

gboolean ed25519_verify(const guint8 *signature, const guint8 *message, gsize length, const guint8 *key)
{
  Field p[4], q[4];
  guint8 digest[64], h[32], packed[32];
  gsize digest_length = sizeof digest;
  gint64 x[64];
  GChecksum *sha512;

  if (!signature || !key || (!message && length) || !scalar_canonical(signature + 32) || !point_unpack_negated(q, key))
    return FALSE;
  sha512 = g_checksum_new(G_CHECKSUM_SHA512);
  g_checksum_update(sha512, signature, 32); /* R */
  g_checksum_update(sha512, key, 32);
  if (length)
    g_checksum_update(sha512, message, length);
  g_checksum_get_digest(sha512, digest, &digest_length);
  g_checksum_free(sha512);
  for (int i = 0; i < 64; i++)
    x[i] = digest[i];
  scalar_reduce(h, x);

  point_multiply(p, q, h);                /* -h A */
  point_multiply_base(q, signature + 32); /* S B */
  point_add(p, q);                        /* S B - h A, which is R when the signature is good */
  point_pack(packed, p);
  return memcmp(packed, signature, 32) == 0;
}

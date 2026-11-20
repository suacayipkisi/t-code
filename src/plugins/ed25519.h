/*
 * Checking an Ed25519 signature (RFC 8032; GLib only): the registry's index is signed with one, and t-code
 * holds the public key. Only checking: t-code never signs anything.
 */
#pragma once

#include <glib.h>

/* Whether signature (64 bytes) is key's (32 bytes) over message. FALSE for anything malformed too: a key
 * that is not a point, a signature whose S is not below the group's order. */
gboolean ed25519_verify(const guint8 *signature, const guint8 *message, gsize length, const guint8 *key);

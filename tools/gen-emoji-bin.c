/*
 * Generates src/editor/preview/emoji.bin and src/editor/preview/emoji_bin.h
 *
 * Written in 100% pure standard C (no GLib, no external dependencies).
 * Compile:
 *   cc -O2 -Wall -Wextra -std=c23 -o tools/gen-emoji-bin tools/gen-emoji-bin.c
 * Usage:
 *   tools/gen-emoji-bin [tools/emoji.json]
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
  uint16_t name_offset;
  uint16_t name_len;
  uint16_t text_offset;
  uint16_t text_len;
} EmojiBinEntry;

typedef struct {
  char magic[4]; /* "EMOJ" */
  uint32_t count;
} EmojiBinHeader;
#pragma pack(pop)

typedef struct {
  char *name;
  char *text;
} EmojiPair;

typedef struct {
  EmojiPair *items;
  size_t count;
  size_t capacity;
} PairList;

static void pair_list_init(PairList *list)
{
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void pair_list_append(PairList *list, const char *name, const char *text)
{
  if (list->count >= list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 256;
    list->items = realloc(list->items, list->capacity * sizeof(EmojiPair));
  }
  list->items[list->count].name = strdup(name);
  list->items[list->count].text = strdup(text);
  list->count++;
}

static void pair_list_free(PairList *list)
{
  for (size_t i = 0; i < list->count; i++) {
    free(list->items[i].name);
    free(list->items[i].text);
  }
  free(list->items);
}

static int compare_pairs(const void *a, const void *b)
{
  const EmojiPair *pa = a;
  const EmojiPair *pb = b;
  return strcmp(pa->name, pb->name);
}

/* Extract string between quotes: "key": "value" */
static bool extract_quoted(const char *src, char *out, size_t max_out)
{
  const char *q1 = strchr(src, '"');
  if (!q1)
    return false;
  const char *q2 = strchr(q1 + 1, '"');
  if (!q2)
    return false;

  size_t len = q2 - (q1 + 1);
  if (len >= max_out)
    len = max_out - 1;
  memcpy(out, q1 + 1, len);
  out[len] = '\0';
  return true;
}

static void parse_json_file(FILE *f, PairList *list)
{
  char line[4096];
  char current_name[256] = { 0 };
  char current_emoji[256] = { 0 };

  while (fgets(line, sizeof(line), f)) {
    char *name_key = strstr(line, "\"name\":");
    if (name_key) {
      extract_quoted(name_key + 7, current_name, sizeof(current_name));
    }

    char *emoji_key = strstr(line, "\"emoji\":");
    if (emoji_key) {
      extract_quoted(emoji_key + 8, current_emoji, sizeof(current_emoji));
    }

    if (current_name[0] && current_emoji[0]) {
      pair_list_append(list, current_name, current_emoji);
      current_name[0] = '\0';
      current_emoji[0] = '\0';
    }
  }
}

/* Buffer structure for building binary */
typedef struct {
  uint8_t *data;
  size_t size;
  size_t capacity;
} ByteBuffer;

static void buf_append(ByteBuffer *b, const void *data, size_t len)
{
  if (b->size + len > b->capacity) {
    b->capacity = (b->size + len) * 2;
    if (b->capacity < 1024)
      b->capacity = 1024;
    b->data = realloc(b->data, b->capacity);
  }
  memcpy(b->data + b->size, data, len);
  b->size += len;
}

int main(int argc, char **argv)
{
  const char *input_path = argc > 1 ? argv[1] : "tools/emoji.json";
  FILE *f = fopen(input_path, "r");
  if (!f) {
    /* Fallback to local emoji.json if run from tools/ */
    input_path = "emoji.json";
    f = fopen(input_path, "r");
  }

  if (!f) {
    fprintf(stderr, "Cannot open tools/emoji.json\n");
    fprintf(stderr, "Usage: %s [path/to/emoji.json]\n", argv[0]);
    return 1;
  }

  PairList list;
  pair_list_init(&list);
  parse_json_file(f, &list);
  fclose(f);

  if (list.count == 0) {
    fprintf(stderr, "No emoji pairs found in %s\n", input_path);
    return 1;
  }

  /* Sort by name (strcmp) */
  qsort(list.items, list.count, sizeof(EmojiPair), compare_pairs);
  printf("Loaded and sorted %zu emoji entries from %s\n", list.count, input_path);

  /* Build binary structure */
  ByteBuffer strings = { 0 };
  EmojiBinEntry *entries = malloc(list.count * sizeof(EmojiBinEntry));

  for (size_t i = 0; i < list.count; i++) {
    const char *name = list.items[i].name;
    const char *text = list.items[i].text;
    size_t nlen = strlen(name);
    size_t tlen = strlen(text);

    entries[i].name_offset = (uint16_t)strings.size;
    entries[i].name_len = (uint16_t)nlen;
    buf_append(&strings, name, nlen + 1);

    entries[i].text_offset = (uint16_t)strings.size;
    entries[i].text_len = (uint16_t)tlen;
    buf_append(&strings, text, tlen + 1);
  }

  EmojiBinHeader hdr = {
    .magic = { 'E', 'M', 'O', 'J' },
    .count = (uint32_t)list.count,
  };

  ByteBuffer bin = { 0 };
  buf_append(&bin, &hdr, sizeof(hdr));
  buf_append(&bin, entries, list.count * sizeof(EmojiBinEntry));
  buf_append(&bin, strings.data, strings.size);

  /* 1. Write emoji.bin */
  const char *bin_path = "src/editor/preview/emoji.bin";
  FILE *fbin = fopen(bin_path, "wb");
  if (fbin) {
    fwrite(bin.data, 1, bin.size, fbin);
    fclose(fbin);
    printf("Wrote %s (%zu bytes)\n", bin_path, bin.size);
  }

  /* 2. Write emoji_bin.h */
  const char *h_path = "src/editor/preview/emoji_bin.h";
  FILE *fh = fopen(h_path, "w");
  if (fh) {
    fprintf(fh, "/* Generated by tools/gen-emoji-bin: do not edit. */\n");
    fprintf(fh, "#pragma once\n\n");
    fprintf(fh, "#include <stddef.h>\n");
    fprintf(fh, "#include <stdint.h>\n");
    fprintf(fh, "#include <string.h>\n\n");

    fprintf(fh, "#pragma pack(push, 1)\n");
    fprintf(fh, "typedef struct {\n");
    fprintf(fh, "  uint16_t name_offset;\n");
    fprintf(fh, "  uint16_t name_len;\n");
    fprintf(fh, "  uint16_t text_offset;\n");
    fprintf(fh, "  uint16_t text_len;\n");
    fprintf(fh, "} EmojiBinEntry;\n\n");

    fprintf(fh, "typedef struct {\n");
    fprintf(fh, "  char magic[4]; /* 'EMOJ' */\n");
    fprintf(fh, "  uint32_t count;\n");
    fprintf(fh, "} EmojiBinHeader;\n");
    fprintf(fh, "#pragma pack(pop)\n\n");

    fprintf(fh, "/* Embedded binary payload (.rodata) */\n");
    fprintf(fh, "/* clang-format off */\n");
    fprintf(fh, "static const unsigned char emoji_bin[] = {\n");

    for (size_t i = 0; i < bin.size; i++) {
      if (i % 12 == 0)
        fprintf(fh, "  ");
      fprintf(fh, "0x%02x,", bin.data[i]);
      if (i % 12 == 11 || i == bin.size - 1)
        fprintf(fh, "\n");
      else
        fprintf(fh, " ");
    }

    fprintf(fh, "};\n");
    fprintf(fh, "/* clang-format on */\n\n");
    fprintf(fh, "static const size_t emoji_bin_len = sizeof(emoji_bin);\n\n");

    fprintf(fh, "/* Fast O(log N) lookup directly from the embedded binary data. */\n");
    fprintf(fh, "static inline const char *emoji_bin_lookup(const char *name, size_t len)\n");
    fprintf(fh, "{\n");
    fprintf(fh, "  if (emoji_bin_len < sizeof(EmojiBinHeader))\n");
    fprintf(fh, "    return NULL;\n\n");
    fprintf(fh, "  const EmojiBinHeader *hdr = (const EmojiBinHeader *)emoji_bin;\n");
    fprintf(fh, "  const EmojiBinEntry *entries = (const EmojiBinEntry *)(emoji_bin + sizeof(EmojiBinHeader));\n");
    fprintf(
        fh,
        "  const char *strings = (const char *)(emoji_bin + sizeof(EmojiBinHeader) + hdr->count * sizeof(EmojiBinEntry));\n\n");
    fprintf(fh, "  size_t low = 0;\n");
    fprintf(fh, "  size_t high = hdr->count;\n\n");
    fprintf(fh, "  while (low < high) {\n");
    fprintf(fh, "    size_t mid = low + (high - low) / 2;\n");
    fprintf(fh, "    const EmojiBinEntry *e = &entries[mid];\n");
    fprintf(fh, "    const char *entry_name = strings + e->name_offset;\n\n");
    fprintf(fh, "    size_t min_len = len < e->name_len ? len : e->name_len;\n");
    fprintf(fh, "    int cmp = strncmp(name, entry_name, min_len);\n");
    if (true) {
      fprintf(fh, "    if (cmp == 0) {\n");
      fprintf(fh, "      if (len < e->name_len)\n");
      fprintf(fh, "        cmp = -1;\n");
      fprintf(fh, "      else if (len > e->name_len)\n");
      fprintf(fh, "        cmp = 1;\n");
      fprintf(fh, "    }\n\n");
      fprintf(fh, "    if (cmp == 0)\n");
      fprintf(fh, "      return strings + e->text_offset;\n");
      fprintf(fh, "    if (cmp < 0)\n");
      fprintf(fh, "      high = mid;\n");
      fprintf(fh, "    else\n");
      fprintf(fh, "      low = mid + 1;\n");
    }
    fprintf(fh, "  }\n");
    fprintf(fh, "  return NULL;\n");
    fprintf(fh, "}\n");

    fclose(fh);
    printf("Wrote %s\n", h_path);
  }

  free(entries);
  free(strings.data);
  free(bin.data);
  pair_list_free(&list);
  return 0;
}

/*
 * The processes t-code runs, read from /proc (GLib-only): t-code itself and every descendant, each
 * with its CPU use, memory and the part of t-code it belongs to (a plugin, a language server, the
 * debugger, a terminal tab). The Process Monitor window (dialogs/processes.c) shows them.
 */
#pragma once

#include <glib.h>

/* Which part of t-code a process belongs to; also the order the owners are listed in. */
typedef enum {
  PROC_TCODE,    /* t-code itself, and helpers nobody else claims (git, rg, curl) */
  PROC_PLUGIN,   /* a plugin's program */
  PROC_SERVER,   /* a language server */
  PROC_DEBUGGER, /* gdb and the program it debugs */
  PROC_TERMINAL, /* a terminal tab's shell and what runs in it */
} ProcKind;

/* Who started a process: the modules fill a table of these (pid -> ProcOwner *, procs_owners_new);
 * a process without one belongs to its nearest ancestor's owner. */
typedef struct {
  ProcKind kind;
  char *label; /* "Plugin: Spell Check", "Language server: clangd", "Terminal: make" */
} ProcOwner;

typedef struct {
  int pid, ppid;
  int depth;      /* 0: the root (t-code), 1: its children, ... */
  guint64 start;  /* clock ticks after boot: tells a pid's process from a later one */
  char *name;     /* the command name ("clangd") */
  char *command;  /* the whole command line, or the name when it has none */
  double cpu;     /* percent of one CPU since the last sample; 0 on the first */
  guint64 memory; /* bytes: PSS (shared pages split between their users), RSS when PSS cannot be read */
  guint64 rss;    /* bytes: RSS (every shared page counted in full, as most system monitors show it) */
  int threads;
  const ProcOwner *owner; /* never NULL; belongs to the owners table given to procs_sample */
} Proc;

typedef struct ProcSampler ProcSampler;

/* An empty owners table for procs_own / procs_sample. */
GHashTable *procs_owners_new(void);

/* pid belongs to kind with that label (copied); a pid given twice keeps the first. Inline: the
 * modules that name their processes (plugins, language servers, gdb, terminals) need no more. */
static inline void procs_own(GHashTable *owners, int pid, ProcKind kind, const char *label)
{
  if (pid <= 0 || g_hash_table_contains(owners, GINT_TO_POINTER(pid)))
    return;
  ProcOwner *o = g_new(ProcOwner, 1);
  o->kind = kind;
  o->label = g_strdup(label);
  g_hash_table_insert(owners, GINT_TO_POINTER(pid), o);
}

ProcSampler *procs_sampler_new(void);
void procs_sampler_free(ProcSampler *s);

/* root and every descendant, root first, each followed by its own descendants (children by kind of
 * owner, then label, then pid; deeper ones by pid). The root belongs to PROC_TCODE "t-code" unless
 * owners says otherwise. CPU is measured against the sampler's last call. Proc * entries, freed
 * with the array, which must go before owners does. */
GPtrArray *procs_sample(ProcSampler *s, int root, GHashTable *owners);

/* Sends sig to pid while it is still the process started at start (through a pidfd: no reuse race);
 * FALSE when it is gone or the signal could not be sent. */
gboolean procs_signal(int pid, guint64 start, int sig);

/* "512 KB", "12.3 MB", "1.20 GB". */
char *procs_format_memory(guint64 bytes);

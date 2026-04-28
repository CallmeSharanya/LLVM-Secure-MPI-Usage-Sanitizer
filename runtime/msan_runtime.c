#include "msan_runtime.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MSAN_MAX_LOC
#define MSAN_MAX_LOC 256
#endif

#ifndef MSAN_MAX_TYPENAME
#define MSAN_MAX_TYPENAME 64
#endif

typedef enum {
  MSAN_EV_SEND = 1,
  MSAN_EV_RECV = 2,
} MsanEventKind;

typedef struct {
  uint32_t kind;
  int32_t rank;
  int32_t peer;      // dest for send, source for recv
  int32_t tag;
  int32_t count;
  int32_t type_size;
  uint64_t nbytes;
  uint64_t comm_f;   // MPI_Comm as Fortran handle
  uint64_t buf_addr; // raw pointer value
  uint64_t seq;
  char type_name[MSAN_MAX_TYPENAME];
  char loc[MSAN_MAX_LOC];
} MsanEvent;

typedef struct {
  MsanEvent *data;
  size_t len;
  size_t cap;
} MsanVec;

static MsanVec g_events;
static uint64_t g_seq = 0;
static int g_rank = -1;
static int g_size = -1;
static int g_inited = 0;

static MPI_Datatype msan_dt_from_handle(uint64_t h) {
  return (MPI_Datatype)(uintptr_t)h;
}

static MPI_Comm msan_comm_from_handle(uint64_t h) {
  return (MPI_Comm)(uintptr_t)h;
}

static void msan_init_if_needed(void) {
  if (g_inited)
    return;

  // Safe to call multiple times after MPI_Init.
  PMPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
  PMPI_Comm_size(MPI_COMM_WORLD, &g_size);

  // Make MPI calls return errors instead of aborting the whole job, so we can
  // still report sanitizer findings at finalize (e.g., MPI_ERR_TRUNCATE).
  (void)PMPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);

  g_inited = 1;
}

void __msan_init(const char *file, int line) {
  (void)file;
  (void)line;
  msan_init_if_needed();
}

static void msan_vec_push(MsanEvent ev) {
  if (g_events.len == g_events.cap) {
    size_t new_cap = (g_events.cap == 0) ? 64 : (g_events.cap * 2);
    void *p = realloc(g_events.data, new_cap * sizeof(MsanEvent));
    if (!p) {
      fprintf(stderr, "[msan] OOM while recording events\n");
      PMPI_Abort(MPI_COMM_WORLD, 2);
    }
    g_events.data = (MsanEvent *)p;
    g_events.cap = new_cap;
  }
  g_events.data[g_events.len++] = ev;
}

static void msan_set_typename(MPI_Datatype datatype, char out[MSAN_MAX_TYPENAME]) {
  int len = 0;
  out[0] = '\0';

  // MPI_Type_get_name is available in MPI-2+.
  PMPI_Type_get_name(datatype, out, &len);
  if (len <= 0) {
    snprintf(out, MSAN_MAX_TYPENAME, "<unnamed>");
  } else if (len >= MSAN_MAX_TYPENAME) {
    out[MSAN_MAX_TYPENAME - 1] = '\0';
  }
}

static void msan_set_loc(const char *file, int line, char out[MSAN_MAX_LOC]) {
  if (!file)
    file = "<unknown>";
  snprintf(out, MSAN_MAX_LOC, "%s:%d", file, line);
  out[MSAN_MAX_LOC - 1] = '\0';
}

static uint64_t msan_comm_id(MPI_Comm comm) {
  MPI_Fint f = PMPI_Comm_c2f(comm);
  return (uint64_t)f;
}

static int msan_type_size(MPI_Datatype datatype) {
  int sz = 0;
  PMPI_Type_size(datatype, &sz);
  return sz;
}

void __msan_before_send(void *buf, int count, uint64_t datatype_handle, int dest,
                        int tag, uint64_t comm_handle, const char *file,
                        int line) {
  msan_init_if_needed();

  MPI_Datatype datatype = msan_dt_from_handle(datatype_handle);
  MPI_Comm comm = msan_comm_from_handle(comm_handle);

  MsanEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.kind = MSAN_EV_SEND;
  ev.rank = g_rank;
  ev.peer = dest;
  ev.tag = tag;
  ev.count = count;
  ev.type_size = msan_type_size(datatype);
  ev.nbytes = (uint64_t)(count < 0 ? 0 : (uint64_t)count) * (uint64_t)ev.type_size;
  ev.comm_f = msan_comm_id(comm);
  ev.buf_addr = (uint64_t)(uintptr_t)buf;
  ev.seq = ++g_seq;
  msan_set_typename(datatype, ev.type_name);
  msan_set_loc(file, line, ev.loc);

  msan_vec_push(ev);
}

void __msan_after_recv(void *buf, int count, uint64_t datatype_handle,
                       int source, int tag, uint64_t comm_handle, void *status,
                       const char *file, int line) {
  msan_init_if_needed();

  MPI_Datatype datatype = msan_dt_from_handle(datatype_handle);
  MPI_Comm comm = msan_comm_from_handle(comm_handle);

  MPI_Status *st = (MPI_Status *)status;

  int actual_src = source;
  int actual_tag = tag;

  if (st && st != MPI_STATUS_IGNORE) {
    actual_src = st->MPI_SOURCE;
    actual_tag = st->MPI_TAG;
  }

  MsanEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.kind = MSAN_EV_RECV;
  ev.rank = g_rank;
  ev.peer = actual_src;
  ev.tag = actual_tag;
  ev.count = count;
  ev.type_size = msan_type_size(datatype);
  ev.nbytes = (uint64_t)(count < 0 ? 0 : (uint64_t)count) * (uint64_t)ev.type_size;
  ev.comm_f = msan_comm_id(comm);
  ev.buf_addr = (uint64_t)(uintptr_t)buf;
  ev.seq = ++g_seq;
  msan_set_typename(datatype, ev.type_name);
  msan_set_loc(file, line, ev.loc);

  msan_vec_push(ev);
}

static int ev_key_equal(const MsanEvent *s, const MsanEvent *r) {
  // Match send -> recv by (src=send.rank, dst=send.peer, tag, comm)
  if (s->kind != MSAN_EV_SEND || r->kind != MSAN_EV_RECV)
    return 0;
  if ((int32_t)s->rank != (int32_t)r->peer)
    return 0;
  if ((int32_t)s->peer != (int32_t)r->rank)
    return 0;
  if ((int32_t)s->tag != (int32_t)r->tag)
    return 0;
  if (s->comm_f != r->comm_f)
    return 0;
  return 1;
}

static void report_mismatch(const MsanEvent *send_ev, const MsanEvent *recv_ev,
                            const char *kind, const char *detail) {
  fprintf(stderr,
          "[msan][%s] %s\n"
          "  send: rank=%d -> dest=%d tag=%d comm=%" PRIu64
          " type=%s count=%d bytes=%" PRIu64 " at %s\n"
          "  recv: rank=%d <- src=%d tag=%d comm=%" PRIu64
          " type=%s count=%d bytes=%" PRIu64 " at %s\n",
          kind, detail, send_ev->rank, send_ev->peer, send_ev->tag, send_ev->comm_f,
          send_ev->type_name, send_ev->count, send_ev->nbytes, send_ev->loc,
          recv_ev->rank, recv_ev->peer, recv_ev->tag, recv_ev->comm_f,
          recv_ev->type_name, recv_ev->count, recv_ev->nbytes, recv_ev->loc);
}

static void analyze_events_root(MsanEvent *all, size_t total) {
  // Naive matching: for each recv, find first unmatched send with same key.
  uint8_t *send_used = (uint8_t *)calloc(total, 1);
  if (!send_used) {
    fprintf(stderr, "[msan] OOM during analysis\n");
    return;
  }

  for (size_t i = 0; i < total; ++i) {
    MsanEvent *r = &all[i];
    if (r->kind != MSAN_EV_RECV)
      continue;

    MsanEvent *matched = NULL;
    size_t matched_idx = 0;
    for (size_t j = 0; j < total; ++j) {
      MsanEvent *s = &all[j];
      if (s->kind != MSAN_EV_SEND)
        continue;
      if (send_used[j])
        continue;
      if (!ev_key_equal(s, r))
        continue;
      matched = s;
      matched_idx = j;
      break;
    }

    if (!matched) {
      fprintf(stderr,
              "[msan][unmatched-recv] recv has no matching send\n"
              "  recv: rank=%d <- src=%d tag=%d comm=%" PRIu64
              " type=%s count=%d bytes=%" PRIu64 " at %s\n",
              r->rank, r->peer, r->tag, r->comm_f, r->type_name, r->count, r->nbytes,
              r->loc);
      continue;
    }

    send_used[matched_idx] = 1;

    if (strcmp(matched->type_name, r->type_name) != 0) {
      report_mismatch(matched, r, "type-mismatch",
                      "MPI_Datatype name differs");
    }

    if (matched->nbytes != r->nbytes) {
      report_mismatch(matched, r, "size-mismatch",
                      "send bytes != recv buffer capacity");
    }
  }

  // Any remaining sends unmatched?
  for (size_t j = 0; j < total; ++j) {
    MsanEvent *s = &all[j];
    if (s->kind != MSAN_EV_SEND)
      continue;
    if (send_used[j])
      continue;

    fprintf(stderr,
            "[msan][unmatched-send] send has no matching recv\n"
            "  send: rank=%d -> dest=%d tag=%d comm=%" PRIu64
            " type=%s count=%d bytes=%" PRIu64 " at %s\n",
            s->rank, s->peer, s->tag, s->comm_f, s->type_name, s->count, s->nbytes,
            s->loc);
  }

  free(send_used);
}

void __msan_finalize(const char *file, int line) {
  (void)file;
  (void)line;

  msan_init_if_needed();

  int local_n = (int)g_events.len;
  int *counts_events = NULL;

  if (g_rank == 0) {
    counts_events = (int *)calloc((size_t)g_size, sizeof(int));
    if (!counts_events) {
      fprintf(stderr, "[msan] OOM in finalize\n");
      return;
    }
  }

  PMPI_Gather(&local_n, 1, MPI_INT, counts_events, 1, MPI_INT, 0,
              MPI_COMM_WORLD);

  MsanEvent *all = NULL;
  int *recvcounts_bytes = NULL;
  int *displs_bytes = NULL;
  int total_events = 0;

  if (g_rank == 0) {
    recvcounts_bytes = (int *)calloc((size_t)g_size, sizeof(int));
    displs_bytes = (int *)calloc((size_t)g_size, sizeof(int));
    if (!recvcounts_bytes || !displs_bytes) {
      fprintf(stderr, "[msan] OOM in finalize\n");
      free(recvcounts_bytes);
      free(displs_bytes);
      free(counts_events);
      return;
    }

    for (int i = 0; i < g_size; ++i) {
      displs_bytes[i] = total_events * (int)sizeof(MsanEvent);
      recvcounts_bytes[i] = counts_events[i] * (int)sizeof(MsanEvent);
      total_events += counts_events[i];
    }

    all = (MsanEvent *)calloc((size_t)total_events, sizeof(MsanEvent));
    if (!all) {
      fprintf(stderr, "[msan] OOM in finalize\n");
      free(recvcounts_bytes);
      free(displs_bytes);
      free(counts_events);
      return;
    }
  }

  // Gather events as bytes.
  PMPI_Gatherv(g_events.data,
               local_n * (int)sizeof(MsanEvent),
               MPI_BYTE,
               all,
               recvcounts_bytes,
               displs_bytes,
               MPI_BYTE,
               0,
               MPI_COMM_WORLD);

  if (g_rank == 0) {
    analyze_events_root(all, (size_t)total_events);
  }

  free(all);
  free(recvcounts_bytes);
  free(displs_bytes);
  free(counts_events);

  free(g_events.data);
  g_events.data = NULL;
  g_events.len = 0;
  g_events.cap = 0;
}

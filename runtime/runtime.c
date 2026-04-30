#include "msan_runtime.h"
#include "detector.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

  PMPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
  PMPI_Comm_size(MPI_COMM_WORLD, &g_size);
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

static uint32_t msan_checksum(const void *buf, uint64_t nbytes) {
  if (!buf || nbytes == 0) return 0;
  uint32_t hash = 2166136261u;
  const uint8_t *p = (const uint8_t *)buf;
  for (uint64_t i = 0; i < nbytes; i++) {
    hash ^= p[i];
    hash *= 16777619u;
  }
  return hash;
}

static void log_event(MsanEventKind kind, void *buf, int count, uint64_t datatype_handle,
                      int peer, int tag, uint64_t comm_handle, const char *file, int line) {
  msan_init_if_needed();
  MPI_Datatype datatype = msan_dt_from_handle(datatype_handle);
  MPI_Comm comm = msan_comm_from_handle(comm_handle);

  MsanEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.kind = (uint32_t)kind;
  ev.rank = g_rank;
  ev.peer = peer;
  ev.tag = tag;
  ev.count = count;
  ev.type_size = msan_type_size(datatype);
  ev.nbytes = (uint64_t)(count < 0 ? 0 : (uint64_t)count) * (uint64_t)ev.type_size;
  ev.comm_f = msan_comm_id(comm);
  ev.buf_addr = (uint64_t)(uintptr_t)buf;
  ev.seq = ++g_seq;
  ev.timestamp = PMPI_Wtime();
  
  if (kind == MSAN_EV_SEND || kind == MSAN_EV_RECV) {
    ev.checksum = msan_checksum(buf, ev.nbytes);
  }

  msan_set_typename(datatype, ev.type_name);
  msan_set_loc(file, line, ev.loc);

  msan_vec_push(ev);
}

void __msan_before_send(void *buf, int count, uint64_t datatype_handle, int dest,
                        int tag, uint64_t comm_handle, const char *file,
                        int line) {
  log_event(MSAN_EV_SEND, buf, count, datatype_handle, dest, tag, comm_handle, file, line);
}

void __msan_after_recv(void *buf, int count, uint64_t datatype_handle,
                       int source, int tag, uint64_t comm_handle, void *status,
                       const char *file, int line) {
  msan_init_if_needed();
  MPI_Status *st = (MPI_Status *)status;
  int actual_src = source;
  int actual_tag = tag;

  if (st && st != MPI_STATUS_IGNORE) {
    actual_src = st->MPI_SOURCE;
    actual_tag = st->MPI_TAG;
  }

  log_event(MSAN_EV_RECV, buf, count, datatype_handle, actual_src, actual_tag, comm_handle, file, line);
}

void __msan_before_collective(const char *name, void *sendbuf, void *recvbuf,
                              int count, uint64_t datatype_handle, int root,
                              uint64_t comm_handle, const char *file, int line) {
  // For collectives, we log both buffers if applicable. 
  // Here we just log the event with the name.
  log_event(MSAN_EV_COLLECTIVE, recvbuf ? recvbuf : sendbuf, count, datatype_handle, root, 0, comm_handle, file, line);
  
  // Update the last event with the collective name
  if (g_events.len > 0) {
    strncpy(g_events.data[g_events.len - 1].coll_name, name, 31);
  }
}

void __msan_finalize(const char *file, int line) {
  (void)file;
  (void)line;

  msan_init_if_needed();

  int local_n = (int)g_events.len;
  int *counts_events = NULL;

  if (g_rank == 0) {
    counts_events = (int *)calloc((size_t)g_size, sizeof(int));
  }

  PMPI_Gather(&local_n, 1, MPI_INT, counts_events, 1, MPI_INT, 0, MPI_COMM_WORLD);

  MsanEvent *all = NULL;
  int *recvcounts_bytes = NULL;
  int *displs_bytes = NULL;
  int total_events = 0;

  if (g_rank == 0) {
    recvcounts_bytes = (int *)calloc((size_t)g_size, sizeof(int));
    displs_bytes = (int *)calloc((size_t)g_size, sizeof(int));
    for (int i = 0; i < g_size; ++i) {
      displs_bytes[i] = total_events * (int)sizeof(MsanEvent);
      recvcounts_bytes[i] = counts_events[i] * (int)sizeof(MsanEvent);
      total_events += counts_events[i];
    }
    all = (MsanEvent *)calloc((size_t)total_events, sizeof(MsanEvent));
  }

  PMPI_Gatherv(g_events.data, local_n * (int)sizeof(MsanEvent), MPI_BYTE,
               all, recvcounts_bytes, displs_bytes, MPI_BYTE, 0, MPI_COMM_WORLD);

  if (g_rank == 0) {
    msan_analyze_events(all, (size_t)total_events, g_size);
    free(all);
    free(recvcounts_bytes);
    free(displs_bytes);
    free(counts_events);
  }

  free(g_events.data);
  g_events.data = NULL;
  g_events.len = 0;
  g_events.cap = 0;
}

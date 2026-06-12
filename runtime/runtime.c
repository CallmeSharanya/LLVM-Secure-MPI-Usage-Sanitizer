#include "msan_runtime.h"
#include "detector.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define SECURE_OVERHEAD 36
static const unsigned char psk[32] = "01234567890123456789012345678901"; // 32-byte key
static uint64_t seq_counters[1024][1024] = {0}; // simple 2D array for sequence numbers

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

void __msan_secure_send(void *buf, int count, uint64_t datatype_handle, int dest,
                        int tag, uint64_t comm_handle, const char *file,
                        int line) {
  msan_init_if_needed();
  MPI_Datatype datatype = msan_dt_from_handle(datatype_handle);
  MPI_Comm comm = msan_comm_from_handle(comm_handle);

  log_event(MSAN_EV_SEND, buf, count, datatype_handle, dest, tag, comm_handle, file, line);

  int type_size;
  PMPI_Type_size(datatype, &type_size);
  int payload_len = count * type_size;

  unsigned char *wire_buf = (unsigned char *)malloc(payload_len + SECURE_OVERHEAD);
  if (!wire_buf) PMPI_Abort(comm, 1);

  uint64_t seq = ++seq_counters[g_rank][dest];
  memcpy(wire_buf, &seq, 8);
  unsigned char iv[12];
  RAND_bytes(iv, 12);
  memcpy(wire_buf + 8, iv, 12);

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int outlen;
  EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, psk, iv);
  EVP_EncryptUpdate(ctx, wire_buf + 20, &outlen, (const unsigned char *)buf, payload_len);
  int ciphertext_len = outlen;
  EVP_EncryptFinal_ex(ctx, wire_buf + 20 + outlen, &outlen);
  ciphertext_len += outlen;
  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, wire_buf + 20 + ciphertext_len);
  EVP_CIPHER_CTX_free(ctx);

  PMPI_Send(wire_buf, ciphertext_len + 36, MPI_BYTE, dest, tag, comm);
  free(wire_buf);
}

void __msan_secure_recv(void *buf, int count, uint64_t datatype_handle,
                        int source, int tag, uint64_t comm_handle, void *status,
                        const char *file, int line) {
  msan_init_if_needed();
  MPI_Datatype datatype = msan_dt_from_handle(datatype_handle);
  MPI_Comm comm = msan_comm_from_handle(comm_handle);

  MPI_Status probe_st;
  PMPI_Probe(source, tag, comm, &probe_st);
  int wire_len;
  PMPI_Get_count(&probe_st, MPI_BYTE, &wire_len);

  unsigned char *wire_buf = (unsigned char *)malloc(wire_len);
  if (!wire_buf) PMPI_Abort(comm, 1);

  MPI_Status recv_st;
  PMPI_Recv(wire_buf, wire_len, MPI_BYTE, probe_st.MPI_SOURCE, probe_st.MPI_TAG, comm, &recv_st);

  int actual_src = recv_st.MPI_SOURCE;
  int actual_tag = recv_st.MPI_TAG;

  uint64_t seq;
  memcpy(&seq, wire_buf, 8);
  unsigned char iv[12];
  memcpy(iv, wire_buf + 8, 12);
  unsigned char recv_tag[16];
  memcpy(recv_tag, wire_buf + wire_len - 16, 16);

  if (seq <= seq_counters[actual_src][g_rank]) {
      fprintf(stderr, "[msan][SECURITY-ERROR] Replay attack detected from rank %d (seq %" PRIu64 " <= expected %" PRIu64 ")!\n", actual_src, seq, seq_counters[actual_src][g_rank]);
      PMPI_Abort(comm, 1);
  }
  seq_counters[actual_src][g_rank] = seq;

  int payload_len = wire_len - 36;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int outlen;
  EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, psk, iv);
  EVP_DecryptUpdate(ctx, (unsigned char *)buf, &outlen, wire_buf + 20, payload_len);
  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, recv_tag);
  int ret = EVP_DecryptFinal_ex(ctx, ((unsigned char *)buf) + outlen, &outlen);
  EVP_CIPHER_CTX_free(ctx);

  if (ret <= 0) {
      fprintf(stderr, "[msan][SECURITY-ERROR] Cryptographic authentication failed for message from rank %d!\n", actual_src);
      PMPI_Abort(comm, 1);
  }

  free(wire_buf);
  if (status && status != MPI_STATUS_IGNORE) {
      memcpy(status, &recv_st, sizeof(MPI_Status));
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

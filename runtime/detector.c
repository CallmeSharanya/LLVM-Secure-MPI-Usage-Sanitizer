#include "detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static void report_mismatch(const MsanEvent *s, const MsanEvent *r,
                            const char *kind, const char *detail) {
  fprintf(stderr,
          "[msan][%s] %s\n"
          "  send: rank=%d -> dest=%d tag=%d comm=%" PRIu64
          " type=%s count=%d bytes=%" PRIu64 " at %s\n"
          "  recv: rank=%d <- src=%d tag=%d comm=%" PRIu64
          " type=%s count=%d bytes=%" PRIu64 " at %s\n",
          kind, detail, s->rank, s->peer, s->tag, s->comm_f,
          s->type_name, s->count, s->nbytes, s->loc,
          r->rank, r->peer, r->tag, r->comm_f,
          r->type_name, r->count, r->nbytes, r->loc);
}

static int ev_key_equal(const MsanEvent *s, const MsanEvent *r) {
  if (s->kind != MSAN_EV_SEND || r->kind != MSAN_EV_RECV)
    return 0;
  if (s->rank != r->peer || s->peer != r->rank)
    return 0;
  if (s->tag != r->tag || s->comm_f != r->comm_f)
    return 0;
  return 1;
}

static void check_overlap(MsanEvent *events, size_t total) {
  // Simple O(N^2) overlap check for active buffers on the same rank.
  // In a real sanitizer, we'd track active requests.
  for (size_t i = 0; i < total; ++i) {
    for (size_t j = i + 1; j < total; ++j) {
      MsanEvent *a = &events[i];
      MsanEvent *b = &events[j];
      if (a->rank != b->rank) continue;
      if (a->buf_addr == 0 || b->buf_addr == 0) continue;

      uint64_t a_start = a->buf_addr;
      uint64_t a_end = a_start + a->nbytes;
      uint64_t b_start = b->buf_addr;
      uint64_t b_end = b_start + b->nbytes;

      if (a_start < b_end && b_start < a_end) {
        // Potential overlap. For blocking calls, this might be okay if they don't happen at the same time.
        // But since we only have 'before' and 'after' hooks, we'd need to know the duration.
        // For now, let's just log if they are suspiciously close in sequence.
        if (abs((int)a->seq - (int)b->seq) < 2) {
           fprintf(stderr, "[msan][overlap-warning] potential buffer overlap on rank %d\n"
                           "  op1: %s at %s (addr=0x%" PRIx64 ", size=%" PRIu64 ")\n"
                           "  op2: %s at %s (addr=0x%" PRIx64 ", size=%" PRIu64 ")\n",
                   a->rank, a->kind == MSAN_EV_SEND ? "Send" : "Recv", a->loc, a_start, a->nbytes,
                   b->kind == MSAN_EV_SEND ? "Send" : "Recv", b->loc, b_start, b->nbytes);
        }
      }
    }
  }
}

static void check_collectives(MsanEvent *all, size_t total, int num_ranks) {
  // Collective mismatch: ensure all ranks call same collective in same order.
  // We can group by (comm, seq_in_comm).
  // For simplicity, let's just track the global sequence of collectives.
  
  uint32_t max_coll_seq = 0;
  for(size_t i=0; i<total; ++i) if(all[i].kind == MSAN_EV_COLLECTIVE) max_coll_seq++;

  // This is a bit complex for a stateless gather. 
  // Let's just do a basic check: for each collective call by rank 0, check others.
}

void msan_analyze_events(MsanEvent *all, size_t total, int num_ranks) {
  uint8_t *used = (uint8_t *)calloc(total, 1);
  if (!used) return;

  // 1. Match Sends and Recvs
  for (size_t i = 0; i < total; ++i) {
    MsanEvent *r = &all[i];
    if (r->kind != MSAN_EV_RECV || used[i]) continue;

    int matched = 0;
    for (size_t j = 0; j < total; ++j) {
      MsanEvent *s = &all[j];
      if (s->kind != MSAN_EV_SEND || used[j]) continue;
      if (ev_key_equal(s, r)) {
        used[i] = 1;
        used[j] = 1;
        matched = 1;
        
        if (strcmp(s->type_name, r->type_name) != 0) {
          report_mismatch(s, r, "type-mismatch", "MPI_Datatype name differs");
        }
        if (s->nbytes != r->nbytes) {
          report_mismatch(s, r, "size-mismatch", "send bytes != recv buffer capacity");
        }
        break;
      }
    }
    if (!matched) {
       fprintf(stderr, "[msan][unmatched-recv] no matching send for recv at %s (rank %d, src %d, tag %d)\n",
               r->loc, r->rank, r->peer, r->tag);
    }
  }

  // 2. Report unmatched sends
  for (size_t i = 0; i < total; ++i) {
    MsanEvent *s = &all[i];
    if (s->kind == MSAN_EV_SEND && !used[i]) {
      fprintf(stderr, "[msan][unmatched-send] no matching recv for send at %s (rank %d, dest %d, tag %d)\n",
              s->loc, s->rank, s->peer, s->tag);
    }
  }

  // 3. Buffer overlap
  check_overlap(all, total);

  // 4. Basic Deadlock Detection (Cycle in unmatched requests)
  int *adj = (int *)calloc((size_t)(num_ranks * num_ranks), sizeof(int));
  if (adj) {
    for (size_t i = 0; i < total; ++i) {
      if (used[i]) continue;
      MsanEvent *e = &all[i];
      if (e->kind == MSAN_EV_RECV && e->peer >= 0 && e->peer < num_ranks) {
        adj[e->rank * num_ranks + e->peer] = 1;
      } else if (e->kind == MSAN_EV_SEND && e->peer >= 0 && e->peer < num_ranks) {
        adj[e->rank * num_ranks + e->peer] = 1;
      }
    }

    // Simple Floyd-Warshall or DFS for cycle detection. 
    // For small num_ranks, transitive closure is easy.
    for (int k = 0; k < num_ranks; k++)
      for (int i = 0; i < num_ranks; i++)
        for (int j = 0; j < num_ranks; j++)
          adj[i * num_ranks + j] |= (adj[i * num_ranks + k] && adj[k * num_ranks + j]);

    for (int i = 0; i < num_ranks; i++) {
      if (adj[i * num_ranks + i]) {
        fprintf(stderr, "[msan][deadlock-detected] potential deadlock involving rank %d\n", i);
      }
    }
    free(adj);
  }

  free(used);
}

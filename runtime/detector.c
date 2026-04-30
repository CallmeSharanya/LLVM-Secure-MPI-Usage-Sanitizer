#include "detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

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
  size_t *offsets = (size_t *)calloc((size_t)num_ranks, sizeof(size_t));
  size_t *counts = (size_t *)calloc((size_t)num_ranks, sizeof(size_t));
  if (!offsets || !counts) {
    if (offsets) free(offsets);
    if (counts) free(counts);
    return;
  }

  // Identify range of events for each rank in the 'all' array
  int current_rank = -1;
  for (size_t i = 0; i < total; ++i) {
    if (all[i].rank != current_rank) {
      current_rank = all[i].rank;
      if (current_rank >= 0 && current_rank < num_ranks) {
        offsets[current_rank] = i;
      }
    }
    if (current_rank >= 0 && current_rank < num_ranks) {
      counts[current_rank]++;
    }
  }

  // Find max collectives per rank
  size_t max_colls = 0;
  size_t *num_colls = (size_t *)calloc((size_t)num_ranks, sizeof(size_t));
  if (!num_colls) {
    free(offsets); free(counts);
    return;
  }

  for (int r = 0; r < num_ranks; r++) {
    for (size_t i = 0; i < counts[r]; i++) {
      if (all[offsets[r] + i].kind == MSAN_EV_COLLECTIVE) {
        num_colls[r]++;
      }
    }
    if (num_colls[r] > max_colls) max_colls = num_colls[r];
  }

  if (max_colls == 0) {
    free(offsets); free(counts); free(num_colls);
    return;
  }

  // Compare collective calls rank-by-rank for each sequence index
  for (size_t i = 0; i < max_colls; i++) {
    MsanEvent *ref = NULL;
    int ref_rank = -1;

    for (int r = 0; r < num_ranks; r++) {
      // Find i-th collective for rank r
      MsanEvent *curr = NULL;
      size_t c_idx = 0;
      for (size_t j = 0; j < counts[r]; j++) {
        if (all[offsets[r] + j].kind == MSAN_EV_COLLECTIVE) {
          if (c_idx == i) {
            curr = &all[offsets[r] + j];
            break;
          }
          c_idx++;
        }
      }

      if (!ref && curr) {
        ref = curr;
        ref_rank = r;
        continue;
      }

      if (ref && !curr) {
        fprintf(stderr, "[msan][collective-mismatch] rank %d missing collective call #%zu\n"
                        "  reference: %s at %s (rank %d)\n",
                r, (size_t)(i + 1), ref->coll_name, ref->loc, ref_rank);
      } else if (ref && curr) {
        if (strcmp(ref->coll_name, curr->coll_name) != 0 || ref->comm_f != curr->comm_f || ref->peer != curr->peer) {
          fprintf(stderr, "[msan][collective-mismatch] collective call #%zu mismatch\n"
                          "  rank %d: %s, comm=%" PRIu64 ", root=%d at %s\n"
                          "  rank %d: %s, comm=%" PRIu64 ", root=%d at %s\n",
                  (size_t)(i + 1), ref_rank, ref->coll_name, ref->comm_f, ref->peer, ref->loc,
                  r, curr->coll_name, curr->comm_f, curr->peer, curr->loc);
        }
      }
    }
  }

  free(offsets); free(counts); free(num_colls);
}

static void msan_generate_dot_graph(MsanEvent *all, size_t total) {
  FILE *f = fopen("msan_comm_graph.dot", "w");
  if (!f) return;
  fprintf(f, "digraph MPIComm {\n");
  fprintf(f, "  rankdir=LR;\n");
  fprintf(f, "  node [shape=circle];\n");

  uint8_t *matrix = calloc(1024 * 1024, 1); // Simple bitset for 1024 ranks
  for (size_t i = 0; i < total; i++) {
    if (all[i].kind == MSAN_EV_SEND) {
      int src = all[i].rank;
      int dest = all[i].peer;
      if (src < 1024 && dest < 1024 && !matrix[src * 1024 + dest]) {
        fprintf(f, "  %d -> %d [label=\"tag=%d\"];\n", src, dest, all[i].tag);
        matrix[src * 1024 + dest] = 1;
      }
    }
  }
  fprintf(f, "}\n");
  fclose(f);
  free(matrix);
  fprintf(stderr, "[msan][info] Communication graph generated: msan_comm_graph.dot\n");
}

void msan_analyze_events(MsanEvent *all, size_t total, int num_ranks) {
  uint8_t *used = (uint8_t *)calloc(total, 1);
  if (!used) return;

  double total_lat = 0;
  int lat_count = 0;
  double sum_size = 0;
  double sum_size_sq = 0;
  int send_count = 0;

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
        
        // Latency
        if (r->timestamp > s->timestamp) {
          total_lat += (r->timestamp - s->timestamp);
          lat_count++;
        }

        // Integrity
        if (s->checksum != r->checksum) {
          report_mismatch(s, r, "integrity-violation", "Checksum mismatch: data corruption suspected");
        }

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
       fprintf(stderr, "[msan][unmatched-recv] no matching send for recv at %s (rank %d, src %d, tag %d). Potential Timeout/Deadlock.\n",
               r->loc, r->rank, r->peer, r->tag);
    }
  }

  // Stats for Anomaly Detection
  for (size_t i = 0; i < total; i++) {
    if (all[i].kind == MSAN_EV_SEND) {
      double sz = (double)all[i].nbytes;
      sum_size += sz;
      sum_size_sq += sz * sz;
      send_count++;
    }
  }

  if (send_count > 1) {
    double mean = sum_size / send_count;
    double var = (sum_size_sq / send_count) - (mean * mean);
    double stddev = sqrt(var > 0 ? var : 0);
    
    for (size_t i = 0; i < total; i++) {
      if (all[i].kind == MSAN_EV_SEND && stddev > 0) {
        double sz = (double)all[i].nbytes;
        if (fabs(sz - mean) > 3 * stddev) {
          fprintf(stderr, "[msan][anomaly-warning] unusual message size detected at %s: %" PRIu64 " bytes (mean=%.1f, stddev=%.1f)\n",
                  all[i].loc, all[i].nbytes, mean, stddev);
        }
      }
    }
  }

  if (lat_count > 0) {
    fprintf(stderr, "[msan][info] Average P2P latency: %.6f seconds\n", total_lat / lat_count);
  }

  // 2. Report unmatched sends
  for (size_t i = 0; i < total; ++i) {
    MsanEvent *s = &all[i];
    if (s->kind == MSAN_EV_SEND && !used[i]) {
      fprintf(stderr, "[msan][unmatched-send] no matching recv for send at %s (rank %d, dest %d, tag %d). Potential Deadlock.\n",
              s->loc, s->rank, s->peer, s->tag);
    }
  }

  // 3. Buffer overlap
  check_overlap(all, total);

  // 4. Collective mismatch
  check_collectives(all, total, num_ranks);

  // 5. Graph generation
  msan_generate_dot_graph(all, total);

  // 6. Basic Deadlock Detection (Cycle in unmatched requests)
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

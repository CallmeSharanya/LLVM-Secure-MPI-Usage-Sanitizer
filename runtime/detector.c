#include "detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#define MSAN_TIMEOUT_THRESHOLD 5.0

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

static void check_overlap(MsanEvent *events, size_t total, int *overlap_count) {
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
           (*overlap_count)++;
        }
      }
    }
  }
}

static void check_collectives(MsanEvent *all, size_t total, int num_ranks, int *coll_mismatch_count) {
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
        (*coll_mismatch_count)++;
      } else if (ref && curr) {
        if (strcmp(ref->coll_name, curr->coll_name) != 0 || ref->comm_f != curr->comm_f || ref->peer != curr->peer) {
          fprintf(stderr, "[msan][collective-mismatch] collective call #%zu mismatch\n"
                          "  rank %d: %s, comm=%" PRIu64 ", root=%d at %s\n"
                          "  rank %d: %s, comm=%" PRIu64 ", root=%d at %s\n",
                  (size_t)(i + 1), ref_rank, ref->coll_name, ref->comm_f, ref->peer, ref->loc,
                  r, curr->coll_name, curr->comm_f, curr->peer, curr->loc);
          (*coll_mismatch_count)++;
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

static void msan_write_json_report(size_t total_events,
                                   int send_count,
                                   int recv_count,
                                   int coll_count,
                                   int matched_pairs,
                                   int unmatched_send_count,
                                   int unmatched_recv_count,
                                   int total_errors,
                                   int type_mismatch_count,
                                   int size_mismatch_count,
                                   int integrity_count,
                                   int coll_mismatch_count,
                                   int deadlock_count,
                                   int replay_count,
                                   int timeout_count,
                                   int anomaly_count,
                                   int overlap_count,
                                   int lat_count,
                                   double avg_latency) {
  FILE *f = fopen("mpi_report.json", "w");
  if (!f) {
    fprintf(stderr, "[msan][warning] failed to write mpi_report.json\n");
    return;
  }

  fprintf(f, "{\n");
  fprintf(f, "  \"summary\": {\n");
  fprintf(f, "    \"total_events\": %zu,\n", total_events);
  fprintf(f, "    \"sends\": %d,\n", send_count);
  fprintf(f, "    \"recvs\": %d,\n", recv_count);
  fprintf(f, "    \"collectives\": %d,\n", coll_count);
  fprintf(f, "    \"matched_pairs\": %d,\n", matched_pairs);
  fprintf(f, "    \"unmatched_sends\": %d,\n", unmatched_send_count);
  fprintf(f, "    \"unmatched_recvs\": %d,\n", unmatched_recv_count);
  fprintf(f, "    \"errors_detected\": %d,\n", total_errors);
  fprintf(f, "    \"type_mismatch\": %d,\n", type_mismatch_count);
  fprintf(f, "    \"size_mismatch\": %d,\n", size_mismatch_count);
  fprintf(f, "    \"integrity_violation\": %d,\n", integrity_count);
  fprintf(f, "    \"collective_mismatch\": %d,\n", coll_mismatch_count);
  fprintf(f, "    \"deadlock_detected\": %d,\n", deadlock_count);
  fprintf(f, "    \"replay_detected\": %d,\n", replay_count);
  fprintf(f, "    \"timeout_warning\": %d,\n", timeout_count);
  fprintf(f, "    \"anomaly_warning\": %d,\n", anomaly_count);
  fprintf(f, "    \"overlap_warning\": %d,\n", overlap_count);
  if (lat_count > 0) {
    fprintf(f, "    \"avg_p2p_latency\": %.9f,\n", avg_latency);
  } else {
    fprintf(f, "    \"avg_p2p_latency\": null,\n");
  }
  fprintf(f, "    \"comm_graph\": \"msan_comm_graph.dot\"\n");
  fprintf(f, "  },\n");
  fprintf(f, "  \"errors\": []\n");
  fprintf(f, "}\n");

  fclose(f);
}

void msan_analyze_events(MsanEvent *all, size_t total, int num_ranks) {
  uint8_t *used = (uint8_t *)calloc(total, 1);
  if (!used) return;

  double total_lat = 0;
  int lat_count = 0;
  double sum_size = 0;
  double sum_size_sq = 0;
  int send_count = 0;

  /* Error counters for summary report */
  int type_mismatch_count = 0;
  int size_mismatch_count = 0;
  int integrity_count = 0;
  int coll_mismatch_count = 0;
  int deadlock_count = 0;
  int replay_count = 0;
  int timeout_count = 0;
  int anomaly_count = 0;
  int overlap_count = 0;
  int matched_pairs = 0;
  int unmatched_send_count = 0;
  int unmatched_recv_count = 0;
  int recv_count = 0;
  int coll_count = 0;

  /* Count event types */
  for (size_t i = 0; i < total; i++) {
    if (all[i].kind == MSAN_EV_RECV) recv_count++;
    else if (all[i].kind == MSAN_EV_COLLECTIVE) coll_count++;
  }

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
        matched_pairs++;
        
        // Latency
        if (r->timestamp > s->timestamp) {
          total_lat += (r->timestamp - s->timestamp);
          lat_count++;
        }

        // Integrity
        if (s->checksum != r->checksum) {
          report_mismatch(s, r, "integrity-violation", "Checksum mismatch: data corruption suspected");
          integrity_count++;
        }

        if (strcmp(s->type_name, r->type_name) != 0) {
          report_mismatch(s, r, "type-mismatch", "MPI_Datatype name differs");
          type_mismatch_count++;
        }
        if (s->nbytes != r->nbytes) {
          report_mismatch(s, r, "size-mismatch", "send bytes != recv buffer capacity");
          size_mismatch_count++;
        }
        break;
      }
    }
    if (!matched) {
       fprintf(stderr, "[msan][unmatched-recv] no matching send for recv at %s (rank %d, src %d, tag %d). Potential Timeout/Deadlock.\n",
               r->loc, r->rank, r->peer, r->tag);
       unmatched_recv_count++;
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
          anomaly_count++;
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
      unmatched_send_count++;
    }
  }

  // 2b. Replay / Duplicate Message Detection
  // Look at all MATCHED send events for identical (rank, peer, tag, comm_f, checksum)
  {
    uint8_t *reported = (uint8_t *)calloc(total, 1);
    if (reported) {
      for (size_t i = 0; i < total; ++i) {
        if (all[i].kind != MSAN_EV_SEND || !used[i] || reported[i]) continue;
        if (all[i].checksum == 0) continue;

        for (size_t j = i + 1; j < total; ++j) {
          if (all[j].kind != MSAN_EV_SEND || !used[j] || reported[j]) continue;
          if (all[j].checksum == 0) continue;

          if (all[i].rank == all[j].rank &&
              all[i].peer == all[j].peer &&
              all[i].tag == all[j].tag &&
              all[i].comm_f == all[j].comm_f &&
              all[i].checksum == all[j].checksum) {
            fprintf(stderr, "[msan][replay-detected] duplicate message detected:\n"
                            "  send #1: rank=%d -> dest=%d tag=%d comm=%" PRIu64 " checksum=0x%08X at %s\n"
                            "  send #2: rank=%d -> dest=%d tag=%d comm=%" PRIu64 " checksum=0x%08X at %s\n"
                            "  Possible replay attack or unintended message duplication.\n",
                    all[i].rank, all[i].peer, all[i].tag, all[i].comm_f, all[i].checksum, all[i].loc,
                    all[j].rank, all[j].peer, all[j].tag, all[j].comm_f, all[j].checksum, all[j].loc);
            reported[i] = 1;
            reported[j] = 1;
            replay_count++;
          }
        }
      }
      free(reported);
    }
  }

  // 2c. Timeout-Based Stall Detection
  // For every unmatched recv, check if the wait time exceeds MSAN_TIMEOUT_THRESHOLD
  for (size_t i = 0; i < total; ++i) {
    if (all[i].kind != MSAN_EV_RECV || used[i]) continue;

    // Find the earliest send timestamp among all sends where send.rank == recv.peer
    // (i.e., sends originating from the rank the recv is expecting a message from)
    double earliest_send_ts = -1.0;
    int found_send = 0;
    for (size_t j = 0; j < total; ++j) {
      if (all[j].kind == MSAN_EV_SEND && all[j].rank == all[i].peer) {
        if (!found_send || all[j].timestamp < earliest_send_ts) {
          earliest_send_ts = all[j].timestamp;
          found_send = 1;
        }
      }
    }

    if (found_send && (all[i].timestamp - earliest_send_ts) > MSAN_TIMEOUT_THRESHOLD) {
      fprintf(stderr, "[msan][timeout-warning] rank=%d waited >%.1fs for message from rank=%d tag=%d — possible stall or node failure at %s\n",
              all[i].rank, MSAN_TIMEOUT_THRESHOLD, all[i].peer, all[i].tag, all[i].loc);
      timeout_count++;
    }
  }

  // 3. Buffer overlap
  check_overlap(all, total, &overlap_count);

  // 4. Collective mismatch
  check_collectives(all, total, num_ranks, &coll_mismatch_count);

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
        deadlock_count++;
      }
    }
    free(adj);
  }

  // 7. Summary Report
  {
    int total_errors = type_mismatch_count + size_mismatch_count +
                       integrity_count + coll_mismatch_count +
                       deadlock_count + replay_count + timeout_count +
                       anomaly_count + overlap_count;
    double avg_latency = lat_count > 0 ? total_lat / lat_count : 0.0;

    fprintf(stderr, "[msan][summary] ============================================\n");
    fprintf(stderr, "[msan][summary] MPI Sanitizer — Analysis Complete\n");
    fprintf(stderr, "[msan][summary] Total events analyzed : %zu\n", total);
    fprintf(stderr, "[msan][summary] Sends                 : %d\n", send_count);
    fprintf(stderr, "[msan][summary] Recvs                 : %d\n", recv_count);
    fprintf(stderr, "[msan][summary] Collectives           : %d\n", coll_count);
    fprintf(stderr, "[msan][summary] Matched pairs         : %d\n", matched_pairs);
    fprintf(stderr, "[msan][summary] Unmatched sends       : %d\n", unmatched_send_count);
    fprintf(stderr, "[msan][summary] Unmatched recvs       : %d\n", unmatched_recv_count);
    fprintf(stderr, "[msan][summary] Errors detected       : %d\n", total_errors);
    fprintf(stderr, "[msan][summary]   type-mismatch       : %d\n", type_mismatch_count);
    fprintf(stderr, "[msan][summary]   size-mismatch       : %d\n", size_mismatch_count);
    fprintf(stderr, "[msan][summary]   integrity-violation : %d\n", integrity_count);
    fprintf(stderr, "[msan][summary]   collective-mismatch : %d\n", coll_mismatch_count);
    fprintf(stderr, "[msan][summary]   deadlock-detected   : %d\n", deadlock_count);
    fprintf(stderr, "[msan][summary]   replay-detected     : %d\n", replay_count);
    fprintf(stderr, "[msan][summary]   timeout-warning     : %d\n", timeout_count);
    fprintf(stderr, "[msan][summary]   anomaly-warning     : %d\n", anomaly_count);
    fprintf(stderr, "[msan][summary]   overlap-warning     : %d\n", overlap_count);
    if (lat_count > 0) {
      fprintf(stderr, "[msan][summary] Avg P2P latency       : %.6fs\n", avg_latency);
    } else {
      fprintf(stderr, "[msan][summary] Avg P2P latency       : N/A\n");
    }
    fprintf(stderr, "[msan][summary] Comm graph written to : msan_comm_graph.dot\n");
    fprintf(stderr, "[msan][summary] ============================================\n");

    msan_write_json_report(total, send_count, recv_count, coll_count, matched_pairs,
                           unmatched_send_count, unmatched_recv_count, total_errors,
                           type_mismatch_count, size_mismatch_count, integrity_count,
                           coll_mismatch_count, deadlock_count, replay_count,
                           timeout_count, anomaly_count, overlap_count, lat_count,
                           avg_latency);
  }

  free(used);
}

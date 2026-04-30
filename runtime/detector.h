#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef MSAN_MAX_LOC
#define MSAN_MAX_LOC 256
#endif

#ifndef MSAN_MAX_TYPENAME
#define MSAN_MAX_TYPENAME 64
#endif

typedef enum {
  MSAN_EV_SEND = 1,
  MSAN_EV_RECV = 2,
  MSAN_EV_COLLECTIVE = 3,
} MsanEventKind;

typedef struct {
  uint32_t kind;      // MsanEventKind
  int32_t rank;
  int32_t peer;      // dest for send, source for recv, root for some collectives
  int32_t tag;
  int32_t count;
  int32_t type_size;
  uint64_t nbytes;
  uint64_t comm_f;   // MPI_Comm as Fortran handle
  uint64_t buf_addr; // raw pointer value
  uint64_t seq;
  double timestamp;
  uint32_t checksum;
  char type_name[MSAN_MAX_TYPENAME];
  char loc[MSAN_MAX_LOC];
  char coll_name[32]; // e.g. "Bcast", "Reduce"
} MsanEvent;

void msan_analyze_events(MsanEvent *all, size_t total, int num_ranks);

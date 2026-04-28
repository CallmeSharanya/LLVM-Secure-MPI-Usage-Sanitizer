#pragma once

#include <mpi.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void __msan_init(const char *file, int line);

void __msan_before_send(void *buf, int count, uint64_t datatype_handle,
                        int dest, int tag, uint64_t comm_handle,
                        const char *file, int line);

void __msan_after_recv(void *buf, int count, uint64_t datatype_handle,
                       int source, int tag, uint64_t comm_handle, void *status,
                       const char *file, int line);

void __msan_finalize(const char *file, int line);

#ifdef __cplusplus
}
#endif

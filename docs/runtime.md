# MSan Runtime Hook Contract

This document defines the contract between the LLVM instrumentation pass and the MSan runtime library.

## Overview

The MSan runtime tracks MPI events (Send/Recv) and validates them during `MPI_Finalize` to detect mismatches in data types or message sizes. The instrumentation pass is responsible for inserting calls to these hooks at appropriate locations in the user code.

## Runtime Hooks

### `void __msan_init(const char *file, int line)`

*   **When to call**: Immediately after `MPI_Init` or `MPI_Init_thread`.
*   **Purpose**: Initializes the sanitizer runtime, captures the current rank and communicator size, and sets up error handlers to ensure the sanitizer can report findings even if MPI errors occur.
*   **Parameters**:
    *   `file`: Source file path (e.g., `__FILE__`).
    *   `line`: Source line number (e.g., `__LINE__`).

### `void __msan_before_send(void *buf, int count, uint64_t datatype_handle, int dest, int tag, uint64_t comm_handle, const char *file, int line)`

*   **When to call**: Immediately before a call to `MPI_Send`.
*   **Purpose**: Records a send event for later matching.
*   **Parameters**:
    *   `buf`: The data buffer pointer.
    *   `count`: Number of elements being sent.
    *   `datatype_handle`: The `MPI_Datatype` handle, cast to `uint64_t`.
    *   `dest`: Destination rank.
    *   `tag`: Message tag.
    *   `comm_handle`: The `MPI_Comm` handle, cast to `uint64_t`.
    *   `file`/`line`: Location of the `MPI_Send` call.

### `void __msan_after_recv(void *buf, int count, uint64_t datatype_handle, int source, int tag, uint64_t comm_handle, void *status, const char *file, int line)`

*   **When to call**: Immediately after a call to `MPI_Recv`.
*   **Purpose**: Records a receive event. If wildcards (`MPI_ANY_SOURCE`, `MPI_ANY_TAG`) were used, the `status` object is used to determine the actual peer and tag.
*   **Parameters**:
    *   `buf`: The data buffer pointer.
    *   `count`: The expected number of elements (buffer capacity).
    *   `datatype_handle`: The `MPI_Datatype` handle, cast to `uint64_t`.
    *   `source`: Source rank (may be `MPI_ANY_SOURCE`).
    *   `tag`: Message tag (may be `MPI_ANY_TAG`).
    *   `comm_handle`: The `MPI_Comm` handle, cast to `uint64_t`.
    *   `status`: Pointer to the `MPI_Status` object updated by the receive.
    *   `file`/`line`: Location of the `MPI_Recv` call.

### `void __msan_finalize(const char *file, int line)`

*   **When to call**: Immediately before `MPI_Finalize`.
*   **Purpose**: Triggers global analysis. Rank 0 gathers events from all ranks and checks for unmatched sends/receives or mismatches in types/sizes.
*   **Parameters**:
    *   `file`/`line`: Location of the `MPI_Finalize` call.

## Implementation Requirements

### Location Information
The instrumentation pass must provide the source file and line number for every hook.
- **Type**: `const char*` for file, `int` for line.
- **LLVM**: Extracted from `DILocation` metadata attached to the MPI call instruction.

### Handle Conversion
MPI handles (`MPI_Datatype`, `MPI_Comm`) must be passed as `uint64_t`.
- **LLVM**:
    - If the handle is a pointer (e.g., in some MPI implementations), it should be cast to an integer using `ptrtoint`.
    - If it is an integer handle, it should be zero-extended to 64 bits.

### Pointers
Buffer pointers (`buf`) and status pointers (`status`) must be passed as raw pointers (`void*`).
- **LLVM**: Cast to `i8*` using `bitcast`.

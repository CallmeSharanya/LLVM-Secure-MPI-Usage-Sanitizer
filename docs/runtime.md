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

### `void __msan_before_collective(const char *name, void *sendbuf, void *recvbuf, int count, uint64_t datatype_handle, int root, uint64_t comm_handle, const char *file, int line)`

*   **When to call**: Immediately before a collective call (e.g., `MPI_Bcast`, `MPI_Barrier`, `MPI_Reduce`).
*   **Purpose**: Records a collective event. Validates that all ranks participate in the same collective sequence and use consistent arguments (e.g., matching root).
*   **Parameters**:
    *   `name`: The name of the collective operation (e.g., `"Bcast"`).
    *   `sendbuf`/`recvbuf`: Pointers to the send and receive buffers.
    *   `count`: Element count.
    *   `datatype_handle`: The `MPI_Datatype` handle.
    *   `root`: The root rank of the collective (or `-1` if not applicable).
    *   `comm_handle`: The `MPI_Comm` handle.
    *   `file`/`line`: Location of the collective call.

### `void __msan_finalize(const char *file, int line)`

*   **When to call**: Immediately before `MPI_Finalize`.
*   **Purpose**: Triggers global analysis. Rank 0 gathers events from all ranks and performs verification of P2P calls, collectives, and advanced analytics.

## Advanced Analytics & Error Detection

The sanitizer implements several advanced checks during the analysis phase:

### 1. Message Integrity (Checksums)
The runtime computes a fast **FNV-1a checksum** of the data buffer before a send and after a receive. The analyzer compares these checksums to detect silent data corruption during transit.

### 2. Latency Profiling
Every event is timestamped using `MPI_Wtime()`. The analyzer computes the duration between a matching `MPI_Send` and `MPI_Recv` to report the average point-to-point latency.

### 3. Communication Graph
The analyzer generates a **Graphviz DOT file** (`msan_comm_graph.dot`) representing the communication topology of the application, labeling edges with message tags.

### 4. Anomaly Detection
The analyzer calculates the statistical mean and standard deviation of message sizes. Any message size that deviates by more than **3 standard deviations (3σ)** is flagged as an anomaly.

### 5. Collective Mismatch Detection
Ensures that all ranks in a communicator call the same collectives in the same order. It also verifies that parameters like the `root` rank are consistent across all participants.

### 6. Deadlock Detection
Builds a "Wait-For" graph based on unmatched sends and receives. If a cycle is found in the graph, a potential deadlock is reported involving the specific ranks.

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

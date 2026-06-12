# MPI Sanitizer with LLVM and VS Code Integration

## Overview

This project presents a **Secure** LLVM-based MPI usage sanitizer that not only instruments MPI applications at the IR level to report communication defects at runtime but also acts as an **active cryptographic network wrapper**. It is designed to help developers diagnose common errors in distributed programs (datatype mismatches, message-size mismatches, collective inconsistencies, deadlocks) while simultaneously enforcing **military-grade payload encryption, message authentication, and replay protection** on all point-to-point communication.

In addition to the core sanitizer, the project includes a VS Code extension that builds and runs MPI programs, reads generated reports, and surfaces diagnostics directly inside the editor through squiggles, CodeLens actions, and a dashboard panel.

## Objectives

- Develop an LLVM pass that identifies MPI calls and injects runtime hooks without requiring manual source-code changes.
- Build a runtime analysis library that records communication events and compares send and receive metadata across ranks.
- Detect and report MPI correctness issues, including datatype mismatches, buffer-size mismatches, collective mismatches, and deadlock risks.
- **Enforce Network Security (NPS):** Transparently upgrade insecure `MPI_Send`/`MPI_Recv` calls into a secure communication channel using AES-256-GCM payload encryption, HMAC authentication, and Sequence Number tracking.
- Integrate the sanitizer with VS Code so users can compile, run, inspect, and navigate diagnostics from the editor.

## Project Components

### 1. LLVM Pass

The pass instruments selected MPI functions and forwards relevant call-site information to the runtime. It captures:

- source location
- buffer pointer
- element count
- MPI datatype handle
- communicator handle
- peer rank or root rank

Furthermore, it completely **erases** insecure point-to-point MPI calls (`MPI_Send`, `MPI_Recv`) and statically links them to our secure runtime equivalents (`__msan_secure_send`, `__msan_secure_recv`).

### 2. Runtime Library

The runtime acts as both an analysis engine and a **cryptographic security enclave**. 
- **Sanitizer Engine:** Records events from all ranks, compares matching communication operations, generates final JSON reports, and produces a Graphviz DOT file for the communication topology.
- **Security Engine:** Transparently intercepts payload buffers, encrypts them using `EVP_aes_256_gcm` before putting them on the wire, attaches an authentication tag (HMAC) for integrity verification, and enforces strict sequence numbering for replay attack mitigation.

### 3. VS Code Extension

The extension provides a practical front end for the sanitizer. It includes:

- a build-and-run command
- an LSP-based diagnostic bridge
- CodeLens actions above MPI calls
- a webview dashboard for report visualization

## Advanced Network Security Features (NPS Focus)

This wrapper turns any insecure, legacy MPI C/C++ application into a hardened, zero-trust network topology without requiring *any* developer changes.

### 1. True Payload Encryption (Confidentiality)
All point-to-point data is encrypted in-memory before it hits the network card. We use **OpenSSL's AES-256-GCM** algorithm with a pre-shared key. The raw bytes traversing the network are purely ciphertext, protecting proprietary scientific data from packet sniffers.

### 2. Cryptographic Authentication (Integrity)
Because we use Galois/Counter Mode (GCM), an **Authenticated Encryption with Associated Data (AEAD)** cipher, every payload gets a 16-byte cryptographically secure Authentication Tag. If a Man-in-the-Middle (MitM) attempts to flip bits on the wire, the decryption step strictly fails and aborts the node with a `[SECURITY-ERROR]`.

### 3. Replay Attack Protection
The runtime tracks all peer-to-peer connection states. Each message is prepended with a strictly monotonically increasing **8-byte Sequence Number**. If an attacker records an encrypted message and attempts to replay it to the receiver later, the runtime detects the outdated sequence number and instantly aborts the connection with a `[SECURITY-ERROR] Replay attack detected!`.

## Repository Layout

- `passes/` - LLVM pass implementation
- `runtime/` - sanitizer runtime and event analysis
- `tests/` - MPI test cases with correct and incorrect behavior
- `examples/` - focused examples demonstrating specific mismatch types
- `mpi-sanitizer-vscode/` - VS Code extension workspace

## Build Instructions

### Prerequisites

- LLVM 18 or newer
- Clang
- CMake 3.16 or newer
- OpenMPI or MPICH
- **OpenSSL 3.x** (for `libcrypto` AES-GCM and HMAC operations)
- Node.js for the VS Code extension

### Build the sanitizer

From the repository root:

```bash
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build -j
```

This produces:

- `build/libMPISanitizePass.so`
- `build/libmsan_runtime.so`

### Build the VS Code extension

```bash
cd mpi-sanitizer-vscode
npm install
npm run compile
```

## Running the sanitizer manually

Example using `examples/type_mismatch.c`:

```bash
mkdir -p out
MPI_CFLAGS="$(mpicc --showme:compile)"
MPI_LDFLAGS="$(mpicc --showme:link)"

clang -g -O0 -emit-llvm -c ../examples/type_mismatch.c $MPI_CFLAGS -o out/type_mismatch.bc
opt -load-pass-plugin ../build/libMPISanitizePass.so -passes=mpi-sanitize \
  out/type_mismatch.bc -o out/type_mismatch.inst.bc
clang -g -O0 out/type_mismatch.inst.bc -o out/type_mismatch.inst \
  $MPI_LDFLAGS -L../build -lmsan_runtime -Wl,-rpath,$PWD/../build
LD_LIBRARY_PATH=$PWD/../build mpirun -np 2 out/type_mismatch.inst
```

## Running through VS Code

1. Open the `mpi-sanitizer-vscode` folder in VS Code.
2. Press `F5` to launch the Extension Development Host.
3. Open a C or C++ MPI sample file.
4. Open the Command Palette with `Ctrl+Shift+P`.
5. Run `MPI Sanitize: Build & Analyze` or `MPI Sanitize: Build & Analyse`.

The command compiles the active file, executes it under MPI, and reads the generated report back into the editor.

## Sample Programs

The extension workspace includes sample MPI programs that demonstrate both correct and buggy behavior:

- `samples/mpi_correct_pingpong.c`
- `samples/mpi_type_mismatch.c`
- `samples/mpi_size_mismatch.cpp`
- `samples/mpi_collective_mismatch.c`
- `samples/mpi_deadlock.c`

## Diagnostics and Reporting

The sanitizer report is consumed by the VS Code extension and shown as:

- editor squiggles for diagnostics
- CodeLens actions above MPI calls
- a dashboard for report inspection

The runtime also emits a communication graph file named `msan_comm_graph.dot`.

## Notes

- The runtime sets `MPI_ERRORS_RETURN` after initialization so MPI errors do not terminate execution before the sanitizer report is produced.
- If MPI headers are not resolved in VS Code, the extension workspace includes an IntelliSense configuration for OpenMPI include paths.

## Conclusion

The project combines compiler instrumentation, runtime analysis, and IDE integration to provide a practical workflow for diagnosing MPI communication errors. It is intended as a compact but complete demonstration of how static instrumentation and runtime validation can be combined into a developer-friendly debugging tool.

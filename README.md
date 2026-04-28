# LLVM MPI Sanitizer (MPISanitize)

LLVM pass + runtime that instruments MPI point-to-point calls and reports common mismatches at runtime.

See [desc.md](desc.md) for the full project writeup and roadmap.

## Requirements

- LLVM 14+ (`clang`, `opt`, `llvm-config`)
- CMake 3.16+
- MPI implementation (OpenMPI or MPICH)

On Ubuntu with LLVM 14 + OpenMPI, the following should exist:

- `clang --version`
- `opt --version`
- `llvm-config --version`
- `mpicc --version`

## Build

```bash
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build -j
```

This builds:

- `build/libMPISanitizePass.so` (LLVM pass plugin)
- `build/libmsan_runtime.so` (runtime library)

## What gets instrumented

Currently the pass instruments:

- `MPI_Init` / `MPI_Init_thread` (runtime init)
- `MPI_Send` (before-call hook)
- `MPI_Recv` (after-call hook)
- `MPI_Finalize` (final report)

## Run (manual pipeline)

Example: instrument and run `examples/type_mismatch.c`.

```bash
mkdir -p out
MPI_CFLAGS="$(mpicc --showme:compile)"
MPI_LDFLAGS="$(mpicc --showme:link)"

# 1) C -> LLVM bitcode
clang -g -O0 -emit-llvm -c examples/type_mismatch.c $MPI_CFLAGS -o out/type_mismatch.bc

# 2) Run the pass (injects calls to the runtime)
opt -load-pass-plugin build/libMPISanitizePass.so -passes=mpi-sanitize \
  out/type_mismatch.bc -o out/type_mismatch.inst.bc

# 3) Link against MPI + sanitizer runtime
clang -g -O0 out/type_mismatch.inst.bc -o out/type_mismatch.inst \
  $MPI_LDFLAGS -Lbuild -lmsan_runtime -Wl,-rpath,$PWD/build

# 4) Execute (2 ranks for the provided examples)
LD_LIBRARY_PATH=$PWD/build mpirun -np 2 out/type_mismatch.inst
```

You should see reports like:

- `[msan][type-mismatch] ...`
- `[msan][size-mismatch] ...`

## Examples

- `examples/type_mismatch.c`: sender uses `MPI_INT`, receiver uses `MPI_DOUBLE`
- `examples/size_mismatch.c`: sender sends 4 ints, receiver only receives 2 ints

Run them the same way as above (just swap the input file name).

## Notes

- The runtime sets `MPI_ERRORS_RETURN` after `MPI_Init` so truncation errors (e.g. `MPI_ERR_TRUNCATE`) don’t abort the job before the sanitizer prints its report at `MPI_Finalize`.

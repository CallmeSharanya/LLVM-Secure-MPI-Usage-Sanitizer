# MPI Sanitizer Sample Programs

- mpi_correct_pingpong.c: correct send/recv pair.
- mpi_type_mismatch.c: send MPI_INT, receive MPI_DOUBLE.
- mpi_size_mismatch.cpp: send 6 ints, receive 2 ints.
- mpi_collective_mismatch.c: ranks call different collectives.
- mpi_deadlock.c: two ranks wait on recv with no matching sends.

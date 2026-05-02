/*
 * wrong_tag.c — Tag mismatch between send and recv
 *
 * Bug tested: Mismatched tags causing unmatched send
 * Expected sanitizer output:
 *   [msan][unmatched-send] — rank 0's send (tag=5) has no matching recv
 *
 * Rank 0 sends two messages to rank 1: one with tag=5 (the wrong tag
 * that nobody receives) and one with tag=77 (which rank 1 does receive).
 * Rank 1 only receives tag=77 and sends tag=88 back to rank 0.
 *
 * The send with tag=5 remains unmatched. MPI buffers the small message
 * so the program completes without hanging.
 *
 * Run with: mpirun -np 2 ./wrong_tag
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank = -1, size = -1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != 2) {
        if (rank == 0)
            fprintf(stderr, "Error: requires exactly 2 ranks\n");
        MPI_Finalize();
        return 1;
    }

    int val = rank;
    MPI_Status st;

    if (rank == 0) {
        /* Send with tag=5 — rank 1 never receives this => unmatched-send */
        int extra = 999;
        MPI_Send(&extra, 1, MPI_INT, 1, 5, MPI_COMM_WORLD);

        /* Send with tag=77 — rank 1 receives this (matched) */
        MPI_Send(&val, 1, MPI_INT, 1, 77, MPI_COMM_WORLD);

        /* Recv tag=88 from rank 1 (matched) */
        MPI_Recv(&val, 1, MPI_INT, 1, 88, MPI_COMM_WORLD, &st);
    } else {
        /* Recv tag=77 from rank 0 (matches rank 0's second send) */
        MPI_Recv(&val, 1, MPI_INT, 0, 77, MPI_COMM_WORLD, &st);

        /* Send tag=88 to rank 0 (matches rank 0's recv) */
        int reply = 42;
        MPI_Send(&reply, 1, MPI_INT, 0, 88, MPI_COMM_WORLD);
    }

    MPI_Finalize();
    return 0;
}

/*
 * correct_multi_send.c — Correct multi-target point-to-point
 *
 * Bug tested: NONE (correct program)
 * Expected sanitizer output: No [msan] errors
 *
 * Rank 0 sends a unique int to each of ranks 1, 2, 3.
 * Each rank receives exactly what rank 0 sent to it.
 * All types, counts, and tags match.
 *
 * NOTE: Each send uses a separate buffer element (vals[dest]) to
 * avoid triggering overlap-warning on consecutive sends sharing
 * the same buffer address.
 *
 * Run with: mpirun --oversubscribe -np 4 ./correct_multi_send
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank = -1, size = -1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 4) {
        if (rank == 0)
            fprintf(stderr, "Error: requires at least 4 ranks\n");
        MPI_Finalize();
        return 1;
    }

    if (rank == 0) {
        /* Use separate buffer slots to avoid overlap-warning */
        int vals[4];
        for (int dest = 1; dest < 4; dest++) {
            vals[dest] = dest * 100;
            MPI_Send(&vals[dest], 1, MPI_INT, dest, dest, MPI_COMM_WORLD);
            fprintf(stderr, "Rank 0: sent %d to rank %d (tag=%d)\n",
                    vals[dest], dest, dest);
        }
    } else if (rank < 4) {
        /* Ranks 1-3 each receive from rank 0 */
        int val = 0;
        MPI_Status st;
        MPI_Recv(&val, 1, MPI_INT, 0, rank, MPI_COMM_WORLD, &st);
        fprintf(stderr, "Rank %d: received %d from rank 0\n", rank, val);
    }

    MPI_Finalize();
    return 0;
}

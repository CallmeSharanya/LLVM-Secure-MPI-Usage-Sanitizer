/*
 * bcast_wrong_root.c — MPI_Bcast with mismatched root
 *
 * Bug tested: Collective root mismatch
 * Expected: [msan][collective-mismatch]
 *
 * Rank 0 calls MPI_Bcast with root=0, Rank 1 with root=1.
 * Run with: mpirun -np 2 ./bcast_wrong_root
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

    int val = 42;
    int root = rank;

    fprintf(stderr, "Rank %d: calling MPI_Bcast with root=%d\n", rank, root);
    MPI_Bcast(&val, 1, MPI_INT, root, MPI_COMM_WORLD);
    fprintf(stderr, "Rank %d: bcast value = %d\n", rank, val);

    MPI_Finalize();
    return 0;
}

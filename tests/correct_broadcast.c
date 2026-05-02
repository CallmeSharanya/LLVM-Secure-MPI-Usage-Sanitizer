/*
 * correct_broadcast.c — Correct MPI_Bcast usage
 *
 * Bug tested: NONE (correct program)
 * Expected sanitizer output: No [msan] errors
 *
 * Rank 0 broadcasts an int array of size 4 to all ranks.
 * All ranks use MPI_INT, count=4, same root=0.
 *
 * Run with: mpirun --oversubscribe -np 4 ./correct_broadcast
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

    int data[4] = {0, 0, 0, 0};

    if (rank == 0) {
        data[0] = 10;
        data[1] = 20;
        data[2] = 30;
        data[3] = 40;
    }

    MPI_Bcast(data, 4, MPI_INT, 0, MPI_COMM_WORLD);

    fprintf(stderr, "Rank %d: data = [%d, %d, %d, %d]\n",
            rank, data[0], data[1], data[2], data[3]);

    MPI_Finalize();
    return 0;
}

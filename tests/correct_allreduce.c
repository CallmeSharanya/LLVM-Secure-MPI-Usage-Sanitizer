/*
 * correct_allreduce.c — Correct MPI_Allreduce usage
 *
 * Bug tested: NONE (correct program)
 * Expected sanitizer output: No [msan] errors
 *
 * All ranks participate in MPI_Allreduce with MPI_SUM.
 * Matching types and counts across all ranks.
 *
 * Run with: mpirun --oversubscribe -np 4 ./correct_allreduce
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

    int send_val = rank + 1;
    int recv_val = 0;

    MPI_Allreduce(&send_val, &recv_val, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    fprintf(stderr, "Rank %d: allreduce result = %d (expected %d)\n",
            rank, recv_val, (size * (size + 1)) / 2);

    MPI_Finalize();
    return 0;
}

/*
 * correct_pingpong.c — Correct ping-pong communication
 *
 * Bug tested: NONE (correct program)
 * Expected sanitizer output: No [msan] errors
 *
 * Rank 0 sends an int to rank 1, rank 1 sends an int back to rank 0.
 * All types, counts, and tags match on both sides.
 *
 * Run with: mpirun -np 2 ./correct_pingpong
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

    int send_val, recv_val;
    MPI_Status st;

    if (rank == 0) {
        send_val = 42;
        MPI_Send(&send_val, 1, MPI_INT, 1, 10, MPI_COMM_WORLD);
        MPI_Recv(&recv_val, 1, MPI_INT, 1, 20, MPI_COMM_WORLD, &st);
        fprintf(stderr, "Rank 0: sent %d, received %d\n", send_val, recv_val);
    } else {
        MPI_Recv(&recv_val, 1, MPI_INT, 0, 10, MPI_COMM_WORLD, &st);
        send_val = recv_val + 1;
        MPI_Send(&send_val, 1, MPI_INT, 0, 20, MPI_COMM_WORLD);
        fprintf(stderr, "Rank 1: received %d, sent %d\n", recv_val, send_val);
    }

    MPI_Finalize();
    return 0;
}

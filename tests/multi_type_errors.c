/*
 * multi_type_errors.c — Multiple errors in one run
 *
 * Bug tested: Both type-mismatch and size-mismatch
 * Expected:
 *   [msan][size-mismatch] — tag=1: send count=4, recv count=2
 *   [msan][type-mismatch] — tag=2: send MPI_INT, recv MPI_DOUBLE
 *   tag=3: correct (no error)
 *
 * Rank 0 sends 3 messages to rank 1:
 *   tag=1: MPI_INT count=4 -> rank 1 recvs MPI_INT count=2
 *   tag=2: MPI_INT count=1 -> rank 1 recvs MPI_DOUBLE count=1
 *   tag=3: MPI_FLOAT count=1 -> rank 1 recvs MPI_FLOAT count=1
 *
 * Run with: mpirun -np 2 ./multi_type_errors
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

    MPI_Status st;

    if (rank == 0) {
        /* tag=1: send 4 ints (rank 1 only recvs 2 => size mismatch) */
        int buf1[4] = {1, 2, 3, 4};
        MPI_Send(buf1, 4, MPI_INT, 1, 1, MPI_COMM_WORLD);

        /* tag=2: send 1 int (rank 1 recvs as double => type mismatch) */
        int buf2 = 99;
        MPI_Send(&buf2, 1, MPI_INT, 1, 2, MPI_COMM_WORLD);

        /* tag=3: send 1 float (correct — rank 1 recvs 1 float) */
        float buf3 = 2.718f;
        MPI_Send(&buf3, 1, MPI_FLOAT, 1, 3, MPI_COMM_WORLD);
    } else {
        /* tag=1: recv only 2 ints (size mismatch with 4 sent) */
        int rbuf1[2] = {0, 0};
        MPI_Recv(rbuf1, 2, MPI_INT, 0, 1, MPI_COMM_WORLD, &st);

        /* tag=2: recv as double (type mismatch with int sent) */
        double rbuf2 = 0.0;
        MPI_Recv(&rbuf2, 1, MPI_DOUBLE, 0, 2, MPI_COMM_WORLD, &st);

        /* tag=3: recv as float (correct) */
        float rbuf3 = 0.0f;
        MPI_Recv(&rbuf3, 1, MPI_FLOAT, 0, 3, MPI_COMM_WORLD, &st);

        fprintf(stderr, "Rank 1: received all 3 messages\n");
    }

    MPI_Finalize();
    return 0;
}

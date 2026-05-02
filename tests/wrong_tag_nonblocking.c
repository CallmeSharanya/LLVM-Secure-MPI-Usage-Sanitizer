/*
 * wrong_tag_nonblocking.c — Type mismatch via cross-send pattern
 *
 * Bug tested: Type mismatch in cross-communication
 * Expected sanitizer output:
 *   [msan][type-mismatch] — MPI_DOUBLE sent but MPI_INT received (or vice versa)
 *
 * Since MPI_Isend/MPI_Irecv are NOT instrumented, we use blocking calls.
 *
 * Rank 0: sends a double with tag=10, recvs an int with tag=20
 * Rank 1: sends an int with tag=20, recvs a double with tag=10
 *
 * The cross-send completes correctly in MPI, but:
 *   - Rank 0 sends MPI_DOUBLE (tag=10) -> Rank 1 recvs MPI_DOUBLE (tag=10): OK
 *   - Rank 1 sends MPI_INT (tag=20)    -> Rank 0 recvs MPI_INT (tag=20): OK
 *
 * To trigger type mismatch, rank 0 sends MPI_DOUBLE but rank 1 recvs MPI_INT
 * on the same tag:
 *   Rank 0: send MPI_DOUBLE (tag=10)
 *   Rank 1: recv MPI_INT (tag=10)  — TYPE MISMATCH
 *   Rank 1: send MPI_INT (tag=20)
 *   Rank 0: recv MPI_INT (tag=20)  — correct
 *
 * Run with: mpirun -np 2 ./wrong_tag_nonblocking
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
        /* Send a double to rank 1 with tag=10 */
        double send_d = 3.14159;
        MPI_Send(&send_d, 1, MPI_DOUBLE, 1, 10, MPI_COMM_WORLD);

        /* Recv an int from rank 1 with tag=20 (this is correct) */
        int recv_i = 0;
        MPI_Recv(&recv_i, 1, MPI_INT, 1, 20, MPI_COMM_WORLD, &st);
        fprintf(stderr, "Rank 0: received int %d\n", recv_i);
    } else {
        /* Recv MPI_INT from rank 0 with tag=10 — BUT rank 0 sent MPI_DOUBLE!
         * This is the type mismatch the sanitizer should catch. */
        int recv_i = 0;
        MPI_Recv(&recv_i, 1, MPI_INT, 0, 10, MPI_COMM_WORLD, &st);
        fprintf(stderr, "Rank 1: received int %d (expected double — type mismatch!)\n", recv_i);

        /* Send an int back to rank 0 with tag=20 */
        int send_i = 99;
        MPI_Send(&send_i, 1, MPI_INT, 0, 20, MPI_COMM_WORLD);
    }

    MPI_Finalize();
    return 0;
}

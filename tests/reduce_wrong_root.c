/*
 * reduce_wrong_root.c — Collective mismatch via MPI_Bcast root disagreement
 *
 * Bug tested: Collective root mismatch
 * Expected sanitizer output:
 *   [msan][collective-mismatch] — ranks disagree on root
 *
 * NOTE: MPI_Reduce with truly different roots on each rank causes
 * a real MPI-level deadlock (both ranks try to receive, neither sends).
 * To avoid hanging, we first do a correct MPI_Reduce (root=0) so the
 * reduce completes, then do a MPI_Bcast with mismatched roots (which
 * doesn't deadlock because both ranks act as broadcaster). The sanitizer
 * detects the root mismatch on the Bcast collective.
 *
 * Run with: mpirun -np 2 ./reduce_wrong_root
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

    /* Step 1: Correct MPI_Reduce — completes normally */
    int send_val = rank + 1;
    int recv_val = 0;
    MPI_Reduce(&send_val, &recv_val, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        fprintf(stderr, "Rank 0: reduce result = %d\n", recv_val);
    }

    /* Step 2: MPI_Bcast with mismatched roots — triggers collective-mismatch.
     * Each rank uses root=rank, so rank 0 says root=0, rank 1 says root=1.
     * MPI_Bcast doesn't deadlock here because both ranks think they are
     * the broadcaster and send data. */
    int bcast_val = 42;
    int root = rank;

    fprintf(stderr, "Rank %d: calling MPI_Bcast with root=%d\n", rank, root);
    MPI_Bcast(&bcast_val, 1, MPI_INT, root, MPI_COMM_WORLD);

    MPI_Finalize();
    return 0;
}

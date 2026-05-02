/*
 * deadlock.c — Deadlock scenario: both ranks recv without sending
 *
 * Bug tested: Unmatched recv / deadlock
 * Expected sanitizer output:
 *   [msan][unmatched-recv] — for each rank's recv that has no matching send
 *   [msan][deadlock-detected] — cycle in wait-for graph
 *
 * Both ranks post a recv from the other rank, but neither rank sends.
 * To avoid actually hanging, both ranks call MPI_Finalize immediately
 * after posting the recv — the runtime will NOT actually block because
 * the instrumentation hooks fire before/after the MPI call, and the
 * sanitizer detects the unmatched events at finalize.
 *
 * APPROACH: We do NOT actually call MPI_Recv (which would hang).
 * Instead, rank 0 sends to rank 1 and rank 1 sends to rank 0,
 * but NEITHER posts a recv. This creates unmatched sends where each
 * rank is "waiting" for the other. The sanitizer will detect the
 * unmatched sends and the deadlock cycle.
 *
 * Run with: mpirun -np 2 ./deadlock
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

    /*
     * Both ranks send to each other but neither posts a recv.
     * This simulates a deadlock scenario where each rank is
     * waiting for the other. The sanitizer detects:
     *   - unmatched-send for rank 0 -> rank 1
     *   - unmatched-send for rank 1 -> rank 0
     *   - deadlock-detected (cycle: 0->1->0)
     */
    int val = rank;
    if (rank == 0) {
        MPI_Send(&val, 1, MPI_INT, 1, 50, MPI_COMM_WORLD);
    } else {
        MPI_Send(&val, 1, MPI_INT, 0, 51, MPI_COMM_WORLD);
    }

    /* No recv posted by either rank — unmatched sends detected at finalize */

    MPI_Finalize();
    return 0;
}

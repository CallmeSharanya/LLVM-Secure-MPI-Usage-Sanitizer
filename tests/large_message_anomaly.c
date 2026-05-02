/*
 * large_message_anomaly.c — Anomaly detection via huge message
 *
 * Bug tested: Message size anomaly
 * Expected: [msan][anomaly-warning]
 *
 * Rank 0 sends 20 small messages (1 int = 4 bytes each, tags 1-20)
 * then 1 huge message (1,000,000 ints = 4 MB, tag 21).
 * Rank 1 receives all 21 with matching types/counts.
 *
 * With 21 send samples, the mean ≈ 190 KB and stddev ≈ 852 KB.
 * The huge message (4 MB) exceeds mean + 3*stddev ≈ 2.75 MB,
 * triggering the anomaly detector.
 *
 * NOTE: Using only 5-6 small messages is insufficient because the
 * single outlier inflates the stddev so much that it stays within 3σ.
 * 20 small messages provide enough baseline to make the outlier detectable.
 *
 * Run with: mpirun -np 2 ./large_message_anomaly
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_SMALL  20
#define HUGE_COUNT 1000000

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

    if (rank == 0) {
        /* Send 20 small messages (1 int each) */
        for (int t = 1; t <= NUM_SMALL; t++) {
            int val = t * 10;
            MPI_Send(&val, 1, MPI_INT, 1, t, MPI_COMM_WORLD);
        }

        /* Send 1 huge message (1,000,000 ints) */
        int *big = (int *)calloc(HUGE_COUNT, sizeof(int));
        if (!big) {
            fprintf(stderr, "Rank 0: OOM\n");
            MPI_Finalize();
            return 1;
        }
        for (int i = 0; i < HUGE_COUNT; i++)
            big[i] = i;
        MPI_Send(big, HUGE_COUNT, MPI_INT, 1, NUM_SMALL + 1, MPI_COMM_WORLD);
        free(big);
        fprintf(stderr, "Rank 0: sent %d small + 1 huge message\n", NUM_SMALL);
    } else {
        MPI_Status st;
        /* Recv 20 small messages */
        for (int t = 1; t <= NUM_SMALL; t++) {
            int val = 0;
            MPI_Recv(&val, 1, MPI_INT, 0, t, MPI_COMM_WORLD, &st);
        }

        /* Recv the huge message */
        int *big = (int *)calloc(HUGE_COUNT, sizeof(int));
        if (!big) {
            fprintf(stderr, "Rank 1: OOM\n");
            MPI_Finalize();
            return 1;
        }
        MPI_Recv(big, HUGE_COUNT, MPI_INT, 0, NUM_SMALL + 1, MPI_COMM_WORLD, &st);
        fprintf(stderr, "Rank 1: received all %d messages\n", NUM_SMALL + 1);
        free(big);
    }

    MPI_Finalize();
    return 0;
}

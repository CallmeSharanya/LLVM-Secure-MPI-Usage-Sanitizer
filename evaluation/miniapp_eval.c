/*
 * miniapp_eval.c — Distributed Jacobi Iteration Mini-Application
 *
 * A realistic MPI workload for evaluating the LLVM MPI Sanitizer.
 * Simulates a 1D PDE solver where each rank owns a chunk of the
 * domain and exchanges boundary values with neighbors each iteration.
 *
 * Expected sanitizer output:
 *   - NO type-mismatch, size-mismatch, or other errors
 *   - [msan][info] Average P2P latency
 *   - [msan][summary] with all error counts = 0
 *   - msan_comm_graph.dot showing ring/chain topology
 *
 * Run with: mpirun --oversubscribe -np 4 ./miniapp_eval
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GRID_SIZE      100
#define NUM_ITERATIONS 10

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank = -1, size = -1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) {
        if (rank == 0)
            fprintf(stderr, "Error: requires at least 2 ranks\n");
        MPI_Finalize();
        return 1;
    }

    /* Initialize local grid: each cell = 1.0 + rank*0.01 */
    double grid[GRID_SIZE];
    for (int i = 0; i < GRID_SIZE; i++) {
        grid[i] = 1.0 + rank * 0.01;
    }

    double left_ghost = 0.0;
    double right_ghost = 0.0;

    int left_rank  = rank - 1;
    int right_rank = rank + 1;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        MPI_Status st;

        /*
         * Use unique tags per iteration to avoid replay-detection
         * false positives. Tag scheme:
         *   right boundary send/recv: 100 + iter
         *   left  boundary send/recv: 200 + iter
         */
        int tag_right = 100 + iter;
        int tag_left  = 200 + iter;

        /* Exchange with right neighbor */
        if (right_rank < size) {
            /* Send rightmost value to right neighbor */
            MPI_Send(&grid[GRID_SIZE - 1], 1, MPI_DOUBLE,
                     right_rank, tag_right, MPI_COMM_WORLD);
            /* Receive left boundary from right neighbor */
            MPI_Recv(&right_ghost, 1, MPI_DOUBLE,
                     right_rank, tag_left, MPI_COMM_WORLD, &st);
        }

        /* Exchange with left neighbor */
        if (left_rank >= 0) {
            /* Send leftmost value to left neighbor */
            MPI_Send(&grid[0], 1, MPI_DOUBLE,
                     left_rank, tag_left, MPI_COMM_WORLD);
            /* Receive right boundary from left neighbor */
            MPI_Recv(&left_ghost, 1, MPI_DOUBLE,
                     left_rank, tag_right, MPI_COMM_WORLD, &st);
        }

        /* Jacobi update: new[i] = 0.5 * (left + right) */
        double new_grid[GRID_SIZE];
        for (int i = 0; i < GRID_SIZE; i++) {
            double left_val  = (i > 0)              ? grid[i - 1] : left_ghost;
            double right_val = (i < GRID_SIZE - 1)  ? grid[i + 1] : right_ghost;
            new_grid[i] = 0.5 * (left_val + right_val);
        }

        for (int i = 0; i < GRID_SIZE; i++) {
            grid[i] = new_grid[i];
        }
    }

    /* Compute local sum */
    double local_sum = 0.0;
    for (int i = 0; i < GRID_SIZE; i++) {
        local_sum += grid[i];
    }

    /* Global sum via Allreduce */
    double global_sum = 0.0;
    MPI_Allreduce(&local_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);

    if (rank == 0) {
        fprintf(stderr, "Jacobi iteration complete (%d iterations, "
                "%d ranks x %d cells). Global sum = %.6f\n",
                NUM_ITERATIONS, size, GRID_SIZE, global_sum);
    }

    MPI_Finalize();
    return 0;
}

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) {
        if (rank == 0) printf("Run with at least 2 ranks\n");
        MPI_Finalize();
        return 0;
    }

    int val = 100;
    // Mismatch: Rank 0 thinks root is 0, Rank 1 thinks root is 1
    int root = (rank == 0) ? 0 : 1;
    
    if (rank == 0) printf("Rank 0 calling Bcast with root 0\n");
    if (rank == 1) printf("Rank 1 calling Bcast with root 1\n");
    
    MPI_Bcast(&val, 1, MPI_INT, root, MPI_COMM_WORLD);

    if (rank == 0) printf("Rank 0 finished Bcast\n");
    if (rank == 1) printf("Rank 1 finished Bcast\n");

    MPI_Finalize();
    return 0;
}

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
      fprintf(stderr, "Run with exactly 2 ranks\n");
    MPI_Finalize();
    return 1;
  }

  if (rank == 0) {
    int x = 42;
    MPI_Send(&x, 1, MPI_INT, 1, 7, MPI_COMM_WORLD);
  } else {
    double y = 0;
    MPI_Status st;
    MPI_Recv(&y, 1, MPI_DOUBLE, 0, 7, MPI_COMM_WORLD, &st);
  }

  MPI_Finalize();
  return 0;
}

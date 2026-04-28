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
    int buf[4] = {1, 2, 3, 4};
    MPI_Send(buf, 4, MPI_INT, 1, 9, MPI_COMM_WORLD);
  } else {
    int small[2] = {0, 0};
    MPI_Status st;
    MPI_Recv(small, 2, MPI_INT, 0, 9, MPI_COMM_WORLD, &st);
  }

  MPI_Finalize();
  return 0;
}

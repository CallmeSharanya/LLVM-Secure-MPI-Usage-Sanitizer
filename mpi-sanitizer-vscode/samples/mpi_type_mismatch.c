#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if (rank == 0) {
    int buf[4] = {1, 2, 3, 4};
    MPI_Send(buf, 4, MPI_INT, 1, 7, MPI_COMM_WORLD);
  } else if (rank == 1) {
    double buf[4] = {0};
    MPI_Recv(buf, 4, MPI_DOUBLE, 0, 7, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    printf("rank 1 got %f\n", buf[0]);
  }

  MPI_Finalize();
  return 0;
}

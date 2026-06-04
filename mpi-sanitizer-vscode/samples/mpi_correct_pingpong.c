#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if (rank == 0) {
    int buf = 42;
    MPI_Send(&buf, 1, MPI_INT, 1, 5, MPI_COMM_WORLD);
    MPI_Recv(&buf, 1, MPI_INT, 1, 6, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  } else if (rank == 1) {
    int buf = 0;
    MPI_Recv(&buf, 1, MPI_INT, 0, 5, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    buf += 1;
    MPI_Send(&buf, 1, MPI_INT, 0, 6, MPI_COMM_WORLD);
    printf("rank 1 replied %d\n", buf);
  }

  MPI_Finalize();
  return 0;
}

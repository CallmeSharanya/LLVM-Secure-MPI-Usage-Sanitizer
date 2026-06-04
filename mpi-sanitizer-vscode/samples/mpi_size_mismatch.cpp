#include <mpi.h>
#include <iostream>

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if (rank == 0) {
    int buf[6] = {1, 2, 3, 4, 5, 6};
    MPI_Send(buf, 6, MPI_INT, 1, 13, MPI_COMM_WORLD);
  } else if (rank == 1) {
    int buf[2] = {0};
    MPI_Recv(buf, 2, MPI_INT, 0, 13, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    std::cout << "rank 1 got " << buf[0] << std::endl;
  }

  MPI_Finalize();
  return 0;
}

#include <stdio.h>

typedef int MPI_Comm;
typedef int MPI_Datatype;
typedef int MPI_Op;

#define MPI_COMM_WORLD 0
#define MPI_INT 0
#define MPI_SUM 0

static inline int MPI_Init(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  return 0;
}

static inline int MPI_Comm_rank(MPI_Comm comm, int *rank) {
  (void)comm;
  *rank = 0;
  return 0;
}

static inline int MPI_Bcast(void *buffer, int count, MPI_Datatype datatype,
                            int root, MPI_Comm comm) {
  (void)buffer;
  (void)count;
  (void)datatype;
  (void)root;
  (void)comm;
  return 0;
}

static inline int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
                             MPI_Datatype datatype, MPI_Op op, int root,
                             MPI_Comm comm) {
  (void)sendbuf;
  (void)recvbuf;
  (void)count;
  (void)datatype;
  (void)op;
  (void)root;
  (void)comm;
  return 0;
}

static inline int MPI_Finalize(void) { return 0; }

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  int buf = 0;
  if (rank == 0) {
    buf = 99;
    MPI_Bcast(&buf, 1, MPI_INT, 0, MPI_COMM_WORLD);
  } else {
    MPI_Reduce(&buf, &buf, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
  }

  MPI_Finalize();
  return 0;
}

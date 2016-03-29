rdma-experiments
================

These are some simple experiments to help me understand how to use
RDMA with the InfiniBand Verbs interface.

On the Sampa cluster, do something like:

```
# use modern versions of GCC and MPI
module purge
module load gcc intel-mpi

# build 
make simple_write

# run on 4 cluster nodes, with 3 processes (MPI ranks) per node,
# labeling output lines with the rank ID
srun --label --nodes=4 --ntasks-per-node=3 ./simple_write
```

Useful resources
----------------

Good places to look for more on MPI:
* Livermore tutorial: https://computing.llnl.gov/tutorials/mpi/
* MPI spec: http://www.mpi-forum.org/docs/mpi-3.0/mpi30-report.pdf

Good places to look for more on Verbs:
* The Verbs header file, (usually) at ```/usr/include/infiniband/verbs.h```
* Mellanox RDMA-Aware Programming manual: http://www.mellanox.com/related-docs/prod_software/RDMA_Aware_Programming_user_manual.pdf
* libibverbs examples: http://git.kernel.org/cgit/libs/infiniband/libibverbs.git/tree/examples

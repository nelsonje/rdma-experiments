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

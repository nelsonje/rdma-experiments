rdma-experiments
================

Some simple experiments to help me understand how to use RDMA with the InfiniBand Verbs interface

Note: with OpenMPI, I need to add some flags to run using RoCE, described [here](http://www.open-mpi.org/faq/?category=openfabrics#ompi-over-roce). Example:
```mpirun --mca btl openib,self,sm --mca btl_openib_cpc_include rdmacm --hosts```. This works for me on thing1/thing2.

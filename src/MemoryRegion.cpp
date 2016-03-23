#include "MemoryRegion.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "common.hpp"

#include <mpi.h>

#include <limits>
#include <cstdlib>
#include <cstdint>

namespace verbs {

  void MemoryRegion::register_region( void * buf, const size_t size ) {
    size_ = size;
    buf_ = buf;

    // register memory
    //CHECK_NOTNULL( mr = ib.register_memory_region( buf, size_ ) );
    CHECK_NOTNULL( mr_ = ibv_reg_mr( ib_.get_protection_domain(), 
                                     buf, size,
                                     ( IBV_ACCESS_LOCAL_WRITE  | 
                                       IBV_ACCESS_REMOTE_WRITE | 
                                       IBV_ACCESS_REMOTE_READ  |
                                       IBV_ACCESS_REMOTE_ATOMIC) ) );

    DVLOG(2) << "Registering memory region of " << size << " bytes at " << buf 
             << " with keys " << (void*) (uint64_t) mr_->lkey << "/" << (void*) (uint64_t) mr_->rkey;

    // distribute rkeys
    comm_.barrier();
    rkeys_.reset( new uint32_t[ comm_.size ] );
    MPI_CHECK( MPI_Allgather( &mr_->rkey, 1, MPI_INT32_T,
                              &rkeys_[0], 1, MPI_INT32_T,
                              MPI_COMM_WORLD ) );

    // distribute base addresses
    comm_.barrier();
    bases_.reset( new void*[ comm_.size ] );
    MPI_CHECK( MPI_Allgather( &buf_, sizeof(buf_), MPI_BYTE,
                              &bases_[0], sizeof(buf_), MPI_BYTE,
                              MPI_COMM_WORLD ) );

  }

  void MemoryRegion::finalize() {
    if( mr_ ) {
      PCHECK( ibv_dereg_mr( mr_ ) >= 0 );
      mr_ = NULL;
    }
  }

}

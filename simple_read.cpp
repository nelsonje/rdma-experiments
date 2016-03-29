//
// MPI / InfiniBand Verbs simple read demo
//
// Run on Sampa cluster with command like:
//   make && srun --label --nodes=2 --ntasks-per-node=3 ./simple_read
//

#include "MPIConnection.hpp"
#include "Verbs.hpp"
#include "SymmetricMemoryRegion.hpp"
#include "SymmetricAllocator.hpp"

#include <cstring>
#include <sys/types.h>
#include <unistd.h>

int main( int argc, char * argv[] ) {

  // set up MPI communication between all processes in job
  MPIConnection mpi( &argc, &argv );

  // set up IBVerbs queue pairs between all processes in job
  Verbs verbs( mpi );

  // set up symmetric allocator
  SymmetricAllocator allocator( mpi );

#ifdef VERBOSE
  std::cout << "hostname " << mpi.hostname()
            << " MPI rank " << mpi.rank
            << " ranks " << mpi.ranks
            << " locale " << mpi.locale
            << " locales " << mpi.locales
            << " locale rank " << mpi.locale_rank
            << " locale ranks " << mpi.locale_size
            << " pid " << getpid()
            << "\n";
#endif

  //
  // verify reads are working by writing a value from an array on
  // every rank. Rank n will read the product n*m from the nth
  // location of an array on rank m.
  //
  
  // create space to store data for remote ranks
  //
  // There are at least three options for doing this. I'll illustrate
  // two here.
  //
  // First, we can allocate the data statically with a fixed size so
  // that it's at the same base address on every core, like this:
  
  // static int64_t remote_rank_data[ 1 << 20 ]; // 2^20 endpoints should be enough. :-)
  // SymmetricMemoryRegion source_mr( verbs, &remote_rank_data[0], sizeof(remote_rank_data) );  // register allocated memory

  // Second, we can use my symmetric allocator code from Grappa to
  // dynamically allocate space at the same address on all cores.
  int64_t * remote_rank_data = allocator.alloc< int64_t >( mpi.size );
  SymmetricMemoryRegion source_mr( verbs, &remote_rank_data[0], sizeof(int64_t) * mpi.size ); // register allocated memory

  // The third option would be to allocate memory using normal
  // mechanisms on all cores, without caring what addresses the blocks
  // were allocated at, and then communicate each base addresses to
  // all other cores so that they can form a proper virtual address
  // for whatever core they're writing to. This gets a little ugly, so
  // I'm not doing it here.
  
  // initialize array
  for( int64_t i = 0; i < mpi.size; ++i ) {
    remote_rank_data[i] = i * mpi.rank;
  }

  // ensure initialization is done before anybody reads anything
  mpi.barrier();
  
#ifdef VERBOSE
  std::cout << "Base address of remote_rank_data is " << &remote_rank_data[0] << std::endl;
#endif
    

  // create storage for destination data
  int64_t my_data;
  MemoryRegion dest_mr( verbs, &my_data, sizeof(my_data) );

  
  // assume that we'll get all values correctly; check as we receive them in the loop below.
  bool pass = true;

  // write our rank data to remote ranks, one at a time
  for( int i = 0; i < mpi.size; ++i ) {
    // reset destination location to wrong value to detect error
    my_data = -1;
    
    // point scatter/gather element at destination data
    ibv_sge sge;
    std::memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t) dest_mr.base();
    sge.length = dest_mr.size();
    sge.lkey = dest_mr.lkey();

    // create work request for RDMA read
    ibv_send_wr wr;
    std::memset(&wr, 0, sizeof(wr));
    wr.wr_id = i;  // unused here
    wr.next = nullptr; // only one send WR in this linked list
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.imm_data = 0;   // unused here
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED; // create completion queue entry once this operation has completed
    wr.wr.rdma.remote_addr = (uintptr_t) &remote_rank_data[ mpi.rank ]; // read from this rank's slot of remote array
    wr.wr.rdma.rkey = source_mr.rkey( i );

    // hand WR to library/card to send
    verbs.post_send( i, &wr );

    // wait until WR is complete before continuing.
    //
    // If you don't want to wait, you must ensure that 1) source data
    // is unchanged until the WR has completed, and 2) you don't post
    // WRs too fast for the card.
    while( !verbs.poll() ) {
      ; // poll until we get a completion queue entry
    }

    // check that we read the right value
    int64_t expected_value = i * mpi.rank;
    if( expected_value != my_data ) {
      pass = false;
      std::cout << "Rank " << mpi.rank
                << " got bad data from rank " << i
                << ": expected " << expected_value
                << ", got " << remote_rank_data[i]
                << std::endl;
    }
  }
  
  // wait for everyone to finish all reads
  mpi.barrier();
  
  // Use MPI reduction operation to AND together all ranks' "pass" value.
  bool overall_pass = false;
  MPI_CHECK( MPI_Reduce( &pass, &overall_pass, 1, MPI_C_BOOL,
                         MPI_LAND,  // logical and
                         0,         // destination rank
                         mpi.main_communicator_ ) );

  // have one rank check the reduced value
  if( 0 == mpi.rank ){
    if( overall_pass ) {
      std::cout << "PASS: All ranks received correct data." << std::endl;
    } else {
      std::cout << "FAIL: Some rank(s) received incorrect data!" << std::endl;
    }
  }

  mpi.finalize();
  
  return 0; 
}

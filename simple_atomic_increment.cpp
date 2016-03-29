//
// MPI / InfiniBand Verbs simple atomic increment demo
//
// Run on Sampa cluster with command like:
//   make && srun --label --nodes=2 --ntasks-per-node=3 ./simple_atomic_increment
//

#include "MPIConnection.hpp"
#include "Verbs.hpp"
#include "MemoryRegion.hpp"

#include <cstring>
#include <sys/types.h>
#include <unistd.h>

int main( int argc, char * argv[] ) {

  // set up MPI communication between all processes in job
  MPIConnection mpi( &argc, &argv );

  // set up IBVerbs queue pairs between all processes in job
  Verbs verbs( mpi );

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
  // check atomic increments by initialing an array on each rank with
  // that rank's ID. Then each rank n adjusts the nth location of each
  // other rank so it holds n instead.
  //

  // record if a test failed
  bool pass = true;

  // create space to store data from remote ranks
  //
  // This is created statically with a fixed size so that it's at the
  // same base address on every core.
  //
  // You can avoid this by using MMAP to get memory at a specified
  // address, or by communicating base addresses between ranks.
  static int64_t remote_rank_data[ 1 << 20 ]; // 2^20 endpoints should be enough. :-)
  for( int64_t i = 0; i < mpi.size; ++i ) {
    remote_rank_data[i] = mpi.rank; // initialize array with this rank ID
  }
#ifdef VERBOSE
  std::cout << "Base address of remote_rank_data is " << &remote_rank_data[0] << std::endl;
#endif
    
  // register memory region for this array
  MemoryRegion dest_mr( verbs, &remote_rank_data[0], sizeof(remote_rank_data) );

  // create storage for source data
  int64_t my_data;
  MemoryRegion source_mr( verbs, &my_data, sizeof(my_data) );
  
  // write our rank data to remote ranks, one at a time
  for( int i = 0; i < mpi.size; ++i ) {
    // clear out local storage in preparation for receiving
    // pre-increment data
    my_data = 0;
    
    // point scatter/gather element at source data
    ibv_sge sge;
    std::memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t) source_mr.base();
    sge.length = source_mr.size();
    sge.lkey = source_mr.lkey();

    // create work request for RDMA write
    ibv_send_wr wr;
    std::memset(&wr, 0, sizeof(wr));
    wr.wr_id = i;  // unused here
    wr.next = nullptr; // only one send WR in this linked list
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.imm_data = 0;   // unused here
    wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
    wr.send_flags = IBV_SEND_SIGNALED; // create completion queue entry once this operation has completed
    wr.wr.atomic.remote_addr = (uintptr_t) &remote_rank_data[ mpi.rank ]; // write to this rank's slot of remote array
    wr.wr.atomic.rkey = dest_mr.rkey( i );
    wr.wr.atomic.compare_add = mpi.rank - i; // difference between this rank and remote rank, to be added to remote value
    wr.wr.atomic.swap = 0; // unused here

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

    // check that returned value is correct
    if( my_data != i ) {
      pass = false;
      int64_t expected_value = mpi.rank;
      std::cout << "Rank " << mpi.rank
                << " got bad pre-increment data from rank " << i
                << ": expected " << expected_value
                << ", got " << my_data
                << std::endl;
    }
  }
  
  // wait for everyone to finish all writes
  mpi.barrier();

  // check that values were written in our local array correctly
  for( int64_t i = 0; i < mpi.size; ++i ) {
    int64_t expected_value = i;
    if( expected_value != remote_rank_data[i] ) {
      pass = false;
      std::cout << "Rank " << mpi.rank
                << " got bad data from rank " << i
                << ": expected " << expected_value
                << ", got " << remote_rank_data[i]
                << std::endl;
    }
  }
  
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

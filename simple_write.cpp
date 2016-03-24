//
// MPI / InfiniBand Verbs simple write demo
//
// Run on Sampa cluster with command like:
//   make && srun --label --nodes=2 --ntasks-per-node=3 ./simple_write
//

#include "MPIConnection.hpp"
#include "Verbs.hpp"
#include "MemoryRegion.hpp"
#include "SymmetricAllocator.hpp"

#include <cstring>
#include <sys/types.h>
#include <unistd.h>

int main( int argc, char * argv[] ) {

  // set up MPI communication between all processes in job
  MPIConnection m( &argc, &argv );

  // set up IBVerbs queue pairs between all processes in job
  Verbs v( m );

  // set up symmetric allocator
  SymmetricAllocator s( m );

#ifdef VERBOSE
  std::cout << "hostname " << m.hostname()
            << " MPI rank " << m.rank
            << " ranks " << m.ranks
            << " locale " << m.locale
            << " locales " << m.locales
            << " locale rank " << m.locale_rank
            << " locale ranks " << m.locale_size
            << " pid " << getpid()
            << "\n";
#endif

  //
  // verify writes are working by writing a value from each rank to an
  // array on every rank. Rank n will write the product n*m into the
  // nth location of an array on rank m.
  //
  
  // create space to store data from remote ranks
  //
  // There are at least three options for doing this. I'll illustrate
  // two here.
  //
  // First, we can allocate the data statically with a fixed size so
  // that it's at the same base address on every core, like this:
  
  // static int64_t remote_rank_data[ 1 << 20 ]; // 2^20 endpoints should be enough. :-)
  // MemoryRegion dest_mr( v, &remote_rank_data[0], sizeof(remote_rank_data) );  // register allocated memory

  // Second, we can use my symmetric allocator code from Grappa to
  // dynamically allocate space at the same address on all cores.
  int64_t * remote_rank_data = s.alloc< int64_t >( m.size );
  MemoryRegion dest_mr( v, &remote_rank_data[0], sizeof(int64_t) * m.size ); // register allocated memory

  // The third option would be to allocate memory using normal
  // mechanisms on all cores, without caring what addresses the blocks
  // were allocated at, and then communicate each base addresses to
  // all other cores so that they can form a proper virtual address
  // for whatever core they're writing to. This gets a little ugly, so
  // I'm not doing it here.
  
  // initialize array
  for( int64_t i = 0; i < m.size; ++i ) {
    remote_rank_data[i] = -1;
  }
  
#ifdef VERBOSE
  std::cout << "Base address of remote_rank_data is " << &remote_rank_data[0] << std::endl;
#endif
    

  // create storage for source data
  int64_t my_data;
  MemoryRegion source_mr( v, &my_data, sizeof(my_data) );
  
  // write our rank data to remote ranks, one at a time
  for( int i = 0; i < m.size; ++i ) {
    // set value to write
    // (product of my rank and the remote rank)
    my_data = i * m.rank; 
    
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
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED; // create completion queue entry once this operation has completed
    wr.wr.rdma.remote_addr = (uintptr_t) &remote_rank_data[ m.rank ]; // write to this rank's slot of remote array
    wr.wr.rdma.rkey = dest_mr.rkey( i );

    // hand WR to library/card to send
    v.post_send( i, &wr );

    // wait until WR is complete before continuing.
    //
    // If you don't want to wait, you must ensure that 1) source data
    // is unchanged until the WR has completed, and 2) you don't post
    // WRs too fast for the card.
    while( !v.poll() ) {
      ; // poll until we get a completion queue entry
    }
  }
  
  // wait for everyone to finish all writes
  m.barrier();

  // check that values were written in our local array correctly
  bool pass = true;
  for( int64_t i = 0; i < m.size; ++i ) {
    int64_t expected_value = i * m.rank;
    if( expected_value != remote_rank_data[i] ) {
      pass = false;
      std::cout << "Rank " << m.rank
                << " got bad data from rank " << i
                << ": expected " << expected_value
                << ", got " << remote_rank_data[i]
                << std::endl;
    }
  }
  
  // Use MPI reduction operation to AND together all ranks' "pass" value.
  MPI_CHECK( MPI_Reduce( m.rank == 0 ? MPI_IN_PLACE : &pass, &pass, 1, MPI_C_BOOL,
                         MPI_LAND,  // logical and
                         0,         // destination rank
                         m.main_communicator_ ) );

  // have one rank check the reduced value
  if( 0 == m.rank ){
    if( pass ) {
      std::cout << "PASS: All ranks received correct data." << std::endl;
    } else {
      std::cout << "FAIL: Some rank(s) received incorrect data!" << std::endl;
    }
  }

  return 0; 
}

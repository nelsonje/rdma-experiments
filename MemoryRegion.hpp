#pragma once

#include "Verbs.hpp"

class MemoryRegion {
private:
  Verbs & v;
  std::vector< uint32_t > rkeys;
  ibv_mr * mr;
  
public:
  MemoryRegion( Verbs & v, void * base, size_t size )
    : v(v)
    , rkeys( v.m.size )
    , mr( nullptr )
  {
    mr = v.register_memory_region( base, size );
    if( !mr ) {
      std::cerr << "Memory registration failed at " << base
               << " of size " << size
               << std::endl;
      exit(1);
    }
    
    // TODO: maybe exchange base addresses?

    // exchange rkeys
    MPI_CHECK( MPI_Allgather( &mr->rkey, 1, MPI_UINT32_T,
                              &rkeys[0], 1, MPI_UINT32_T,
                              v.m.main_communicator_ ) );

    // ensure exchange is complete before anybody uses the rkeys
    v.m.barrier();
  }

  ~MemoryRegion() {
    int retval = ibv_dereg_mr( mr );
    if( retval != 0 ) {
      perror( "Memory deregistration failed" );
      exit(1);
    }
  }

  inline void * base() const { return mr->addr; }
  inline void * addr() const { return mr->addr; } // synonym of base()
  inline size_t size() const { return mr->length; }
  inline size_t length() const { return mr->length; } // synonym of size()
  inline uint32_t lkey() const { return mr->lkey; }
  inline uint32_t rkey( int remote_rank ) const { return rkeys[remote_rank]; }
};

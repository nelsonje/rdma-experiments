#pragma once

#include "Verbs.hpp"

class MemoryRegion {
private:
  Verbs & v;
  std::vector< uint32_t > rkeys;
  
public:
  // public to make it easier to use in send WRs
  ibv_mr * mr;
  
  MemoryRegion( Verbs & v, void * base, size_t size )
    : v(v)
    , rkeys( v.m.size )
    , mr( nullptr )
  {
    mr = v.register_memory_region( base, size );

    // TODO: maybe exchange base addresses?

    // exchange rkeys
    MPI_CHECK( MPI_Allgather( &mr->rkey, 1, MPI_UINT32_T,
                              &rkeys[0], 1, MPI_UINT32_T,
                              v.m.main_communicator_ ) );
  }

  ~MemoryRegion() {
  }

  inline void * base() const { return mr->addr; }
  inline void * addr() const { return mr->addr; } // synonym of base()
  inline size_t size() const { return mr->length; }
  inline size_t length() const { return mr->length; } // synonym of size()
  inline uint32_t lkey() const { return mr->lkey; }
  inline uint32_t rkey( int remote_rank ) const { return rkeys[remote_rank]; }
};

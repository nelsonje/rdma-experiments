#pragma once

#include <memory>
#include <infiniband/arch.h>
#include <infiniband/verbs.h>

#include "Verbs.hpp"

namespace verbs {

  class MemoryRegion {
  private:
    void * buf_;
    size_t size_;
    struct ibv_mr * mr_;

    std::unique_ptr< uint32_t[] > rkeys_;
    std::unique_ptr< void*[] > bases_;

    RDMA::Communicator & comm_;
    RDMA::Verbs & ib_;

  public:
    MemoryRegion( RDMA::Communicator & comm, RDMA::Verbs & ib )
      : buf_( NULL )
      , size_( 0 )
      , mr_( NULL )
      , rkeys_()
      , bases_()
      , comm_( comm )
      , ib_( ib )
    { }
    
    MemoryRegion( RDMA::Communicator & comm, RDMA::Verbs & ib, void * buf, const size_t size )
      : buf_( NULL )
      , size_( 0 )
      , mr_( NULL )
      , rkeys_()
      , bases_()
      , comm_( comm )
      , ib_( ib )
    { register_region(buf,size); }
    
    ~MemoryRegion() {
      finalize();
    }
  
    void register_region( void * buf, size_t size );

    void finalize();

    inline void * base() const { return buf_; }
    inline const size_t size() const { return size_; }
    inline const void * base_on_core( const int core ) const { return bases_[core]; }
    inline const int32_t rkey_on_core( const int core ) const { return rkeys_[core]; }
    inline const int32_t lkey() const { return mr_->lkey; }
    inline const int32_t rkey() const { return mr_->rkey; }

    template< typename T >
    inline intptr_t offset_for_core( const int core ) const { return (reinterpret_cast<intptr_t>(bases_[core]) - 
                                                                      reinterpret_cast<intptr_t>(bases_[comm_.rank])) / sizeof(T); }
  };

}

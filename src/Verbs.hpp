
#pragma once

#include <mpi.h>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include <memory>

typedef int16_t Core;

namespace RDMA {

  // global communicator instance
  class Communicator;
  extern Communicator communicator;

  class Communicator {
  public:
    int rank;
    int size;
    int locale_rank;
    int locale_size;
    int locales;
    Communicator(): rank(-1), size(-1), locale_rank(-1), locale_size(-1) {}
    void init( int * argc_p, char ** argv_p[] );
    void finalize();
  };



  // bundle data, scatter-gather element, and work request for locality
  template< typename T >
  struct RDMA_WR {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    T data;
  } __attribute__ ((aligned (64))); // cache-line align these structs



  class Verbs {
    struct ibv_device ** devices;
    int num_devices;
  
    struct ibv_device * device;
    struct ibv_device_attr device_attributes;

    uint8_t port;
    struct ibv_port_attr port_attributes;
  
    struct ibv_context * context;
    struct ibv_pd * protection_domain;

    struct ibv_cq * completion_queue;

    static const int completion_queue_depth = 256;
    static const int send_message_depth = 1; // using depth instead
    static const int receive_message_depth = 1;
    static const int scatter_gather_element_count = 1;
    static const int max_inline_data = 16; // message rate drops from 6M/s to 4M/s at 29 bytes
    static const int max_dest_rd_atomic = 16; // how many outstanding reads/atomic ops are allowed? (remote end of qp, limited by card)
    static const int max_rd_atomic = 16; // how many outstanding reads/atomic ops are allowed? (local end of qp, limited by card)
    static const int min_rnr_timer = 0x12;  // from Mellanox RDMA-Aware Programming manual
    static const int timeout = 0x12;  // from Mellanox RDMA-Aware Programming manual
    static const int retry_count = 6; // from Mellanox RDMA-Aware Programming manual
    static const int rnr_retry = 0; // from Mellanox RDMA-Aware Programming manual

    struct Endpoint {
      uint16_t lid;
      uint32_t qp_num;
      struct ibv_qp * queue_pair;
    };

    std::unique_ptr< Endpoint[] > endpoints;
    // struct ibv_mr * memory_region;
    // void * remote_address;
    // uint32_t remote_key;

    std::unique_ptr< struct ibv_recv_wr[] > bare_receives;

    void initialize_device();

    void connect();

    void finalize_device();

  public:
    Verbs()
      : devices( NULL )
      , num_devices( 0 )
      , device( NULL )
      , device_attributes()
      , port( 0 )
      , port_attributes()
      , context( NULL )
      , protection_domain( NULL )
      , completion_queue( NULL )
    { }

    void init();

    void finalize();

    struct ibv_mr * register_memory_region( void * base, size_t size );

    void post_send( Core c, struct ibv_send_wr * wr );
  
    void post_receive( Core c, struct ibv_recv_wr * wr );

    int poll();
  };



  class RDMASharedMemory {
  private:
    void * buf;
    size_t size_;
    struct ibv_mr * mr;
    std::unique_ptr< uint32_t[] > rkeys;
    Verbs & ib;
  public:
    RDMASharedMemory( Verbs & ib )
      : buf( NULL )
      , size_( 0 )
      , mr( NULL )
      , rkeys()
      , ib( ib )
    { }

    ~RDMASharedMemory() {
      finalize();
    }
  
    void init( size_t newsize = 1L << 30 );

    void finalize();

    inline void * base() const { return buf; }
    inline size_t size() const { return size_; }
    inline int32_t rkey( Core c ) const { return rkeys[c]; }
    inline int32_t lkey() const { return mr->lkey; }
  };



  template< typename F >
  void with_verbs_do( int * argc_p, char ** argv_p[], F f ) {
    Verbs ib;

    google::ParseCommandLineFlags(argc_p, argv_p, true);
    google::InitGoogleLogging( *argv_p[0] );
    FLAGS_logtostderr = true; // TODO: make this work with envvar so command line flag still works

    communicator.init( argc_p, argv_p );
    ib.init();

    f( ib );

    ib.finalize();
    communicator.finalize();
  }

  //


}


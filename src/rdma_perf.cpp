////////////////////////////////////////////////////////////////////////
// This file is part of Grappa, a system for scaling irregular
// applications on commodity clusters. 

// Copyright (C) 2010-2014 University of Washington and Battelle
// Memorial Institute. University of Washington authorizes use of this
// Grappa software.

// Grappa is free software: you can redistribute it and/or modify it
// under the terms of the Affero General Public License as published
// by Affero, Inc., either version 1 of the License, or (at your
// option) any later version.

// Grappa is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Affero General Public License for more details.

// You should have received a copy of the Affero General Public
// License along with this program. If not, you may obtain one from
// http://www.affero.org/oagpl.html.
////////////////////////////////////////////////////////////////////////

//#include <boost/test/unit_test.hpp>
//#include "Grappa.hpp"
// #include "Tasking.hpp"
// #include "FileIO.hpp"
// #include "Array.hpp"
// #include "Cache.hpp"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "common.hpp"

#include <functional>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/join.hpp>

#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/filesystem.hpp>


#include <mpi.h>
#include <xmmintrin.h>


#include <cstdlib>
#include <cstdint>
#include <arpa/inet.h>
#include <sys/mman.h>

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>


#define MPI_CHECK( block )                      \
  do {                                          \
  CHECK( (block) == 0 ) << "MPI call failed";   \
  } while(0)


DEFINE_int64( message_count, 1L << 20 , "Number of messages sent per node" );
DEFINE_int64( batch_size, 100 , "Number of concurrent sent messages" );

DEFINE_string( test, "simple_rdma_write", "Which test should we run?" );


// #if __BYTE_ORDER == __LITTLE_ENDIAN
// static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
// static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
// #elif __BYTE_ORDER == __BIG_ENDIAN
// static inline uint64_t htonll(uint64_t x) { return x; }
// static inline uint64_t ntohll(uint64_t x) { return x; }
// #else
// #error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN #endif
// #endif

typedef int16_t Core;

namespace Grappa {
namespace comm {
int mycore;
int cores;
int locale_mycore;
int locale_cores;
}
}


double start_time = 0.0;
double end_time = 0.0;



class IBComm {
private:
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
  
  
  void initialize_device() {
    // get device list
    CHECK_NOTNULL( devices = ibv_get_device_list( &num_devices ) );

    if ( num_devices > 1 ) {
      LOG(WARNING) << num_devices << "InfiniBand devices detected; using only first one";
    }
    device = devices[0];

    DVLOG(5) << "Found InfiniBand device " << ibv_get_device_name( device )
             << " with guid " << (void*) ntohll( ibv_get_device_guid( device ) );
    
    // open device and get attributes
    CHECK_NOTNULL( context = ibv_open_device( device ) );
    PCHECK( ibv_query_device( context, &device_attributes ) >= 0 );
    LOG(INFO) << "max_qp_rd_atom: " << device_attributes.max_qp_rd_atom
              << " max_res_rd_atom: " << device_attributes.max_res_rd_atom
              << " max_qp_init_rd_atom: " << device_attributes.max_qp_init_rd_atom;
      
    // choose a port (always port 1 for now) and get attributes
    if( device_attributes.phys_port_cnt > 1 ) {
      LOG(WARNING) << device_attributes.phys_port_cnt << " ports detected; using only first one";
    }
    port = 1;
    PCHECK( ibv_query_port( context, port, &port_attributes ) >= 0 );

    
    // create protection domain
    CHECK_NOTNULL( protection_domain = ibv_alloc_pd( context ) );
  }

  void connect() {
    // create shared completion queue
    CHECK_NOTNULL( completion_queue = ibv_create_cq( context,
                                                     completion_queue_depth,
                                                     NULL,  // no user context
                                                     NULL,  // no completion channel 
                                                     0 ) ); // no completion channel vector

    // allocate storage for each endpoint
    endpoints.reset( new Endpoint[ Grappa::comm::cores ] );
    bare_receives.reset( new struct ibv_recv_wr[ Grappa::comm::cores ] );

    // create queue pair for each endpoint
    for( int i = 0; i < Grappa::comm::cores; ++i ) {
      // create queue pair for this endpoint
      struct ibv_qp_init_attr init_attributes;
      memset( &init_attributes, 0, sizeof( struct ibv_qp_init_attr ) );
      init_attributes.send_cq = completion_queue;
      init_attributes.recv_cq = completion_queue;
      init_attributes.qp_type = IBV_QPT_RC; // use "reliable connections"
      init_attributes.sq_sig_all = 0; // only issue send completions if requested
      init_attributes.cap.max_send_wr = FLAGS_batch_size + 1; //send_message_depth;
      init_attributes.cap.max_recv_wr = receive_message_depth;
      init_attributes.cap.max_send_sge = scatter_gather_element_count;
      init_attributes.cap.max_recv_sge = scatter_gather_element_count;
      init_attributes.cap.max_inline_data = max_inline_data;
      CHECK_NOTNULL( endpoints[i].queue_pair = ibv_create_qp( protection_domain, &init_attributes ) );
    }

    // exchange LIDs
    {
      uint16_t lids[ Grappa::comm::cores ];
      MPI_CHECK( MPI_Allgather(  &port_attributes.lid, 1, MPI_INT16_T,
                                 lids, 1, MPI_INT16_T,
                                 MPI_COMM_WORLD ) );
      for( int i = 0; i < Grappa::comm::cores; ++i ) {
        endpoints[i].lid = lids[i];
        DVLOG(5) << "Core " << i << " lid " << lids[i];
      }
    }


    { // exchange queue pair numbers
      uint32_t my_qp_nums[ Grappa::comm::cores ];
      uint32_t remote_qp_nums[ Grappa::comm::cores ];
      
      for( int i = 0; i < Grappa::comm::cores; ++i ) {
        my_qp_nums[i] = endpoints[i].queue_pair->qp_num;
        DVLOG(5) << "Core " << i << " my qp_num " << my_qp_nums[i];
      }
      
      MPI_CHECK( MPI_Alltoall(  my_qp_nums, 1, MPI_INT32_T,
                                remote_qp_nums, 1, MPI_INT32_T,
                                MPI_COMM_WORLD ) );
      for( int i = 0; i < Grappa::comm::cores; ++i ) {
        DVLOG(5) << "Core " << i << " remote qp_num " << remote_qp_nums[i];
        endpoints[i].qp_num = remote_qp_nums[i];
      }
    }

    // Move queues through INIT, RTR, and RTS
    for( int i = 0; i < Grappa::comm::cores; ++i ) {
      struct ibv_qp_attr attributes;

      // move to INIT
      memset(&attributes, 0, sizeof(attributes));
      attributes.qp_state = IBV_QPS_INIT;
      attributes.port_num = port;
      attributes.pkey_index = 0;
      attributes.qp_access_flags = ( IBV_ACCESS_LOCAL_WRITE |
                                     IBV_ACCESS_REMOTE_WRITE |
                                     IBV_ACCESS_REMOTE_READ |
                                     IBV_ACCESS_REMOTE_ATOMIC );
      PCHECK( ibv_modify_qp( endpoints[i].queue_pair, &attributes,
			     IBV_QP_STATE | 
			     IBV_QP_PKEY_INDEX | 
			     IBV_QP_PORT | 
			     IBV_QP_ACCESS_FLAGS ) >= 0);

      // // post an empty receive message to allow us to proceed
      // bare_receives[i].wr_id = 0xdeadbeef;
      // bare_receives[i].next = NULL;
      // bare_receives[i].sg_list = NULL;
      // bare_receives[i].num_sge = 0;
      // post_receive( i, &bare_receives[i] );
                    
      // move to RTR
      memset(&attributes, 0, sizeof(attributes));
      attributes.qp_state = IBV_QPS_RTR;
      attributes.path_mtu = port_attributes.active_mtu;
      attributes.dest_qp_num = endpoints[i].qp_num;
      attributes.rq_psn = 0;
      attributes.max_dest_rd_atomic = max_dest_rd_atomic;
      attributes.min_rnr_timer = min_rnr_timer;
      attributes.ah_attr.is_global = 0;
      attributes.ah_attr.dlid = endpoints[i].lid;
      attributes.ah_attr.sl = 0;
      attributes.ah_attr.src_path_bits = 0;
      attributes.ah_attr.port_num = port;

      PCHECK( ibv_modify_qp( endpoints[i].queue_pair, &attributes, 
                             IBV_QP_STATE | 
                             IBV_QP_AV |
                             IBV_QP_PATH_MTU | 
                             IBV_QP_DEST_QPN | 
                             IBV_QP_RQ_PSN | 
                             IBV_QP_MAX_DEST_RD_ATOMIC | 
                             IBV_QP_MIN_RNR_TIMER ) >= 0 );

      // move to RTS
      memset(&attributes, 0, sizeof(attributes));
      attributes.qp_state = IBV_QPS_RTS;
      attributes.timeout = timeout;
      attributes.retry_cnt = retry_count;
      attributes.rnr_retry = rnr_retry;
      attributes.sq_psn = 0;
      attributes.max_rd_atomic = max_rd_atomic;
      PCHECK( ibv_modify_qp( endpoints[i].queue_pair, &attributes, 
			     IBV_QP_STATE | 
			     IBV_QP_TIMEOUT |
			     IBV_QP_RETRY_CNT | 
			     IBV_QP_RNR_RETRY | 
			     IBV_QP_SQ_PSN | 
			     IBV_QP_MAX_QP_RD_ATOMIC ) >= 0 );
    }

    MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  }

  void finalize_device() {
    for( int i = 0; i < Grappa::comm::cores; ++i ) {
      if( endpoints[i].queue_pair ) {
        PCHECK( ibv_destroy_qp( endpoints[i].queue_pair ) >= 0 );
        endpoints[i].queue_pair = NULL;
      }
    }

    if( completion_queue ) {
      PCHECK( ibv_destroy_cq( completion_queue ) >= 0 );
      completion_queue = NULL;
    }
    
    if( protection_domain ) {
      PCHECK( ibv_dealloc_pd( protection_domain ) >= 0 );
      protection_domain = NULL;
    }
    
    if( context ) {
      PCHECK( ibv_close_device( context ) >= 0 );
      context = NULL;
    }

    if( devices ) {
      ibv_free_device_list( devices );
      devices = NULL;
    }

    if( device ) {
      device = NULL;
    }
  }

public:
  IBComm()
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

  void init() {
    initialize_device();
    connect();
  }

  void finalize() {
    finalize_device();
  }

  struct ibv_mr * register_memory_region( void * base, size_t size ) {
    struct ibv_mr * mr;
    CHECK_NOTNULL( mr = ibv_reg_mr( protection_domain, 
                                    base, size,
                                    ( IBV_ACCESS_LOCAL_WRITE  | 
                                      IBV_ACCESS_REMOTE_WRITE | 
                                      IBV_ACCESS_REMOTE_READ  |
                                      IBV_ACCESS_REMOTE_ATOMIC) ) );
    return mr;
  }

  void post_send( Core c, struct ibv_send_wr * wr ) {
    struct ibv_send_wr * bad_wr = NULL;
    PCHECK( ibv_post_send( endpoints[c].queue_pair, wr, &bad_wr ) >= 0 );
    CHECK_NULL( bad_wr );
  }
  
  void post_receive( Core c, struct ibv_recv_wr * wr ) {
    struct ibv_recv_wr * bad_wr = NULL;
    PCHECK( ibv_post_recv( endpoints[c].queue_pair, wr, &bad_wr ) >= 0 );
    if( bad_wr ) {
      LOG(ERROR) << "Post receive failed at WR " << bad_wr << " (started at " << wr << ")";
    }
    CHECK_NULL( bad_wr );
  }

  int poll() {
    struct ibv_wc wc;
    int retval = ibv_poll_cq( completion_queue, 1, &wc );
    if( retval < 0 ) {
      LOG(ERROR) << "Failed polling completion queue with status " << retval;
    } else if( retval > 0 ) {
      if( wc.status == IBV_WC_SUCCESS ) {
        if( wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM ) {
          LOG(INFO) << "Immediate value is " << (void*) ((int64_t) wc.imm_data);
        }
      } else {
        LOG(ERROR) << "Got completion for " << (void*) wc.wr_id << " with status " << ibv_wc_status_str( wc.status );
      }
    }
    return retval;
  }
};

template< typename T >
struct RDMA_WR {
  struct ibv_sge sge;
  struct ibv_send_wr wr;
  T data;
} __attribute__ ((aligned (64)));

class RDMASharedMemory {
private:
  void * buf;
  size_t size;
  struct ibv_mr * mr;
  std::unique_ptr< uint32_t[] > rkeys;
  IBComm & ib;
public:
  RDMASharedMemory( IBComm & ib )
    : buf( NULL )
    , size( 0 )
    , mr( NULL )
    , rkeys()
    , ib( ib )
  { }

  ~RDMASharedMemory() {
    finalize();
  }
  
  void init( size_t newsize = 1L << 30 ) {
    size = newsize;

    // allocate memory
    void * base =  (void *) 0x0000100000000000L;
    CHECK_NOTNULL( buf = mmap( base, size,
                               PROT_WRITE | PROT_READ,
                               MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED,
                               -1,
                               (off_t) 0 ) );
    CHECK_EQ( base, buf ) << "Mmap at fixed address failed";

    // register memory
    CHECK_NOTNULL( mr = ib.register_memory_region( buf, size ) );

    // distribute rkeys
    MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
    rkeys.reset( new uint32_t[ Grappa::comm::cores ] );
    MPI_CHECK( MPI_Allgather( &mr->rkey, 1, MPI_INT32_T,
                              &rkeys[0], 1, MPI_INT32_T,
                              MPI_COMM_WORLD ) );

  }

  void finalize() {
    if( mr ) {
      PCHECK( ibv_dereg_mr( mr ) >= 0 );
      mr = NULL;
    }
    if( buf ) {
      munmap( buf, size );
      buf = NULL;
      size = 0;
    }
  }

  inline void * base() const { return buf; }
  inline int32_t rkey( Core c ) const { return rkeys[c]; }
  inline int32_t lkey() const { return mr->lkey; }
};


void simple_send_recv_test( IBComm & ib, RDMASharedMemory & shm ) {
  if( Grappa::comm::mycore == 0 ) { LOG(INFO) << "Starting simple send/recv test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;
  
  memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;
  
  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (send_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_SEND;
  
  start_time = MPI_Wtime();
  for( int i = 0; i < FLAGS_message_count; ++i ) {
    *send_data = 1;
    ib.post_receive( Grappa::comm::mycore, recv_wr );
    ib.post_send( Grappa::comm::mycore, send_wr );
    
    int popped = 0;
    while( popped < 2 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}

void simple_rdma_write_test( IBComm & ib, RDMASharedMemory & shm ) {
  if( Grappa::comm::mycore == 0 ) { LOG(INFO) << "Starting simple RDMA write test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;
  
  memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;
  
  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (send_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_RDMA_WRITE;
  send_wr->wr.rdma.remote_addr = (uintptr_t) recv_data;
  send_wr->wr.rdma.rkey = shm.rkey( Grappa::comm::mycore );
  
  start_time = MPI_Wtime();
  for( int i = 0; i < FLAGS_message_count; ++i ) {
    *send_data = 1;
    //ib.post_receive( Grappa::comm::mycore, recv_wr );
    ib.post_send( Grappa::comm::mycore, send_wr );
    
    int popped = 0;
    while( popped < 1 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}

void simple_rdma_write_immediate_test( IBComm & ib, RDMASharedMemory & shm ) {
  if( Grappa::comm::mycore == 0 ) { LOG(INFO) << "Starting simple RDMA write with immediate test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;
  
  memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;
  
  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (send_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->imm_data = 0x12345678;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  send_wr->wr.rdma.remote_addr = (uintptr_t) recv_data;
  send_wr->wr.rdma.rkey = shm.rkey( Grappa::comm::mycore );
  
  start_time = MPI_Wtime();
  for( int i = 0; i < FLAGS_message_count; ++i ) {
    *send_data = 1;
    ib.post_receive( Grappa::comm::mycore, recv_wr );
    ib.post_send( Grappa::comm::mycore, send_wr );
    
    int popped = 0;
    while( popped < 2 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}


void simple_rdma_read_test( IBComm & ib, RDMASharedMemory & shm ) {
  if( Grappa::comm::mycore == 0 ) { LOG(INFO) << "Starting simple RDMA read test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;

  memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;

  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (recv_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_RDMA_READ;
  send_wr->wr.rdma.remote_addr = (uintptr_t) send_data;
  send_wr->wr.rdma.rkey = shm.rkey( Grappa::comm::mycore );
  
  start_time = MPI_Wtime();
  for( int i = 0; i < FLAGS_message_count; ++i ) {
    //ib.post_receive( Grappa::comm::mycore, recv_wr );
    ib.post_send( Grappa::comm::mycore, send_wr );
    
    int popped = 0;
    while( popped < 1 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}

void paired_write_test( IBComm & ib, RDMASharedMemory & shm) {
  Core target = (Grappa::comm::mycore + Grappa::comm::cores / 2) % Grappa::comm::cores;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    memset( &wrs[i], 0 , sizeof(wrs[i]) );
  }

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    wrs[i].data = i;
    
    wrs[i].sge.addr = (uintptr_t) &wrs[i].data;
    wrs[i].sge.length = sizeof(wrs[i].data);
    wrs[i].sge.lkey = shm.lkey();
    
    wrs[i].wr.wr_id = i;
    wrs[i].wr.next = (i == FLAGS_batch_size-1) ? NULL : &wrs[i+1].wr;
    wrs[i].wr.sg_list = &wrs[i].sge;
    wrs[i].wr.num_sge = 1;
    wrs[i].wr.opcode = IBV_WR_RDMA_WRITE;
    wrs[i].wr.send_flags = ( IBV_SEND_INLINE |
                             ((i == FLAGS_batch_size-1) ? IBV_SEND_SIGNALED : 0) );

    wrs[i].wr.wr.rdma.remote_addr = (intptr_t) &vals[0];
    wrs[i].wr.wr.rdma.rkey = shm.rkey(target);
  }

  start_time = MPI_Wtime();
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  // lower half posts to upper half
  if( Grappa::comm::mycore < (Grappa::comm::cores / 2) ) {
    for( int i = 0; i < FLAGS_message_count; i += FLAGS_batch_size ) {
      ib.post_send( target, &wrs[0].wr );

      // wait for sends to complete
      int popped = 0;
      while( popped < 1 ) { //FLAGS_batch_size ) {
        popped += ib.poll();
      }
    }
  }
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  end_time = MPI_Wtime();

  if( Grappa::comm::mycore >= (Grappa::comm::cores / 2) ) {
    CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  }  
}

void paired_write_bypass_test( IBComm & ib, RDMASharedMemory & shm) {
  Core target = (Grappa::comm::mycore + Grappa::comm::cores / 2) % Grappa::comm::cores;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
  
  RDMA_WR<data_t> proto;
  __m128i * src128 = reinterpret_cast< __m128i * >( &proto );

  memset( &proto, 0 , sizeof(proto) );

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    __m128i * dest128 = reinterpret_cast< __m128i * >( &wrs[i] );

    proto.data = i;
    
    proto.sge.addr = (uintptr_t) &wrs[i].data;
    proto.sge.length = sizeof(wrs[i].data);
    proto.sge.lkey = shm.lkey();
    
    proto.wr.wr_id = i;
    proto.wr.next = (i == FLAGS_batch_size-1) ? NULL : &wrs[i+1].wr;
    proto.wr.sg_list = &wrs[i].sge;
    proto.wr.num_sge = 1;
    proto.wr.opcode = IBV_WR_RDMA_WRITE;
    proto.wr.send_flags = ( IBV_SEND_INLINE |
                             ((i == FLAGS_batch_size-1) ? IBV_SEND_SIGNALED : 0) );

    proto.wr.wr.rdma.remote_addr = (intptr_t) &vals[0];
    proto.wr.wr.rdma.rkey = shm.rkey(target);

    // write out buffer
    __builtin_ia32_movntdq( dest128+0, *(src128+0) );
    __builtin_ia32_movntdq( dest128+1, *(src128+1) );
    __builtin_ia32_movntdq( dest128+2, *(src128+2) );
    __builtin_ia32_movntdq( dest128+3, *(src128+3) );
    __builtin_ia32_movntdq( dest128+4, *(src128+4) );
    __builtin_ia32_movntdq( dest128+5, *(src128+5) );
    __builtin_ia32_movntdq( dest128+6, *(src128+6) );
    __builtin_ia32_movntdq( dest128+7, *(src128+7) );
  }

  start_time = MPI_Wtime();
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  // lower half posts to upper half
  if( Grappa::comm::mycore < (Grappa::comm::cores / 2) ) {
    for( int i = 0; i < FLAGS_message_count; i += FLAGS_batch_size ) {
      ib.post_send( target, &wrs[0].wr );

      // wait for sends to complete
      int popped = 0;
      while( popped < 1 ) { //FLAGS_batch_size ) {
        popped += ib.poll();
      }
    }
  }
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  end_time = MPI_Wtime();

  if( Grappa::comm::mycore >= (Grappa::comm::cores / 2) ) {
    CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  }  
}

void paired_zero_write_test( IBComm & ib, RDMASharedMemory & shm) {
  Core target = (Grappa::comm::mycore + Grappa::comm::cores / 2) % Grappa::comm::cores;

  typedef int8_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    memset( &wrs[i], 0 , sizeof(wrs[i]) );
  }

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    wrs[i].data = i;
    
    wrs[i].sge.addr = (uintptr_t) &wrs[i].data;
    wrs[i].sge.length = 0;  // zero-length SGE (fails if actually read
    wrs[i].sge.lkey = shm.lkey();
    
    wrs[i].wr.wr_id = i;
    wrs[i].wr.next = (i == FLAGS_batch_size-1) ? NULL : &wrs[i+1].wr;
    // no sge
    wrs[i].wr.sg_list = NULL;
    wrs[i].wr.num_sge = 0;
    // // zero-length SGE (same as no sge in inline mode)
    // wrs[i].wr.sg_list = &wrs[i].sge;
    // wrs[i].wr.num_sge = 1;
    wrs[i].wr.opcode = IBV_WR_RDMA_WRITE;
    wrs[i].wr.send_flags = ( IBV_SEND_INLINE |
                             ((i == FLAGS_batch_size-1) ? IBV_SEND_SIGNALED : 0) );

    wrs[i].wr.wr.rdma.remote_addr = (intptr_t) &vals[0];
    wrs[i].wr.wr.rdma.rkey = shm.rkey(target);
  }

  start_time = MPI_Wtime();
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  // lower half posts to upper half
  if( Grappa::comm::mycore < (Grappa::comm::cores / 2) ) {
    for( int i = 0; i < FLAGS_message_count; i += FLAGS_batch_size ) {
      ib.post_send( target, &wrs[0].wr );

      // wait for sends to complete
      int popped = 0;
      while( popped < 1 ) { //FLAGS_batch_size ) {
        popped += ib.poll();
      }
    }
  }
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  end_time = MPI_Wtime();
}

void paired_read_test( IBComm & ib, RDMASharedMemory & shm) {
  Core target = (Grappa::comm::mycore + Grappa::comm::cores / 2) % Grappa::comm::cores;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    memset( &wrs[i], 0 , sizeof(wrs[i]) );
  }

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    wrs[i].data = i;
    
    wrs[i].sge.addr = (uintptr_t) &vals[0];
    wrs[i].sge.length = sizeof(wrs[i].data);
    wrs[i].sge.lkey = shm.lkey();
    
    wrs[i].wr.wr_id = i;
    wrs[i].wr.next = (i == FLAGS_batch_size-1) ? NULL : &wrs[i+1].wr;
    wrs[i].wr.sg_list = &wrs[i].sge;
    wrs[i].wr.num_sge = 1;
    wrs[i].wr.opcode = IBV_WR_RDMA_READ;
    wrs[i].wr.send_flags = ( ((i == FLAGS_batch_size-1) ?
                              (IBV_SEND_SIGNALED) : 0) );
    
    wrs[i].wr.wr.rdma.remote_addr = (intptr_t) &wrs[i].data;
    wrs[i].wr.wr.rdma.rkey = shm.rkey(target);
  }

  start_time = MPI_Wtime();
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  // lower half posts to upper half
  if( Grappa::comm::mycore < (Grappa::comm::cores / 2) ) {
    for( int i = 0; i < FLAGS_message_count; i += FLAGS_batch_size ) {
      ib.post_send( target, &wrs[0].wr );

      // wait for sends to complete
      int popped = 0;
      while( popped < 1 ) { //FLAGS_batch_size ) {
        popped += ib.poll();
      }
    }
  }
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  end_time = MPI_Wtime();

  // if( Grappa::comm::mycore >= (Grappa::comm::cores / 2) ) {
  //   CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  // }  
}

void paired_fetchadd_test( IBComm & ib, RDMASharedMemory & shm) {
  Core target = (Grappa::comm::mycore + Grappa::comm::cores / 2) % Grappa::comm::cores;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    memset( &wrs[i], 0 , sizeof(wrs[i]) );
  }

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    wrs[i].data = i;
    
    wrs[i].sge.addr = (uintptr_t) &wrs[i].data;
    wrs[i].sge.length = sizeof(wrs[i].data);
    wrs[i].sge.lkey = shm.lkey();
    
    wrs[i].wr.wr_id = i;
    wrs[i].wr.next = (i == FLAGS_batch_size-1) ? NULL : &wrs[i+1].wr;
    wrs[i].wr.sg_list = &wrs[i].sge;
    wrs[i].wr.num_sge = 1;
    wrs[i].wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
    wrs[i].wr.send_flags = ( ((i == FLAGS_batch_size-1) ?
                              (IBV_SEND_SIGNALED) : 0) );
    
    wrs[i].wr.wr.atomic.remote_addr = (intptr_t) &vals[0];
    wrs[i].wr.wr.atomic.rkey = shm.rkey(target);
    wrs[i].wr.wr.atomic.compare_add = 1;
  }

  start_time = MPI_Wtime();
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  // lower half posts to upper half
  if( Grappa::comm::mycore < (Grappa::comm::cores / 2) ) {
    for( int i = 0; i < FLAGS_message_count; i += FLAGS_batch_size ) {
      ib.post_send( target, &wrs[0].wr );

      // wait for sends to complete
      int popped = 0;
      while( popped < 1 ) { //FLAGS_batch_size ) {
        popped += ib.poll();
      }
    }
  }
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  end_time = MPI_Wtime();

  LOG(INFO) << vals[0];
  // if( Grappa::comm::mycore >= (Grappa::comm::cores / 2) ) {
  //   CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  // }  
}

int main( int argc, char * argv[] ) {
  google::ParseCommandLineFlags( &argc, &argv, true );
  google::InitGoogleLogging( argv[0] );

  MPI_CHECK( MPI_Init( &argc, &argv ) );
  MPI_CHECK( MPI_Comm_rank( MPI_COMM_WORLD, &Grappa::comm::mycore ) );
  MPI_CHECK( MPI_Comm_size( MPI_COMM_WORLD, &Grappa::comm::cores ) );

  // get locale rank/size
  {
    char * locale_rank_string = NULL;
    if( locale_rank_string = getenv("SLURM_LOCALID") ) {
      Grappa::comm::locale_mycore = atoi(locale_rank_string);
      char * locale_size_string = getenv("SLURM_TASKS_PER_NODE");
      CHECK_NOTNULL( locale_size_string );
      // TODO: verify that locale dimensions are the same for all nodes in job
      Grappa::comm::locale_cores = atoi(locale_size_string);
    } else if( locale_rank_string = getenv("OMPI_COMM_WORLD_LOCAL_RANK") ) {
      Grappa::comm::locale_mycore = atoi(locale_rank_string);
      char * locale_size_string = getenv("OMPI_COMM_WORLD_LOCAL_SIZE");
      CHECK_NOTNULL( locale_size_string );
      Grappa::comm::locale_cores = atoi(locale_size_string);
    } else if( locale_rank_string = getenv("MV2_COMM_WORLD_LOCAL_RANK") ){
      Grappa::comm::locale_mycore = atoi(locale_rank_string);
      char * locale_size_string = getenv("MV2_COMM_WORLD_LOCAL_SIZE");
      CHECK_NOTNULL( locale_size_string );
      Grappa::comm::locale_cores = atoi(locale_size_string);
    } else {
      LOG(ERROR) << "Could not determine locale dimensions. Performance numbers will be wrong.";
    }
    DVLOG(5) << "Core " << Grappa::comm::locale_mycore
             << " of " << Grappa::comm::locale_cores
             << " on this node.";
  }

  CHECK_LE( Grappa::comm::cores / Grappa::comm::locale_cores, 2 )
    << "Right now this test probably only makes sense with two nodes";

  int total_cores = Grappa::comm::locale_cores;
  FLAGS_message_count /= total_cores;
    
  
  if( Grappa::comm::mycore == 0 ) LOG(INFO) << "Running.";
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );

  IBComm ib;
  RDMASharedMemory shm( ib );

  ib.init();
  shm.init();

  if( Grappa::comm::mycore == 0 ) {
    LOG(INFO) << "sizeof(wr)=" << sizeof( struct ibv_send_wr )
              << " sizeof(sge)=" << sizeof( struct ibv_sge )
              << " sizeof(RDMA_WR<int64_t>)=" << sizeof( struct RDMA_WR<int64_t> )
              << " sizeof(wc)=" << sizeof( struct ibv_wc );
  }

  double count = FLAGS_message_count * total_cores;

  if( FLAGS_test == "simple_send_recv" ) {
    simple_send_recv_test( ib, shm );
  } else if( FLAGS_test == "simple_rdma_write" ) {
    simple_rdma_write_test( ib, shm );
  } else if( FLAGS_test == "simple_rdma_write_immediate" ) {
    simple_rdma_write_immediate_test( ib, shm );
  } else if( FLAGS_test == "simple_rdma_read" ) {
    simple_rdma_read_test( ib, shm );
  } else if( FLAGS_test == "paired_write" ) {
    paired_write_test( ib, shm );
  } else if( FLAGS_test == "paired_write_bypass" ) {
    paired_write_bypass_test( ib, shm );
  } else if( FLAGS_test == "paired_zero_write" ) {
    paired_zero_write_test( ib, shm );
  } else if( FLAGS_test == "paired_read" ) {
    paired_read_test( ib, shm );
  } else if( FLAGS_test == "paired_fetchadd" ) {
    paired_fetchadd_test( ib, shm );
  } else {
    LOG(ERROR) << "Test " << FLAGS_test << " not found.";
  }

  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );

  if( Grappa::comm::mycore == 0 ) {
    double duration = end_time - start_time;
    //double count = iterations * (Grappa::comm::cores / 2) * FLAGS_message_count;
    double rate = count / duration;
    LOG(INFO) << "Sent " << count << " messages in " << duration << ": " << rate << " Msgs/s";
  }
  
  shm.finalize();
  ib.finalize();
  
  
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  if( Grappa::comm::mycore == 0 ) LOG(INFO) << "Done.";
  return 0;
}


#include "Verbs.hpp"

#include "common.hpp"

#include <cstdlib>
#include <cstdint>
#include <arpa/inet.h>
#include <sys/mman.h>

DEFINE_int64( max_send_wr, 100 , "Number of concurrent Verbs send messages" );

namespace RDMA {

  // global communicator instance
  Communicator communicator;

  void Communicator::init( int * argc_p, char ** argv_p[] ) {
    MPI_CHECK( MPI_Init( argc_p, argv_p ) ); 

    // this will let our error wrapper actually fire.
    MPI_CHECK( MPI_Comm_set_errhandler( MPI_COMM_WORLD, MPI_ERRORS_RETURN ) );

    // initialize job geometry
    MPI_CHECK( MPI_Comm_rank( MPI_COMM_WORLD, &rank ) );
    MPI_CHECK( MPI_Comm_size( MPI_COMM_WORLD, &size ) );

    // try to compute locale geometry
    char * locale_rank_string = NULL;

    // are we using Slurm?
    if( (locale_rank_string = getenv("SLURM_LOCALID")) ) {
      locale_rank = atoi(locale_rank_string);

      char * locale_size_string = getenv("SLURM_TASKS_PER_NODE");
      CHECK_NOTNULL( locale_size_string );
      // TODO: verify that locale dimensions are the same for all nodes in job
      locale_size = atoi(locale_size_string);

      // are we using OpenMPI?
    } else if( (locale_rank_string = getenv("OMPI_COMM_WORLD_LOCAL_RANK")) ) {
      locale_rank = atoi(locale_rank_string);

      char * locale_size_string = getenv("OMPI_COMM_WORLD_LOCAL_SIZE");
      CHECK_NOTNULL( locale_size_string );
      locale_size = atoi(locale_size_string);

      // are we using MVAPICH2?
    } else if( (locale_rank_string = getenv("MV2_COMM_WORLD_LOCAL_RANK")) ){
      locale_rank = atoi(locale_rank_string);

      char * locale_size_string = getenv("MV2_COMM_WORLD_LOCAL_SIZE");
      CHECK_NOTNULL( locale_size_string );
      locale_size = atoi(locale_size_string);

    } else {
      LOG(ERROR) << "Could not determine locale dimensions!";
      exit(1);
    }

    // verify that job has the same number of processes on each node
    int64_t my_locale_cores = locale_size;
    int64_t max_locale_cores = 0;
    MPI_CHECK( MPI_Allreduce( &my_locale_cores, &max_locale_cores, 1,
                              MPI_INT64_T, MPI_MAX, MPI_COMM_WORLD ) );
    CHECK_EQ( my_locale_cores, max_locale_cores )
      << "Number of processes per locale is not the same across job!";


    // Guess at rank-to-locale mapping
    // TODO: verify ranks aren't allocated in a cyclic fashion
    locale_rank = rank / locale_size;

  
    // Guess at number of locales
    // TODO: verify ranks aren't allocated in a cyclic fashion
    locales = size / locale_size;


    VLOG(2) << "Initialized MPI communicator for rank " << rank
            << " of job size " << size;

    MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  }

  void Communicator::finalize() {
    MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
    MPI_CHECK( MPI_Finalize() );
  }




  void Verbs::initialize_device() {
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
    DVLOG(5) << "max_qp_rd_atom: " << device_attributes.max_qp_rd_atom
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

  void Verbs::connect() {
    // create shared completion queue
    CHECK_NOTNULL( completion_queue = ibv_create_cq( context,
                                                     completion_queue_depth,
                                                     NULL,  // no user context
                                                     NULL,  // no completion channel 
                                                     0 ) ); // no completion channel vector

    // allocate storage for each endpoint
    endpoints.reset( new Endpoint[ communicator.size ] );
    bare_receives.reset( new struct ibv_recv_wr[ communicator.size ] );

    // create queue pair for each endpoint
    for( int i = 0; i < communicator.size; ++i ) {
      // create queue pair for this endpoint
      struct ibv_qp_init_attr init_attributes;
      memset( &init_attributes, 0, sizeof( struct ibv_qp_init_attr ) );
      init_attributes.send_cq = completion_queue;
      init_attributes.recv_cq = completion_queue;
      init_attributes.qp_type = IBV_QPT_RC; // use "reliable connections"
      init_attributes.sq_sig_all = 0; // only issue send completions if requested
      init_attributes.cap.max_send_wr = FLAGS_max_send_wr + 1; //send_message_depth;
      init_attributes.cap.max_recv_wr = receive_message_depth;
      init_attributes.cap.max_send_sge = scatter_gather_element_count;
      init_attributes.cap.max_recv_sge = scatter_gather_element_count;
      init_attributes.cap.max_inline_data = max_inline_data;
      CHECK_NOTNULL( endpoints[i].queue_pair = ibv_create_qp( protection_domain, &init_attributes ) );
    }

    // exchange LIDs
    {
      uint16_t lids[ communicator.size ];
      MPI_CHECK( MPI_Allgather(  &port_attributes.lid, 1, MPI_INT16_T,
                                 lids, 1, MPI_INT16_T,
                                 MPI_COMM_WORLD ) );
      for( int i = 0; i < communicator.size; ++i ) {
        endpoints[i].lid = lids[i];
        DVLOG(5) << "Core " << i << " lid " << lids[i];
      }
    }


    { // exchange queue pair numbers
      uint32_t my_qp_nums[ communicator.size ];
      uint32_t remote_qp_nums[ communicator.size ];
      
      for( int i = 0; i < communicator.size; ++i ) {
        my_qp_nums[i] = endpoints[i].queue_pair->qp_num;
        DVLOG(5) << "Core " << i << " my qp_num " << my_qp_nums[i];
      }
      
      MPI_CHECK( MPI_Alltoall(  my_qp_nums, 1, MPI_INT32_T,
                                remote_qp_nums, 1, MPI_INT32_T,
                                MPI_COMM_WORLD ) );
      for( int i = 0; i < communicator.size; ++i ) {
        DVLOG(5) << "Core " << i << " remote qp_num " << remote_qp_nums[i];
        endpoints[i].qp_num = remote_qp_nums[i];
      }
    }

    // Move queues through INIT, RTR, and RTS
    for( int i = 0; i < communicator.size; ++i ) {
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


  void Verbs::finalize_device() {
    for( int i = 0; i < communicator.size; ++i ) {
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

  void Verbs::init() {
    initialize_device();
    connect();
  }

  void Verbs::finalize() {
    finalize_device();
  }

  struct ibv_mr * Verbs::register_memory_region( void * base, size_t size ) {
    struct ibv_mr * mr;
    CHECK_NOTNULL( mr = ibv_reg_mr( protection_domain, 
                                    base, size,
                                    ( IBV_ACCESS_LOCAL_WRITE  | 
                                      IBV_ACCESS_REMOTE_WRITE | 
                                      IBV_ACCESS_REMOTE_READ  |
                                      IBV_ACCESS_REMOTE_ATOMIC) ) );
    return mr;
  }

  void Verbs::post_send( Core c, struct ibv_send_wr * wr ) {
    struct ibv_send_wr * bad_wr = NULL;
    PCHECK( ibv_post_send( endpoints[c].queue_pair, wr, &bad_wr ) >= 0 );
    CHECK_NULL( bad_wr );
  }

  void Verbs::post_receive( Core c, struct ibv_recv_wr * wr ) {
    struct ibv_recv_wr * bad_wr = NULL;
    PCHECK( ibv_post_recv( endpoints[c].queue_pair, wr, &bad_wr ) >= 0 );
    if( bad_wr ) {
      LOG(ERROR) << "Post receive failed at WR " << bad_wr << " (started at " << wr << ")";
    }
    CHECK_NULL( bad_wr );
  }

  int Verbs::poll() {
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

  void RDMASharedMemory::init( size_t newsize ) {
    size_ = newsize;

    // allocate memory at fixed address
    void * base =  (void *) 0x0000100000000000L;
    CHECK_NOTNULL( buf = mmap( base, size_,
                               PROT_WRITE | PROT_READ,
                               MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED,
                               -1,
                               (off_t) 0 ) );
    CHECK_EQ( base, buf ) << "Mmap at fixed address failed";

    // register memory
    CHECK_NOTNULL( mr = ib.register_memory_region( buf, size_ ) );

    // distribute rkeys
    MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
    rkeys.reset( new uint32_t[ communicator.size ] );
    MPI_CHECK( MPI_Allgather( &mr->rkey, 1, MPI_INT32_T,
                              &rkeys[0], 1, MPI_INT32_T,
                              MPI_COMM_WORLD ) );

  }

  void RDMASharedMemory::finalize() {
    if( mr ) {
      PCHECK( ibv_dereg_mr( mr ) >= 0 );
      mr = NULL;
    }
    if( buf ) {
      munmap( buf, size_ );
      buf = NULL;
      size_ = 0;
    }
  }

}


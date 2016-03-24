#include "Verbs.hpp"

#include <cstring>

/// Discover local Verbs-capable devices; choose one and prepare it for use.
void Verbs::initialize_device( const std::string desired_device_name, const int8_t desired_port ) {
  // get device list
  devices = ibv_get_device_list( &num_devices );
  if( !devices ) {
    std::cerr << "Didn't find any Verbs-capable devices!";
    exit(1);
  }

#ifdef VERBOSE
  // if more than one device, warn that we're choosing the named one.
  if ( num_devices > 1 ) {
    std::cout << num_devices << " Verbs devices detected; searching for " << desired_device_name << std::endl;
  }
#endif
  
  // search for device
  for( int i = 0; i < num_devices; ++i ) {
#ifdef VERBOSE
    std::cout << "Found Verbs device " << ibv_get_device_name( devices[i] )
              << " with guid " << (void*) ntohll( ibv_get_device_guid( devices[i] ) )
              << std::endl;
#endif
    if( (num_devices == 1) || (desired_device_name == ibv_get_device_name(devices[i])) ) {
      // choose this device
      device = devices[i];
      device_name = ibv_get_device_name( device );
      device_guid = ntohll( ibv_get_device_guid( device ) );
    }
  }

  // ensure we found a device
  if( !device ) {
    std::cerr << "Didn't find device " << desired_device_name << "\n";
    exit(1);
  }
    
  // open device context and get device attributes
  context = ibv_open_device( device );
  if( !context ) {
    std::cerr << "Failed to get context for device " << device_name << "\n";
    exit(1);
  }
  int retval = ibv_query_device( context, &device_attributes );
  if( retval < 0 ) {
    perror( "Error getting device attributes" );
    exit(1);
  }
  
  // choose a port on the device and get port attributes
#ifdef VERBOSE
  if( device_attributes.phys_port_cnt > 1 ) {
    std::cout << (int) device_attributes.phys_port_cnt << " ports detected; using port " << (int) desired_port << std::endl;
  }
#endif
  if( device_attributes.phys_port_cnt < desired_port ) {
    std::cerr << "expected " << (int) desired_port << " ports, but found " << (int) device_attributes.phys_port_cnt;
    exit(1);
  }
  port = desired_port;
  retval = ibv_query_port( context, port, &port_attributes );
  if( retval < 0 ) {
    perror( "Error getting port attributes" );
    exit(1);
  }
  
  // create protection domain
  protection_domain = ibv_alloc_pd( context );
  if( !protection_domain ) {
    std::cerr << "Error getting protection domain!\n";
    exit(1);
  }
}

/// release resources on device in preparation for shutting down
void Verbs::finalize_device() {
  for( auto endpoint : endpoints ) {
    if( endpoint.queue_pair ) {
      int retval = ibv_destroy_qp( endpoint.queue_pair );
      if( retval < 0 ) {
        perror( "Error destroying queue pair" );
        exit(1);
      }
      endpoint.queue_pair = nullptr;
    }
    endpoints.clear();
  }

  if( completion_queue ) {
    int retval = ibv_destroy_cq( completion_queue );
    if( retval < 0 ) {
      perror( "Error destroying completion queue" );
      exit(1);
    }
    completion_queue = nullptr;
  }
    
  if( protection_domain ) {
    int retval = ibv_dealloc_pd( protection_domain );
    if( retval < 0 ) {
      perror( "Error deallocating protection domain" );
      exit(1);
    }
    protection_domain = nullptr;
  }
  
  if( context ) {
    int retval = ibv_close_device( context );
    if( retval < 0 ) {
      perror( "Error closing device context" );
      exit(1);
    }
    context = nullptr;
  }
  
  if( devices ) {
    ibv_free_device_list( devices );
    devices = nullptr;
  }
  
  if( device ) {
    device = nullptr;
  }
}

/// set up queue pairs for RDMA operations
void Verbs::connect_queue_pairs() {
  // create shared completion queue
  completion_queue = ibv_create_cq( context,
                                    completion_queue_depth,
                                    NULL,  // no user context
                                    NULL,  // no completion channel 
                                    0 );   // no completion channel vector
  if( !completion_queue ) {
    std::cerr << "Error creating completion queue!\n";
    exit(1);
  }


  // allocate storage for each endpoint in job
  endpoints.resize( m.size );

  // create queue pair for each endpoint
  for( int i = 0; i < m.size; ++i ) {
    // create queue pair for this endpoint
    ibv_qp_init_attr init_attributes;
    std::memset( &init_attributes, 0, sizeof( ibv_qp_init_attr ) );

    // use shared completion queue
    init_attributes.send_cq = completion_queue;
    init_attributes.recv_cq = completion_queue;

    // use "reliable connected" model in order to support RDMA atomics
    init_attributes.qp_type = IBV_QPT_RC;

    // only issue send completions if requested
    init_attributes.sq_sig_all = 0;

    // set queue depths and WR parameters accoring to constants declared earlier
    init_attributes.cap.max_send_wr = send_queue_depth;
    init_attributes.cap.max_recv_wr = receive_queue_depth;
    init_attributes.cap.max_send_sge = scatter_gather_element_count;
    init_attributes.cap.max_recv_sge = scatter_gather_element_count;
    init_attributes.cap.max_inline_data = max_inline_data;

    // create queue pair
    endpoints[i].queue_pair = ibv_create_qp( protection_domain, &init_attributes );
    if( ! endpoints[i].queue_pair ) {
      std::cerr << "Error creating queue pair on rank " << m.rank << " for rank " << i << "!\n";
      exit(1);
    }
  }

  // wait until all ranks have created their queue pairs
  m.barrier();
  
  { // exchange LIDs (addresses) between all ranks
    std::vector< uint16_t > lids( m.size );
    MPI_CHECK( MPI_Allgather(  &port_attributes.lid, 1, MPI_UINT16_T,
                               &lids[0], 1, MPI_UINT16_T,
                               m.main_communicator_ ) );
    for( int i = 0; i < m.size; ++i ) {
      endpoints[i].lid = lids[i];
    }
  }


  { // exchange queue pair numbers
    // first, prepare contiguous list of my QP numbers
    std::vector< uint32_t > my_qp_nums( m.size );
    for( int i = 0; i < m.size; ++i ) {
      my_qp_nums[i] = endpoints[i].queue_pair->qp_num;
    }

    // now, gather list of remote QP numbers
    std::vector< uint32_t > remote_qp_nums( m.size );
    MPI_CHECK( MPI_Alltoall(  &my_qp_nums[0], 1, MPI_UINT32_T,
                              &remote_qp_nums[0], 1, MPI_UINT32_T,
                              m.main_communicator_ ) );
    for( int i = 0; i < m.size; ++i ) {
      endpoints[i].qp_num = remote_qp_nums[i];
    }
  }

  // once everybody's done with that, we can go connect each one of our queues
  m.barrier();

  // Connect all our queue pairs: move queues through INIT, RTR, and RTS
  for( int i = 0; i < m.size; ++i ) {
    ibv_qp_attr attributes;
    std::memset(&attributes, 0, sizeof(attributes));

    // move to INIT
    attributes.qp_state = IBV_QPS_INIT;
    attributes.port_num = port;
    attributes.pkey_index = 0;
    attributes.qp_access_flags = ( IBV_ACCESS_LOCAL_WRITE |
                                   IBV_ACCESS_REMOTE_WRITE |
                                   IBV_ACCESS_REMOTE_READ |
                                   IBV_ACCESS_REMOTE_ATOMIC );
    int retval = ibv_modify_qp( endpoints[i].queue_pair, &attributes,
                                IBV_QP_STATE | 
                                IBV_QP_PKEY_INDEX | 
                                IBV_QP_PORT | 
                                IBV_QP_ACCESS_FLAGS );
    if( retval < 0 ) {
      perror( "Error setting queue pair to INIT" );
      exit(1);
    }

    /// in theory, we need to post an empty receive WR to proceed, but
    /// when we're doing RDMA-only stuff it seems to work without one.
    // bare_receives[i].wr_id = 0xdeadbeef;
    // bare_receives[i].next = NULL;
    // bare_receives[i].sg_list = NULL;
    // bare_receives[i].num_sge = 0;
    // post_receive( i, &bare_receives[i] );
                    
    // move to RTR
    std::memset(&attributes, 0, sizeof(attributes));
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
    retval = ibv_modify_qp( endpoints[i].queue_pair, &attributes, 
                            IBV_QP_STATE | 
                            IBV_QP_AV |
                            IBV_QP_PATH_MTU | 
                            IBV_QP_DEST_QPN | 
                            IBV_QP_RQ_PSN | 
                            IBV_QP_MAX_DEST_RD_ATOMIC | 
                            IBV_QP_MIN_RNR_TIMER );
    if( retval < 0 ) {
      perror( "Error setting queue pair to RTR" );
      exit(1);
    }

    // move to RTS
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.qp_state = IBV_QPS_RTS;
    attributes.timeout = timeout;
    attributes.retry_cnt = retry_count;
    attributes.rnr_retry = rnr_retry;
    attributes.sq_psn = 0;
    attributes.max_rd_atomic = max_rd_atomic;
    retval = ibv_modify_qp( endpoints[i].queue_pair, &attributes, 
                            IBV_QP_STATE | 
                            IBV_QP_TIMEOUT |
                            IBV_QP_RETRY_CNT | 
                            IBV_QP_RNR_RETRY | 
                            IBV_QP_SQ_PSN | 
                            IBV_QP_MAX_QP_RD_ATOMIC );
    if( retval < 0 ) {
      perror( "Error setting queue pair to RTR" );
      exit(1);
    }
  }

  // wait for everybody to have connected everything before proceeding
  m.barrier();
}

/// Register a region of memory with Verbs library
ibv_mr * Verbs::register_memory_region( void * base, size_t size ) {
  ibv_mr * mr;
  mr = ibv_reg_mr( protection_domain, 
                   base, size,
                   ( IBV_ACCESS_LOCAL_WRITE  | 
                     IBV_ACCESS_REMOTE_WRITE | 
                     IBV_ACCESS_REMOTE_READ  |
                     IBV_ACCESS_REMOTE_ATOMIC) );
  if( !mr ) {
    std::cerr << "Error registring memory region at " << base << " of " << size << " bytes!\n";
    exit(1);
  }
  
  return mr;
}

/// post a receive request for a remote rank
void Verbs::post_receive( int remote_rank, ibv_recv_wr * wr ) {
  ibv_recv_wr * bad_wr = nullptr;

  int retval = ibv_post_recv( endpoints[remote_rank].queue_pair, wr, &bad_wr );
  if( retval < 0 ) {
    perror( "Error posting receive WR" );
    exit(1);
  }
  
  if( bad_wr ) {
    std::cerr << "Error posting receive WR at WR " << bad_wr << " (first WR in list was " << wr << ")";
    exit(1);
  }
}

/// post a send request to a remote rank
void Verbs::post_send( int remote_rank, ibv_send_wr * wr ) {
  ibv_send_wr * bad_wr = nullptr;
  
  int retval = ibv_post_send( endpoints[remote_rank].queue_pair, wr, &bad_wr );
  if( retval < 0 ) {
    perror( "Error posting receive WR" );
    exit(1);
  }
  
  if( bad_wr ) {
    std::cerr << "Error posting send WR at WR " << bad_wr << " (first WR in list was " << wr << ")";
    exit(1);
  }
}

/// consume up to max_entries completion queue entries. Returns number
/// of entries consumed.
int Verbs::poll( int max_entries ) {
  struct ibv_wc wc;
  int retval = ibv_poll_cq( completion_queue, max_entries, &wc );
  if( retval < 0 ) {
    std::cerr << "Failed polling completion queue with status " << retval << "\n";
    exit(1);
  } else if( retval > 0 ) {
    if( wc.status == IBV_WC_SUCCESS ) {
      if( wc.opcode == IBV_WC_RDMA_WRITE ) {
#ifdef VERBOSE
        std::cout << "Got completion for WR ID " << wc.wr_id << std::endl;
#endif
      } else if( wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM ) {
#ifdef VERBOSE
        std::cout << "Got completion for WR ID " << wc.wr_id << " with immediate value " << (void*) ((int64_t) wc.imm_data) << std::endl;
#endif
      } else {
#ifdef VERBOSE
        std::cout << "Got completion for something with id " << ((int64_t) wc.wr_id) << std::endl;
#endif
      }
    } else {
      std::cerr << "Got completion for " << (void*) wc.wr_id << " with status " << ibv_wc_status_str( wc.status ) << "\n";
      exit(1);
    }
  }
  return retval;
}


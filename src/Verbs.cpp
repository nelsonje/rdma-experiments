
#include "Verbs.hpp"

#include "common.hpp"

#include <limits>
#include <cstdlib>
#include <cstdint>
#include <arpa/inet.h>
#include <sys/mman.h>

DEFINE_int64( max_send_wr, 16, "Number of concurrent Verbs send messages" );
DEFINE_string( ib_device, "mlx4_0", "Name of InfiniBand device to use" );
DEFINE_int64( ib_port, 1, "Which port of InfiniBand device to use" );
DEFINE_int64( concurrent_sends, 8, "How many concurrent sends are allowed?" );

namespace RDMA {

  // global communicator instance
  Communicator communicator;

  void Communicator::init( int * argc_p, char ** argv_p[] ) {
    MPI_CHECK( MPI_Init( argc_p, argv_p ) ); 

    // get locale-local communicator
    MPI_CHECK( MPI_Comm_split_type( MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &locale_comm ) );
    MPI_CHECK( MPI_Comm_rank( locale_comm, &locale_rank ) );
    MPI_CHECK( MPI_Comm_size( locale_comm, &locale_size ) );

  // get locale count
    int32_t localesint = locale_rank == 0; // count one per locale and broadcast
    MPI_CHECK( MPI_Allreduce( MPI_IN_PLACE, &localesint, 1, MPI_INT32_T,
                              MPI_SUM, MPI_COMM_WORLD ) );
    locales = localesint;

    // get my locale
    int32_t mylocaleint = locale_rank == 0;  // count one per locale and sum
    MPI_CHECK( MPI_Scan( MPI_IN_PLACE, &mylocaleint, 1, MPI_INT32_T,
                         MPI_SUM, MPI_COMM_WORLD ) );
    // copy to all cores in locale
    MPI_CHECK( MPI_Bcast( &mylocaleint, 1, MPI_INT32_T,
                          0, locale_comm ) );
    mylocaleint -= 1; // make zero-indexed
    locale = mylocaleint;
    
    // make new communicator with ranks laid out like we want
    MPI_CHECK( MPI_Comm_split( MPI_COMM_WORLD, 0, mylocaleint, &grappa_comm ) );
    int grappa_mycoreint = -1;
    int grappa_coresint = -1;
    MPI_CHECK( MPI_Comm_rank( grappa_comm, &grappa_mycoreint ) );
    MPI_CHECK( MPI_Comm_size( grappa_comm, &grappa_coresint ) );
    rank = grappa_mycoreint;
    size = grappa_coresint;
    
    // allocate and initialize core-to-locale translation
    locale_of_rank.reset( new int32_t[ size ] );
    MPI_CHECK( MPI_Allgather( &locale, 1, MPI_INT32_T,
                              &locale_of_rank[0], 1, MPI_INT32_T,
                              grappa_comm ) );
    
  
    // verify locale numbering is consistent with locales
    int32_t localemin = std::numeric_limits<int32_t>::max();
    int32_t localemax = std::numeric_limits<int32_t>::min();
    MPI_CHECK( MPI_Reduce( &mylocaleint, &localemin, 1, MPI_INT32_T,
                           MPI_MIN, 0, locale_comm ) );
    MPI_CHECK( MPI_Reduce( &mylocaleint, &localemax, 1, MPI_INT32_T,
                           MPI_MAX, 0, locale_comm ) );
    if( 0 == locale_rank ) {
      CHECK_EQ( localemin, localemax ) << "Locale ID is not consistent across locale!";
    }

    // verify locale core count is the same across job
    int32_t locale_coresmin = std::numeric_limits<int32_t>::max();
    int32_t locale_coresmax = std::numeric_limits<int32_t>::min();
    MPI_CHECK( MPI_Reduce( &locale_size, &locale_coresmin, 1, MPI_INT32_T,
                           MPI_MIN, 0, grappa_comm ) );
    MPI_CHECK( MPI_Reduce( &locale_size, &locale_coresmax, 1, MPI_INT32_T,
                           MPI_MAX, 0, grappa_comm ) );
    if( 0 == grappa_mycoreint ) {
      CHECK_EQ( locale_coresmin, locale_coresmax ) << "Number of cores per locale is not the same across job!";
    }

  
    DVLOG(2) << "hostname " << hostname()
             << " mycore_ " << rank
             << " cores_ " << size
             << " mylocale_ " << locale
             << " locales_ " << locales
             << " locale_mycore_ " << locale_rank
             << " locale_cores_ " << locale_size
             << " pid " << getpid();
    
    // this will let our error wrapper actually fire.
    MPI_CHECK( MPI_Comm_set_errhandler( locale_comm, MPI_ERRORS_RETURN ) );
    MPI_CHECK( MPI_Comm_set_errhandler( grappa_comm, MPI_ERRORS_RETURN ) );

    barrier();

    VLOG(2) << "Initialized MPI communicator for rank " << rank
            << " of job size " << size;

    barrier();
  }

  void Communicator::finalize() {
    barrier();
    MPI_CHECK( MPI_Finalize() );
  }

  void Communicator::barrier() const {
    MPI_CHECK( MPI_Barrier( grappa_comm ) );
  }


  const char * Communicator::hostname() const {
    static char name[ MPI_MAX_PROCESSOR_NAME ] = {0};
    static int name_size = 0;
    if( '\0' == name[0] ) {
      MPI_CHECK( MPI_Get_processor_name( &name[0], &name_size ) );
    }
    return &name[0];
  }

  void Verbs::initialize_device() {
    device = NULL;

    // get device list
    CHECK_NOTNULL( devices = ibv_get_device_list( &num_devices ) );

    device = NULL;
    std::string desired_device_name = "mlx4_0";

    if ( num_devices > 1 ) {
      LOG(WARNING) << num_devices << " InfiniBand devices detected; using " << FLAGS_ib_device;
    }
    //device = devices[0];

    for( int i = 0; i < num_devices; ++i ) {
      DVLOG(5) << "Found InfiniBand device " << ibv_get_device_name( devices[i] )
               << " with guid " << (void*) ntohll( ibv_get_device_guid( devices[i] ) );
      if( FLAGS_ib_device == ibv_get_device_name( devices[i] ) ) {
        device = devices[i];
      }
    }
    
    if( device == NULL ) {
      LOG(ERROR) << "Didn't find device " << FLAGS_ib_device;
      exit(1);
    }
    
    // open device and get attributes
    CHECK_NOTNULL( context = ibv_open_device( device ) );
    PCHECK( ibv_query_device( context, &device_attributes ) >= 0 );
    DVLOG(5) << "max_qp_rd_atom: " << device_attributes.max_qp_rd_atom
             << " max_res_rd_atom: " << device_attributes.max_res_rd_atom
             << " max_qp_init_rd_atom: " << device_attributes.max_qp_init_rd_atom;
      
    // choose a port and get attributes
    if( device_attributes.phys_port_cnt > 1 ) {
      LOG(WARNING) << (int) device_attributes.phys_port_cnt << " ports detected; using port " << FLAGS_ib_port;
    }
    if( device_attributes.phys_port_cnt < FLAGS_ib_port ) {
      LOG(ERROR) << "expected " << FLAGS_ib_port << " ports, but found " << (int) device_attributes.phys_port_cnt;
      exit(1);
    }
    PCHECK( ibv_query_port( context, FLAGS_ib_port, &port_attributes ) >= 0 );

    
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
                                 communicator.grappa_comm ) );
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
                                communicator.grappa_comm ) );
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
      attributes.port_num = FLAGS_ib_port;
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
      attributes.ah_attr.port_num = FLAGS_ib_port;

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

    communicator.barrier();
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
    int retval = 0;
    int backoff_s = 1;
    //poll();
    ++outstanding;
    wr->wr_id = ++last_outstanding;
    if(FLAGS_concurrent_sends == last_outstanding) {
      poll();
      wr->send_flags = IBV_SEND_SIGNALED;
      VLOG(3) << "Enquing coalesced send for " << last_outstanding << " of " << outstanding << " ops (" << wr->wr_id << ")";
      last_outstanding = 0;
    }
    PCHECK( (retval = ibv_post_send( endpoints[c].queue_pair, wr, &bad_wr )) >= 0 );
    while( bad_wr != NULL ) {
      LOG(WARNING) << "Work queue returned bad_wr = " << (void*) bad_wr
                   << "; waiting " << backoff_s << " second(s) to retry with " << outstanding << " entries.";
      sleep(backoff_s);
        
      poll();
      bad_wr = NULL;
      PCHECK( ibv_post_send( endpoints[c].queue_pair, wr, &bad_wr ) >= 0 );
      if( bad_wr == NULL ) outstanding++;
      
      backoff_s <<= 1;
    }
        
    ///////return bad_wr == NULL;
    //LOG(INFO) << "retval == " << retval << " bad_wr == " << bad_wr;
    //CHECK_NULL( bad_wr );
    // while( bad_wr != NULL ) {
    //   LOG(WARNING) << "Argh";
    //   poll();
    //   sleep(10);
    //   bad_wr = NULL;
    //   PCHECK( ibv_post_send( endpoints[c].queue_pair, wr, &bad_wr ) >= 0 );
    // }
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
        if( wc.opcode == IBV_WC_RDMA_WRITE ) {
          VLOG(3) << "got coalesced completion for " << wc.wr_id << " ops";
          outstanding -= -wc.wr_id;
        } else if( wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM ) {
          VLOG(3) << "Immediate value is " << (void*) ((int64_t) wc.imm_data);
        } else {
          VLOG(3) << "Got completion for something with id " << ((int64_t) wc.wr_id);
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
    void * base = NULL; // (void *) 0x0000123400000000L;
    // CHECK_NOTNULL( buf = mmap( base, size_,
    //                            PROT_WRITE | PROT_READ,
    //                            MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED,
    //                            -1,
    //                            (off_t) 0 ) );
    PCHECK( (buf = mmap( base, size_,
                         PROT_WRITE | PROT_READ,
                         MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED,
                         -1,
                         (off_t) 0 )) > 0 );
    //PCHECK( buf == 0 );
    //PCHECK( buf == ((void*) -1) );
    CHECK_EQ( base, buf ) << "Mmap of size " << size_ << " at fixed address failed";
    flag = true;
    attach( buf, newsize );
  }

  void RDMASharedMemory::attach( void * newbuf, size_t size ) {
    size_ = size;
    buf = newbuf;
    DVLOG(2) << "Registering memory region of " << size << " bytes at " << buf;

    // register memory
    CHECK_NOTNULL( mr = ib.register_memory_region( buf, size_ ) );

    // distribute rkeys
    communicator.barrier();
    rkeys.reset( new uint32_t[ communicator.size ] );
    MPI_CHECK( MPI_Allgather( &mr->rkey, 1, MPI_INT32_T,
                              &rkeys[0], 1, MPI_INT32_T,
                              communicator.grappa_comm ) );

  }

  void RDMASharedMemory::finalize() {
    if( mr ) {
      PCHECK( ibv_dereg_mr( mr ) >= 0 );
      mr = NULL;
    }
    if( flag && buf ) {
      munmap( buf, size_ );
      buf = NULL;
      size_ = 0;
    }
  }

}


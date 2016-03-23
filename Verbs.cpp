#include "Verbs.hpp"

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
    std::cout << num_devices << " Verbs devices detected; searching for " << desired_device_name << "\n";
  }
#endif
  
  // search for device
  for( int i = 0; i < num_devices; ++i ) {
#ifdef VERBOSE
    std::cout << "Found Verbs device " << ibv_get_device_name( devices[i] )
              << " with guid " << (void*) ntohll( ibv_get_device_guid( devices[i] ) )
              << "\n";
#endif
    if( (num_devices > 1) && (desired_device_name != ibv_get_device_name(devices[i])) ) {
      // if we are choosing between multiple devices and this one's name doesn't match, skip it.
      continue;
    }

    device = devices[i];
    device_name = ibv_get_device_name( device );
    device_guid = ntohll( ibv_get_device_guid( device ) );
  }

  // ensure we found a device
  if( device == NULL ) {
    std::cerr << "Didn't find device " << desired_device_name << "\n";
    exit(1);
  }
    
  // open device and get device attributes
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
    std::cout << (int) device_attributes.phys_port_cnt << " ports detected; using port " << (int) desired_port << "\n";
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

void Verbs::finalize_device() {
  // for( int i = 0; i < communicator.size; ++i ) {
  //   if( endpoints[i].queue_pair ) {
  //     PCHECK( ibv_destroy_qp( endpoints[i].queue_pair ) >= 0 );
  //     endpoints[i].queue_pair = NULL;
  //   }
  // }

  // if( completion_queue ) {
  //   PCHECK( ibv_destroy_cq( completion_queue ) >= 0 );
  //   completion_queue = NULL;
  // }
    
  if( protection_domain ) {
    int retval = ibv_dealloc_pd( protection_domain );
    if( retval < 0 ) {
      perror( "Error deallocating protection domain" );
      exit(1);
    }
    protection_domain = NULL;
  }
  
  if( context ) {
    int retval = ibv_close_device( context );
    if( retval < 0 ) {
      perror( "Error closing device context" );
      exit(1);
    }
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


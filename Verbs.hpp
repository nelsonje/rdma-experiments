#pragma once

#include "MPIConnection.hpp"

#include <infiniband/arch.h>
#include <infiniband/verbs.h>

#include <memory>

class Verbs {
  MPIConnection & m;

  // list of Verbs-capable devices
  ibv_device ** devices;
  int num_devices;

  ibv_device * device;
  const char * device_name;
  uint64_t device_guid;
  ibv_device_attr device_attributes;

  uint8_t port;
  ibv_port_attr port_attributes;
  
  ibv_context * context;
  ibv_pd * protection_domain;

  ibv_cq * completion_queue;
  int outstanding;
  int last_outstanding;

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
    ibv_qp * queue_pair;
  };

  std::unique_ptr< Endpoint[] > endpoints;

  void initialize_device( const std::string desired_device_name, const int8_t desired_port );


  void connect();
  
  void finalize_device();
  
public:
  Verbs( MPIConnection & m )
    : m(m)
    , devices( nullptr )
    , num_devices( 0 )
    , device( nullptr )
    , device_attributes()
    , port( 0 )
    , port_attributes()
    , context( nullptr )
    , protection_domain( nullptr )
    , completion_queue( nullptr )
    , outstanding(0)
    , last_outstanding(0)
  { }
  
  void init( const std::string desired_device_name = "mlx4_0", const int8_t desired_port = 1 ) {
    initialize_device( desired_device_name, desired_port );
    //connect();
  }
  
  void finalize() {
    finalize_device();
  }
  
  struct ibv_mr * register_memory_region( void * base, size_t size );
  
  struct ibv_pd * get_protection_domain() const { return protection_domain; }
  
  int poll();

};

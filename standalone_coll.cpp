//
// MPI / InfiniBand Verbs simple read demo
//
// Run on Sampa cluster with command like:
//   make && srun --label --nodes=2 --ntasks-per-node=3 ./simple_read
//

#include <cstring>
#include <sys/types.h>
#include <unistd.h>

#include <infiniband/arch.h>
#include <infiniband/verbs.h>

#include <string>
#include <vector>
#include <iostream>

#define VERBS_CHECK( verbs_call )                                       \
  do {                                                                  \
    int retval;                                                         \
    if( (retval = (verbs_call)) < 0 ) {                                 \
      std::string errstr = "Verbs call failed: " #verbs_call;           \
        perror( errstr.c_str() );                                       \
        exit(1);                                                        \
    }                                                                   \
  } while(0)

int main( int argc, char * argv[] ) {

  const std::string desired_device_name = "mlx4_0";
  const int8_t desired_port = 1;

  /// list of Verbs-capable devices
  ibv_device ** devices;
  int num_devices;

  /// info about chosen device
  ibv_device * device;
  const char * device_name;
  uint64_t device_guid;
  ibv_device_attr device_attributes;
  ibv_exp_device_attr exp_device_attributes;

  /// info about chosen port
  uint8_t port;
  ibv_port_attr port_attributes;

  /// device context, used for most Verbs operations
  ibv_context * context;

  /// protection domain to go with context
  ibv_pd * protection_domain;

  /// constants for initializing queues
  static const int completion_queue_depth = 256;
  static const int send_queue_depth    = 16;         // how many operations per queue should we be able to enqueue at a time?
  static const int receive_queue_depth = 1;          // only need 1 if we're just using RDMA ops
  static const int scatter_gather_element_count = 1; // how many SGE's do we allow per operation?
  static const int max_inline_data = 16;             // message rate drops from 6M/s to 4M/s at 29 bytes
  static const int max_dest_rd_atomic = 16;          // how many outstanding reads/atomic ops are allowed? (remote end of qp, limited by card)
  static const int max_rd_atomic = 16;               // how many outstanding reads/atomic ops are allowed? (local end of qp, limited by card)
  static const int min_rnr_timer = 0x12;             // from Mellanox RDMA-Aware Programming manual; probably don't need to touch
  static const int timeout = 0x12;                   // from Mellanox RDMA-Aware Programming manual; probably don't need to touch
  static const int retry_count = 6;                  // from Mellanox RDMA-Aware Programming manual; probably don't need to touch
  static const int rnr_retry = 0;                    // from Mellanox RDMA-Aware Programming manual; probably don't need to touch

  /// completion queue, shared across all endpoints/queue pairs
  ibv_cq * completion_queue;


  //
  // init
  //
  // get device list
  devices = ibv_get_device_list( &num_devices );
  if( !devices ) {
    std::cerr << "Didn't find any Verbs-capable devices!";
    exit(1);
  }
  
  // if more than one device, warn that we're choosing the named one.
  if ( num_devices > 1 ) {
    std::cout << num_devices << " Verbs devices detected; searching for " << desired_device_name << std::endl;
  }
  
  // search for device
  for( int i = 0; i < num_devices; ++i ) {
    std::cout << "Found Verbs device " << ibv_get_device_name( devices[i] )
              << " with guid " << (void*) ntohll( ibv_get_device_guid( devices[i] ) )
              << std::endl;
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
  
  retval = ibv_exp_query_device( context, &exp_device_attributes );
  if( retval < 0 ) {
    perror( "Error getting experimental device attributes" );
    exit(1);
  }
  std::cout << "Experimental attributes:"
            << "\n   fw_ver: " << exp_device_attributes.fw_ver
            << "\n   node_guid: " << (void*) ntohll( exp_device_attributes.node_guid )
            << "\n   sys_image_guid: " << (void*) ntohll( exp_device_attributes.sys_image_guid )
            << "\n   max_mr_size: " << (void*) exp_device_attributes.max_mr_size
            << "\n   page_size_cap: " << exp_device_attributes.page_size_cap
            << "\n   vendor_id: " << exp_device_attributes.vendor_id
            << "\n   vendor_part_id: " << exp_device_attributes.vendor_part_id
            << "\n   hw_ver: " << exp_device_attributes.hw_ver
            << "\n   max_qp: " << exp_device_attributes.max_qp
            << "\n   max_qp_wr: " << exp_device_attributes.max_qp_wr
            << "\n   reserved: " << exp_device_attributes.reserved
            << "\n   max_sge: " << exp_device_attributes.max_sge
            << "\n   max_sge_rd: " << exp_device_attributes.max_sge_rd
            << "\n   max_cq: " << exp_device_attributes.max_cq
            << "\n   max_cqe: " << exp_device_attributes.max_cqe
            << "\n   max_mr: " << exp_device_attributes.max_mr
            << "\n   max_pd: " << exp_device_attributes.max_pd
            << "\n   max_qp_rd_atom: " << exp_device_attributes.max_qp_rd_atom
            << "\n   max_ee_rd_atom: " << exp_device_attributes.max_ee_rd_atom
            << "\n   max_res_rd_atom: " << exp_device_attributes.max_res_rd_atom
            << "\n   max_qp_init_rd_atom: " << exp_device_attributes.max_qp_init_rd_atom
            << "\n   max_ee_init_rd_atom: " << exp_device_attributes.max_ee_init_rd_atom

    //enum ibv_exp_atomic_cap	exp_atomic_cap;
            << "\n   exp_atomic_cap: " << exp_device_attributes.exp_atomic_cap
            << "\n   exp_atomic_cap == IBV_EXP_ATOMIC_NONE: " << (exp_device_attributes.exp_atomic_cap == IBV_EXP_ATOMIC_NONE)
            << "\n   exp_atomic_cap == IBV_EXP_ATOMIC_HCA: " << (exp_device_attributes.exp_atomic_cap == IBV_EXP_ATOMIC_HCA)
            << "\n   exp_atomic_cap == IBV_EXP_ATOMIC_GLOB: " << (exp_device_attributes.exp_atomic_cap == IBV_EXP_ATOMIC_GLOB)
            << "\n   exp_atomic_cap == IBV_EXP_ATOMIC_HCA_REPLY_BE: " << (exp_device_attributes.exp_atomic_cap == IBV_EXP_ATOMIC_HCA_REPLY_BE)

            << "\n   max_ee: " << exp_device_attributes.max_ee
            << "\n   max_rdd: " << exp_device_attributes.max_rdd
            << "\n   max_mw: " << exp_device_attributes.max_mw
            << "\n   max_raw_ipv6_qp: " << exp_device_attributes.max_raw_ipv6_qp
            << "\n   max_raw_ethy_qp: " << exp_device_attributes.max_raw_ethy_qp
            << "\n   max_mcast_grp: " << exp_device_attributes.max_mcast_grp
            << "\n   max_mcast_qp_attach: " << exp_device_attributes.max_mcast_qp_attach
            << "\n   max_total_mcast_qp_attach: " << exp_device_attributes.max_total_mcast_qp_attach
            << "\n   max_ah: " << exp_device_attributes.max_ah
            << "\n   max_fmr: " << exp_device_attributes.max_fmr
            << "\n   max_map_per_fmr: " << exp_device_attributes.max_map_per_fmr
            << "\n   max_srq: " << exp_device_attributes.max_srq
            << "\n   max_srq_wr: " << exp_device_attributes.max_srq_wr
            << "\n   max_srq_sge: " << exp_device_attributes.max_srq_sge
            << "\n   max_pkeys: " << exp_device_attributes.max_pkeys
            << "\n   local_ca_ack_delay: " << exp_device_attributes.local_ca_ack_delay
            << "\n   phys_port_cnt: " << exp_device_attributes.phys_port_cnt
            << "\n   comp_mask: " << (void*)exp_device_attributes.comp_mask
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_CALC_CAP: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_CALC_CAP)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_WITH_TIMESTAMP_MASK: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_WITH_TIMESTAMP_MASK)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_WITH_HCA_CORE_CLOCK: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_WITH_HCA_CORE_CLOCK)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS)
            << "\n   comp_mask & IBV_EXP_DEVICE_DC_RD_REQ: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_DC_RD_REQ)
            << "\n   comp_mask & IBV_EXP_DEVICE_DC_RD_RES: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_DC_RD_RES)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_EXT_ATOMIC_ARGS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_EXT_ATOMIC_ARGS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_UMR: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_UMR)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_ODP: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_ODP)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DCT: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DCT)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_MAX_CTX_RES_DOMAIN: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_MAX_CTX_RES_DOMAIN)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_RX_HASH: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_RX_HASH)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_MAX_WQ_TYPE_RQ: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_MAX_WQ_TYPE_RQ)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DEVICE_CTX: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DEVICE_CTX)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_MP_RQ: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_MP_RQ)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_VLAN_OFFLOADS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_VLAN_OFFLOADS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_EC_CAPS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_EC_CAPS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_MASKED_ATOMICS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_MASKED_ATOMICS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_RX_PAD_END_ALIGN: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_RX_PAD_END_ALIGN)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_TSO_CAPS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_TSO_CAPS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_PACKET_PACING_CAPS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_PACKET_PACING_CAPS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_EC_GF_BASE: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_EC_GF_BASE)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_OOO_CAPS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_OOO_CAPS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_SW_PARSING_CAPS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_SW_PARSING_CAPS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_ODP_MAX_SIZE: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_ODP_MAX_SIZE)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_TM_CAPS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_TM_CAPS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_TUNNEL_OFFLOADS_CAPS: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_TUNNEL_OFFLOADS_CAPS)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE)
            << "\n   comp_mask & IBV_EXP_DEVICE_ATTR_RESERVED: " << !!(exp_device_attributes.comp_mask & IBV_EXP_DEVICE_ATTR_RESERVED)

    // struct ibv_exp_device_calc_cap calc_cap;
            << "\n   calc_cap.data_types: " << exp_device_attributes.calc_cap.data_types
            << "\n   calc_cap.data_sizes: " << exp_device_attributes.calc_cap.data_sizes
            << "\n   calc_cap.int_ops: " << exp_device_attributes.calc_cap.int_ops
            << "\n   calc_cap.uint_ops: " << exp_device_attributes.calc_cap.uint_ops
            << "\n   calc_cap.fp_ops: " << exp_device_attributes.calc_cap.fp_ops

            << "\n   timestamp_mask: " << (void*)exp_device_attributes.timestamp_mask
            << "\n   hca_core_clock: " << exp_device_attributes.hca_core_clock
            << "\n   exp_device_cap_flags: " << (void*)exp_device_attributes.exp_device_cap_flags

            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_RESIZE_MAX_WR: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_RESIZE_MAX_WR)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_BAD_PKEY_CNTR: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_BAD_PKEY_CNTR)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_BAD_QKEY_CNTR: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_BAD_QKEY_CNTR)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_RAW_MULTI: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_RAW_MULTI)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_AUTO_PATH_MIG: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_AUTO_PATH_MIG)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_CHANGE_PHY_PORT: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_CHANGE_PHY_PORT)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_UD_AV_PORT_ENFORCE: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_UD_AV_PORT_ENFORCE)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_CURR_QP_STATE_MOD: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_CURR_QP_STATE_MOD)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_SHUTDOWN_PORT: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_SHUTDOWN_PORT)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_INIT_TYPE: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_INIT_TYPE)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_PORT_ACTIVE_EVENT: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_PORT_ACTIVE_EVENT)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_SYS_IMAGE_GUID: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_SYS_IMAGE_GUID)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_RC_RNR_NAK_GEN: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_RC_RNR_NAK_GEN)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_SRQ_RESIZE: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_SRQ_RESIZE)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_N_NOTIFY_CQ: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_N_NOTIFY_CQ)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_XRC: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_XRC)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_DC_TRANSPORT: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_DC_TRANSPORT)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_QPG: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_QPG)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_UD_RSS: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_UD_RSS)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_UD_TSS: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_UD_TSS)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_EXT_ATOMICS: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_EXT_ATOMICS)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_NOP: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_NOP)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_UMR: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_UMR)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_ODP: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_ODP)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_VXLAN_SUPPORT: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_VXLAN_SUPPORT)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_RX_CSUM_TCP_UDP_PKT: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_RX_CSUM_TCP_UDP_PKT)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_RX_CSUM_IP_PKT: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_RX_CSUM_IP_PKT)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_EC_OFFLOAD: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_EC_OFFLOAD)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_EXT_MASKED_ATOMICS: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_EXT_MASKED_ATOMICS)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_RX_TCP_UDP_PKT_TYPE: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_RX_TCP_UDP_PKT_TYPE)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_SCATTER_FCS: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_SCATTER_FCS)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_MEM_WINDOW: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_MEM_WINDOW)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_WQ_DELAY_DROP: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_WQ_DELAY_DROP)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_PHYSICAL_RANGE_MR: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_PHYSICAL_RANGE_MR)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_MEM_MGT_EXTENSIONS: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_MEM_MGT_EXTENSIONS)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_DC_INFO: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_DC_INFO)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_MW_TYPE_2A: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_MW_TYPE_2A)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_MW_TYPE_2B: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_MW_TYPE_2B)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_CROSS_CHANNEL: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_CROSS_CHANNEL)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_MANAGED_FLOW_STEERING: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_MANAGED_FLOW_STEERING)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_MR_ALLOCATE: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_MR_ALLOCATE)
            << "\n   exp_device_cap_flags & IBV_EXP_DEVICE_SHARED_MR: " << !!(exp_device_attributes.exp_device_cap_flags & IBV_EXP_DEVICE_SHARED_MR)

            << "\n   max_dc_req_rd_atom: " << exp_device_attributes.max_dc_req_rd_atom
            << "\n   max_dc_res_rd_atom: " << exp_device_attributes.max_dc_res_rd_atom
            << "\n   inline_recv_sz: " << exp_device_attributes.inline_recv_sz
            << "\n   max_rss_tbl_sz: " << exp_device_attributes.max_rss_tbl_sz

    //struct ibv_exp_ext_atomics_params ext_atom;
            << "\n   ext_atom.log_atomic_arg_sizes: " << (void*)exp_device_attributes.ext_atom.log_atomic_arg_sizes
            << "\n   ext_atom.max_fa_bit_boundary: " << exp_device_attributes.ext_atom.max_fa_bit_boundary
            << "\n   ext_atom.log_max_atomic_inline: " << exp_device_attributes.ext_atom.log_max_atomic_inline

    //struct ibv_exp_umr_caps umr_caps;
            << "\n   umr_caps.max_klm_list_size: " << exp_device_attributes.umr_caps.max_klm_list_size
            << "\n   umr_caps.max_send_wqe_inline_klms: " << exp_device_attributes.umr_caps.max_send_wqe_inline_klms
            << "\n   umr_caps.max_umr_recursion_depth: " << exp_device_attributes.umr_caps.max_umr_recursion_depth
            << "\n   umr_caps.max_umr_stride_dimension: " << exp_device_attributes.umr_caps.max_umr_stride_dimension

    //struct ibv_exp_odp_caps	odp_caps;
            << "\n   odp_caps.general_odp_caps: " << exp_device_attributes.odp_caps.general_odp_caps
            << "\n   odp_caps.per_transport_caps.rc_odp_caps: " << exp_device_attributes.odp_caps.per_transport_caps.rc_odp_caps
            << "\n   odp_caps.per_transport_caps.uc_odp_caps: " << exp_device_attributes.odp_caps.per_transport_caps.uc_odp_caps
            << "\n   odp_caps.per_transport_caps.ud_odp_caps: " << exp_device_attributes.odp_caps.per_transport_caps.ud_odp_caps
            << "\n   odp_caps.per_transport_caps.dc_odp_caps: " << exp_device_attributes.odp_caps.per_transport_caps.dc_odp_caps
            << "\n   odp_caps.per_transport_caps.xrc_odp_caps: " << exp_device_attributes.odp_caps.per_transport_caps.xrc_odp_caps
            << "\n   odp_caps.per_transport_caps.raw_eth_odp_caps: " << exp_device_attributes.odp_caps.per_transport_caps.raw_eth_odp_caps

            << "\n   max_dct: " << exp_device_attributes.max_dct
            << "\n   max_ctx_res_domain: " << exp_device_attributes.max_ctx_res_domain

    //struct ibv_exp_rx_hash_caps	rx_hash_caps;
            << "\n   rx_hash_caps.max_rwq_indirection_tables: " << exp_device_attributes.rx_hash_caps.max_rwq_indirection_tables
            << "\n   rx_hash_caps.max_rwq_indirection_table_size: " << exp_device_attributes.rx_hash_caps.max_rwq_indirection_table_size
            << "\n   rx_hash_caps.supported_hash_functions: " << exp_device_attributes.rx_hash_caps.supported_hash_functions
            << "\n   rx_hash_caps.supported_packet_fields: " << exp_device_attributes.rx_hash_caps.supported_packet_fields
            << "\n   rx_hash_caps.supported_qps: " << exp_device_attributes.rx_hash_caps.supported_qps

            << "\n   max_wq_type_rq: " << exp_device_attributes.max_wq_type_rq
            << "\n   max_device_ctx: " << exp_device_attributes.max_device_ctx

    //struct ibv_exp_mp_rq_caps	mp_rq_caps;
            << "\n   mp_rq_caps.supported_qps: " << exp_device_attributes.mp_rq_caps.supported_qps
            << "\n   mp_rq_caps.allowed_shifts: " << exp_device_attributes.mp_rq_caps.allowed_shifts
            << "\n   mp_rq_caps.min_single_wqe_log_num_of_strides: " << exp_device_attributes.mp_rq_caps.min_single_wqe_log_num_of_strides
            << "\n   mp_rq_caps.max_single_wqe_log_num_of_strides: " << exp_device_attributes.mp_rq_caps.max_single_wqe_log_num_of_strides
            << "\n   mp_rq_caps.min_single_stride_log_num_of_bytes: " << exp_device_attributes.mp_rq_caps.min_single_stride_log_num_of_bytes
            << "\n   mp_rq_caps.max_single_stride_log_num_of_bytes: " << exp_device_attributes.mp_rq_caps.max_single_stride_log_num_of_bytes

            << "\n   wq_vlan_offloads_cap: " << exp_device_attributes.wq_vlan_offloads_cap

    //struct ibv_exp_ec_caps         ec_caps;
            << "\n   ec_caps.max_ec_data_vector_count: " << exp_device_attributes.ec_caps.max_ec_data_vector_count
            << "\n   ec_caps.max_ec_calc_inflight_calcs: " << exp_device_attributes.ec_caps.max_ec_calc_inflight_calcs

    //struct ibv_exp_masked_atomic_params masked_atomic;
            << "\n   masked_atomic.max_fa_bit_boundary: " << exp_device_attributes.masked_atomic.max_fa_bit_boundary
            << "\n   masked_atomic.log_max_atomic_inline: " << exp_device_attributes.masked_atomic.log_max_atomic_inline
            << "\n   masked_atomic.masked_log_atomic_arg_sizes: " << exp_device_attributes.masked_atomic.masked_log_atomic_arg_sizes
            << "\n   masked_atomic.masked_log_atomic_arg_sizes_network_endianness: " << exp_device_attributes.masked_atomic.masked_log_atomic_arg_sizes_network_endianness

        /*
         * The alignment of the padding end address.
         * When RX end of packet padding is enabled the device will pad the end
         * of RX packet up until the next address which is aligned to the
         * rx_pad_end_addr_align size.
         * Expected size for this field is according to system cache line size,
         * for example 64 or 128. When field is 0 padding is not supported.
         */
            << "\n   rx_pad_end_addr_align: " << exp_device_attributes.rx_pad_end_addr_align

    //struct ibv_exp_tso_caps	tso_caps;
                << "\n   tso_caps.max_tso: " << exp_device_attributes.tso_caps.max_tso
                << "\n   tso_caps.supported_qpts: " << exp_device_attributes.tso_caps.supported_qpts

    //struct ibv_exp_packet_pacing_caps packet_pacing_caps;
            << "\n   packet_pacing_caps.qp_rate_limit_min: " << exp_device_attributes.packet_pacing_caps.qp_rate_limit_min
            << "\n   packet_pacing_caps.qp_rate_limit_max: " << exp_device_attributes.packet_pacing_caps.qp_rate_limit_max
            << "\n   packet_pacing_caps.supported_qpts: " << exp_device_attributes.packet_pacing_caps.supported_qpts
            << "\n   packet_pacing_caps.reserved: " << exp_device_attributes.packet_pacing_caps.reserved

            << "\n   ec_w_mask: " << exp_device_attributes.ec_w_mask

    //struct ibv_exp_ooo_caps ooo_caps;
            << "\n   ooo_caps.rc_caps: " << exp_device_attributes.ooo_caps.rc_caps
            << "\n   ooo_caps.xrc_caps: " << exp_device_attributes.ooo_caps.xrc_caps
            << "\n   ooo_caps.dc_caps: " << exp_device_attributes.ooo_caps.dc_caps
            << "\n   ooo_caps.ud_caps: " << exp_device_attributes.ooo_caps.ud_caps

    //struct ibv_exp_sw_parsing_caps sw_parsing_caps;
            << "\n   sw_parsing_caps.sw_parsing_offloads: " << exp_device_attributes.sw_parsing_caps.sw_parsing_offloads
            << "\n   sw_parsing_caps.supported_qpts: " << exp_device_attributes.sw_parsing_caps.supported_qpts

            << "\n   odp_mr_max_size: " << exp_device_attributes.odp_mr_max_size

    //struct ibv_exp_tm_caps  tm_caps;
            << "\n   tm_caps.max_rndv_hdr_size: " << exp_device_attributes.tm_caps.max_rndv_hdr_size
            << "\n   tm_caps.max_num_tags: " << exp_device_attributes.tm_caps.max_num_tags
            << "\n   tm_caps.capability_flags: " << (void*)exp_device_attributes.tm_caps.capability_flags
            << "\n   tm_caps.capability_flags & IBV_EXP_TM_CAP_RC: " << !!(exp_device_attributes.tm_caps.capability_flags & IBV_EXP_TM_CAP_RC)
            << "\n   tm_caps.capability_flags & IBV_EXP_TM_CAP_DC: " << !!(exp_device_attributes.tm_caps.capability_flags & IBV_EXP_TM_CAP_DC)
            << "\n   tm_caps.max_ops: " << exp_device_attributes.tm_caps.max_ops
            << "\n   tm_caps.max_sge: " << exp_device_attributes.tm_caps.max_sge

            << "\n   tunnel_offloads_caps: " << exp_device_attributes.tunnel_offloads_caps
            << "\n   max_dm_size: " << exp_device_attributes.max_dm_size
            << std::endl;



  // choose a port on the device and get port attributes
  if( device_attributes.phys_port_cnt > 1 ) {
    std::cout << (int) device_attributes.phys_port_cnt << " ports detected; using port " << (int) desired_port << std::endl;
  }
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


  std::cout << "Done.\n";


















//   // set up MPI communication between all processes in job
//   MPIConnection mpi( &argc, &argv );

//   // set up IBVerbs queue pairs between all processes in job
//   Verbs verbs( mpi );

// #ifdef VERBOSE
//   std::cout << "hostname " << mpi.hostname()
//             << " MPI rank " << mpi.rank
//             << " ranks " << mpi.ranks
//             << " locale " << mpi.locale
//             << " locales " << mpi.locales
//             << " locale rank " << mpi.locale_rank
//             << " locale ranks " << mpi.locale_size
//             << " pid " << getpid()
//             << "\n";
// #endif

//   static int64_t remote_rank_data[ 1 << 20 ]; // 2^20 endpoints should be enough. :-)
//   SymmetricMemoryRegion source_mr( verbs, &remote_rank_data[0], sizeof(remote_rank_data) );  // register allocated memory

  
//   // initialize array
//   for( int64_t i = 0; i < mpi.size; ++i ) {
//     remote_rank_data[i] = i * mpi.rank;
//   }

//   // ensure initialization is done before anybody reads anything
//   mpi.barrier();
  
// #ifdef VERBOSE
//   std::cout << "Base address of remote_rank_data is " << &remote_rank_data[0] << std::endl;
// #endif
    

//   // create storage for destination data
//   int64_t my_data;

//   // we can use the SymmetricMemoryRegion wrapper since all the cores
//   // are executing this right now; since this is for local access
//   // only, we'll just ignore the rkeys it exchanges.
//   //SymmetricMemoryRegion dest_mr( verbs, &my_data, sizeof(my_data) );

//   // alternatively, you could ignore my SymmetricMemoryRegion wrapper
//   // and call the Verbs memory region registration wrapper instead
//   ibv_mr * dest_mr_p = verbs.register_memory_region( &my_data, sizeof(my_data) );
  
//   // assume that we'll get all values correctly; check as we receive them in the loop below.
//   bool pass = true;

//   // write our rank data to remote ranks, one at a time
//   for( int i = 0; i < mpi.size; ++i ) {
//     // reset destination location to wrong value to detect error
//     my_data = -1;
    
//     // point scatter/gather element at destination data
//     ibv_sge sge;
//     std::memset(&sge, 0, sizeof(sge));

//     // code that goes with SymmetricMemoryRegion above
//     //sge.addr = (uintptr_t) dest_mr.base();
//     //sge.length = dest_mr.size();
//     //sge.lkey = dest_mr.lkey();
    
//     // code that goes with ibv_mr allocation instead of SymmetricMemoryRegion above
//     sge.addr = (uintptr_t) &my_data;
//     sge.length = sizeof(my_data);
//     sge.lkey = dest_mr_p->lkey; // local memory region key

//     // create work request for RDMA read
//     ibv_send_wr wr;
//     std::memset(&wr, 0, sizeof(wr));
//     wr.wr_id = i;  // unused here
//     wr.next = nullptr; // only one send WR in this linked list
//     wr.sg_list = &sge;
//     wr.num_sge = 1;
//     wr.imm_data = 0;   // unused here
//     wr.opcode = IBV_WR_RDMA_READ;
//     wr.send_flags = IBV_SEND_SIGNALED; // create completion queue entry once this operation has completed
//     wr.wr.rdma.remote_addr = (uintptr_t) &remote_rank_data[ mpi.rank ]; // read from this rank's slot of remote array
//     wr.wr.rdma.rkey = source_mr.rkey( i );

//     // hand WR to library/card to send
//     verbs.post_send( i, &wr );

//     // wait until WR is complete before continuing.
//     //
//     // If you don't want to wait, you must ensure that 1) source data
//     // is unchanged until the WR has completed, and 2) you don't post
//     // WRs too fast for the card.
//     while( !verbs.poll() ) {
//       ; // poll until we get a completion queue entry
//     }

//     // check that we read the right value
//     int64_t expected_value = i * mpi.rank;
//     if( expected_value != my_data ) {
//       pass = false;
//       std::cout << "Rank " << mpi.rank
//                 << " got bad data from rank " << i
//                 << ": expected " << expected_value
//                 << ", got " << remote_rank_data[i]
//                 << std::endl;
//     }
//   }
  
//   // wait for everyone to finish all reads
//   mpi.barrier();
  
//   // Use MPI reduction operation to AND together all ranks' "pass" value.
//   bool overall_pass = false;
//   MPI_CHECK( MPI_Reduce( &pass, &overall_pass, 1, MPI_C_BOOL,
//                          MPI_LAND,  // logical and
//                          0,         // destination rank
//                          mpi.main_communicator_ ) );

//   // have one rank check the reduced value
//   if( 0 == mpi.rank ){
//     if( overall_pass ) {
//       std::cout << "PASS: All ranks received correct data." << std::endl;
//     } else {
//       std::cout << "FAIL: Some rank(s) received incorrect data!" << std::endl;
//     }
//   }

//   mpi.finalize();
  
  return 0; 
}

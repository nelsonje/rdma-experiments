
#include "Verbs.hpp"
#include "common.hpp"
#include <cstring>

#include <xmmintrin.h>

using namespace RDMA;

DEFINE_int64( sizeA, 1L << 20 , "Size of GUPS increment array" );
DEFINE_int64( sizeB, 1L << 20 , "Total number of messages sent" );
DEFINE_int64( batch_size, 64, "Number of concurrent sent messages" );
DEFINE_int64( dest_batch_size, 8, "Number of concurrent sent messages per node for gups" );
DEFINE_int64( seed, 1 , "Seed for random addresses" );

DEFINE_string( mode, "gups", "Which mode should we run in?" );


double start_time = 0.0;
double end_time = 0.0;

int64_t message_count_per_core = 0;
int64_t sizeA_per_core = 0;

const uint64_t lcgM = 6364136223846793005UL;
const uint64_t lcgB = 1442695040888963407UL;
uint64_t random_number = 0;

uint64_t next_random_number() {
  random_number = lcgM * random_number + lcgB;
  return random_number;
}

void simple_send_recv_test( Verbs & ib, RDMASharedMemory & shm ) {
  if( communicator.rank == 0 ) { LOG(INFO) << "Starting simple send/recv test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;
  
  std::memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  std::memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;
  
  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  std::memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (send_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  std::memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_SEND;
  
  start_time = MPI_Wtime();
  for( int i = 0; i < message_count_per_core; ++i ) {
    *send_data = 1;
    ib.post_receive( communicator.rank, recv_wr );
    ib.post_send( communicator.rank, send_wr );
    
    int popped = 0;
    while( popped < 2 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}

void simple_rdma_write_test( Verbs & ib, RDMASharedMemory & shm ) {
  if( communicator.rank == 0 ) { LOG(INFO) << "Starting simple RDMA write test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;
  
  std::memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  std::memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;
  
  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  std::memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (send_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  std::memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_RDMA_WRITE;
  send_wr->wr.rdma.remote_addr = (uintptr_t) recv_data;
  send_wr->wr.rdma.rkey = shm.rkey( communicator.rank );
  
  start_time = MPI_Wtime();
  for( int i = 0; i < message_count_per_core; ++i ) {
    *send_data = 1;
    //ib.post_receive( communicator.rank, recv_wr );
    ib.post_send( communicator.rank, send_wr );
    
    int popped = 0;
    while( popped < 1 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}

void simple_rdma_write_immediate_test( Verbs & ib, RDMASharedMemory & shm ) {
  if( communicator.rank == 0 ) { LOG(INFO) << "Starting simple RDMA write with immediate test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;
  
  std::memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  std::memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;
  
  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  std::memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (send_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  std::memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->imm_data = 0x12345678;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  send_wr->wr.rdma.remote_addr = (uintptr_t) recv_data;
  send_wr->wr.rdma.rkey = shm.rkey( communicator.rank );
  
  start_time = MPI_Wtime();
  for( int i = 0; i < message_count_per_core; ++i ) {
    *send_data = 1;
    ib.post_receive( communicator.rank, recv_wr );
    ib.post_send( communicator.rank, send_wr );
    
    int popped = 0;
    while( popped < 2 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}


void simple_rdma_read_test( Verbs & ib, RDMASharedMemory & shm ) {
  if( communicator.rank == 0 ) { LOG(INFO) << "Starting simple RDMA read test"; }
  int64_t * buf = (int64_t *) shm.base();
  struct ibv_recv_wr * recv_wr = (struct ibv_recv_wr *) &buf[0];
  struct ibv_sge * recv_sge = (struct ibv_sge *) &buf[9];
  int64_t * recv_data = &buf[11];
  
  *recv_data = 456;

  std::memset( recv_sge, 0, sizeof(*recv_sge) );
  recv_sge->addr = (intptr_t) recv_data;
  recv_sge->length = sizeof(int64_t);
  recv_sge->lkey = shm.lkey();
  
  std::memset( recv_wr, 0, sizeof(*recv_wr) );
  recv_wr->wr_id = 1;
  recv_wr->next = NULL;
  recv_wr->sg_list = recv_sge;
  recv_wr->num_sge = 1;

  struct ibv_send_wr * send_wr = (struct ibv_send_wr *) &buf[12];
  struct ibv_sge * send_sge = (struct ibv_sge *) &buf[21];
  int64_t * send_data = &buf[23];
  
  *send_data = 1;
  
  std::memset( send_sge, 0, sizeof(*send_sge) );
  send_sge->addr = intptr_t (recv_data);
  send_sge->length = sizeof(int64_t);
  send_sge->lkey = shm.lkey();
  
  std::memset( send_wr, 0, sizeof(*send_wr) );
  send_wr->wr_id = 2;
  send_wr->next = NULL;
  send_wr->sg_list = send_sge;
  send_wr->num_sge = 1;
  send_wr->send_flags = IBV_SEND_SIGNALED;
  send_wr->opcode = IBV_WR_RDMA_READ;
  send_wr->wr.rdma.remote_addr = (uintptr_t) send_data;
  send_wr->wr.rdma.rkey = shm.rkey( communicator.rank );
  
  start_time = MPI_Wtime();
  for( int i = 0; i < message_count_per_core; ++i ) {
    //ib.post_receive( communicator.rank, recv_wr );
    ib.post_send( communicator.rank, send_wr );
    
    int popped = 0;
    while( popped < 1 ) {
      popped += ib.poll();
    }
  }
  end_time = MPI_Wtime();
  
  CHECK_EQ( *send_data, *recv_data );
}

void paired_write_test( Verbs & ib, RDMASharedMemory & shm) {
  Core target = (communicator.rank + communicator.size / 2) % communicator.size;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    std::memset( &wrs[i], 0 , sizeof(wrs[i]) );
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
  if( communicator.rank < (communicator.size / 2) ) {
    for( int i = 0; i < message_count_per_core; i += FLAGS_batch_size ) {
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

  if( communicator.rank >= (communicator.size / 2) ) {
    CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  }  
}

void paired_write_bypass_test( Verbs & ib, RDMASharedMemory & shm) {
  Core target = (communicator.rank + communicator.size / 2) % communicator.size;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
  
  RDMA_WR<data_t> proto;
  __m128i * src128 = reinterpret_cast< __m128i * >( &proto );

  std::memset( &proto, 0 , sizeof(proto) );

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
  if( communicator.rank < (communicator.size / 2) ) {
    for( int i = 0; i < message_count_per_core; i += FLAGS_batch_size ) {
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

  if( communicator.rank >= (communicator.size / 2) ) {
    CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  }  
}

void paired_zero_write_test( Verbs & ib, RDMASharedMemory & shm) {
  Core target = (communicator.rank + communicator.size / 2) % communicator.size;

  typedef int8_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    std::memset( &wrs[i], 0 , sizeof(wrs[i]) );
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
  if( communicator.rank < (communicator.size / 2) ) {
    for( int i = 0; i < message_count_per_core; i += FLAGS_batch_size ) {
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

void paired_read_test( Verbs & ib, RDMASharedMemory & shm) {
  Core target = (communicator.rank + communicator.size / 2) % communicator.size;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    std::memset( &wrs[i], 0 , sizeof(wrs[i]) );
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
  if( communicator.rank < (communicator.size / 2) ) {
    for( int i = 0; i < message_count_per_core; i += FLAGS_batch_size ) {
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

  // if( communicator.rank >= (communicator.size / 2) ) {
  //   CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  // }  
}

void paired_fetchadd_test( Verbs & ib, RDMASharedMemory & shm) {
  Core target = (communicator.rank + communicator.size / 2) % communicator.size;

  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    std::memset( &wrs[i], 0 , sizeof(wrs[i]) );
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
  if( communicator.rank < (communicator.size / 2) ) {
    for( int i = 0; i < message_count_per_core; i += FLAGS_batch_size ) {
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
  // if( communicator.rank >= (communicator.size / 2) ) {
  //   CHECK_EQ( vals[0], FLAGS_batch_size - 1 );
  // }  
}

void random_write_test( Verbs & ib, RDMASharedMemory & shm) {
  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
  CHECK_LE( vals + sizeA_per_core, (data_t*) ((char*)shm.base() + shm.size()) );
    
  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    std::memset( &wrs[i], 0 , sizeof(wrs[i]) );
  }

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    wrs[i].data = i;
    
    wrs[i].sge.addr = (uintptr_t) &wrs[i].data;
    wrs[i].sge.length = sizeof(wrs[i].data);
    wrs[i].sge.lkey = shm.lkey();
    
    wrs[i].wr.wr_id = i;
    wrs[i].wr.next = ((i+1) % FLAGS_dest_batch_size == 0) ? NULL : &wrs[i+1].wr;
    wrs[i].wr.sg_list = &wrs[i].sge;
    wrs[i].wr.num_sge = 1;
    wrs[i].wr.opcode = IBV_WR_RDMA_WRITE;
    wrs[i].wr.send_flags = IBV_SEND_INLINE | (((i+1) % FLAGS_dest_batch_size == 0) ? IBV_SEND_SIGNALED : 0);
  }

  auto refill_wrs = [&wrs,&vals,&shm] {
    for( int i = 0; i < FLAGS_batch_size; i += FLAGS_dest_batch_size ) {
      Core target = next_random_number() % communicator.size;
      for( int j = 0; j < FLAGS_dest_batch_size; ++j ) {
        wrs[i+j].wr.wr.rdma.rkey = shm.rkey(target);
        wrs[i+j].wr.imm_data = target;
        wrs[i+j].wr.wr.rdma.remote_addr = (intptr_t) &vals[ next_random_number() % sizeA_per_core ];
      }
    }
  };

  start_time = MPI_Wtime();
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  for( int i = 0; i < message_count_per_core; i += FLAGS_batch_size ) {
    refill_wrs();
    for( int i = 0; i < FLAGS_batch_size; i += FLAGS_dest_batch_size ) {
      Core target = wrs[i].wr.imm_data;
      ib.post_send( target, &wrs[i].wr );
    }

    // wait for sends to complete
    int popped = 0;
    auto term = FLAGS_batch_size / FLAGS_dest_batch_size;
    while( popped < term ) {
      popped += ib.poll();
    }
  }

  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  end_time = MPI_Wtime();
}

void gups_test( Verbs & ib, RDMASharedMemory & shm) {
  typedef int64_t data_t;
  RDMA_WR<data_t> * wrs = (RDMA_WR<data_t> *) shm.base();
  data_t * vals = (data_t*) (wrs + FLAGS_batch_size);
  CHECK_LE( vals + sizeA_per_core, (data_t*) ((char*)shm.base() + shm.size()) );

  CHECK( FLAGS_batch_size % FLAGS_dest_batch_size == 0 );

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    std::memset( &wrs[i], 0 , sizeof(wrs[i]) );
  }

  for( int i = 0; i < FLAGS_batch_size; ++i ) {
    wrs[i].data = i;
    
    wrs[i].sge.addr = (uintptr_t) &wrs[i].data;
    wrs[i].sge.length = sizeof(wrs[i].data);
    wrs[i].sge.lkey = shm.lkey();
    
    wrs[i].wr.wr_id = i;
    wrs[i].wr.next = ((i+1) % FLAGS_dest_batch_size == 0) ? NULL : &wrs[i+1].wr;
    wrs[i].wr.sg_list = &wrs[i].sge;
    wrs[i].wr.num_sge = 1;
    wrs[i].wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
    wrs[i].wr.send_flags = (((i+1) % FLAGS_dest_batch_size == 0) ? IBV_SEND_SIGNALED : 0);
  }

  auto refill_wrs = [&wrs,&vals,&shm] {
    for( int i = 0; i < FLAGS_batch_size; i += FLAGS_dest_batch_size ) {
      Core target = next_random_number() % communicator.size;
      for( int j = 0; j < FLAGS_dest_batch_size; ++j ) {
        wrs[i+j].wr.wr.atomic.rkey = shm.rkey(target);
        wrs[i+j].wr.imm_data = target;
        wrs[i+j].wr.wr.atomic.remote_addr = (intptr_t) &vals[ next_random_number() % sizeA_per_core ];
        wrs[i+j].wr.wr.atomic.compare_add = 1;
      }
    }
  };

  start_time = MPI_Wtime();
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  for( int i = 0; i < message_count_per_core; i += FLAGS_batch_size ) {
    refill_wrs();
    for( int i = 0; i < FLAGS_batch_size; i += FLAGS_dest_batch_size ) {
      Core target = wrs[i].wr.imm_data;
      ib.post_send( target, &wrs[i].wr );
    }

    // wait for sends to complete
    int popped = 0;
    auto term = FLAGS_batch_size / FLAGS_dest_batch_size;
    while( popped < term ) {
      popped += ib.poll();
    }
  }
  MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
  end_time = MPI_Wtime();
}


int main(int argc, char * argv[]) {
  with_verbs_do( &argc, &argv, [] ( Verbs & ib ) {
      RDMASharedMemory shm( ib );
      shm.init();

      CHECK(communicator.size >= 2); // at least 2 nodes for these tests...

      random_number = FLAGS_seed + communicator.rank;
      next_random_number();

      MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );

      if( FLAGS_mode == "simple_send_recv" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        simple_send_recv_test( ib, shm );
      } else if( FLAGS_mode == "simple_rdma_write" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        simple_rdma_write_test( ib, shm );
      } else if( FLAGS_mode == "simple_rdma_write_immediate" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        simple_rdma_write_immediate_test( ib, shm );
      } else if( FLAGS_mode == "simple_rdma_read" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        simple_rdma_read_test( ib, shm );
      } else if( FLAGS_mode == "paired_write" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        paired_write_test( ib, shm );
      } else if( FLAGS_mode == "paired_write_bypass" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        paired_write_bypass_test( ib, shm );
      } else if( FLAGS_mode == "paired_zero_write" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        paired_zero_write_test( ib, shm );
      } else if( FLAGS_mode == "paired_read" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        paired_read_test( ib, shm );
      } else if( FLAGS_mode == "paired_fetchadd" ) {
        message_count_per_core = FLAGS_sizeB / communicator.locale_size;
        paired_fetchadd_test( ib, shm );
      } else if( FLAGS_mode == "random_write" ) {
        message_count_per_core = FLAGS_sizeB / communicator.size;
        sizeA_per_core = FLAGS_sizeA / communicator.size;
        random_write_test( ib, shm );
      } else if( FLAGS_mode == "gups" ) {
        message_count_per_core = FLAGS_sizeB / communicator.size;
        sizeA_per_core = FLAGS_sizeA / communicator.size;
        gups_test( ib, shm );
      } else {
        LOG(ERROR) << "Test " << FLAGS_mode << " not found.";
      }
  

      MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );

      if( (FLAGS_mode == "random_write") || (FLAGS_mode == "gups") ) {
        if( communicator.rank == 0 ) {
          double count = FLAGS_sizeB;
          double duration = end_time - start_time;
          //double count = iterations * (communicator.size / 2) * message_count_per_core;
          double rate = count / duration / 1.0e9;
          LOG(INFO) << "Sent " << count << " messages in " << duration << ": " << rate << " GUPS";
        }
      } else {
        if( communicator.rank == 0 ) {
          double count = message_count_per_core * communicator.locale_size;
          double duration = end_time - start_time;
          //double count = iterations * (communicator.size / 2) * message_count_per_core;
          double rate = count / duration;
          LOG(INFO) << "Sent " << count << " messages in " << duration << ": " << rate << " Msgs/s";
        }
      }
    
    });
}


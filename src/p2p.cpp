
#include "Verbs.hpp"
#include "MemoryRegion.hpp"
#include "common.hpp"
#include <cstring>

#include <xmmintrin.h>

using namespace RDMA;

DEFINE_int64( iters, 10 , "iterations" );
DEFINE_int64( m, 100, "columns of array" );
DEFINE_int64( n, 100, "rows of array" );
DEFINE_int64( batch_size, 64, "Number of concurrent sent messages" );
DEFINE_int64( dest_batch_size, 8, "Number of concurrent sent messages per node for gups" );
DEFINE_int64( seed, 1 , "Seed for random addresses" );


double percore_start = 0.0, percore_end = 0.0, percore_time = 0.0;
double work_start = 0.0, work_end = 0.0, work_time = 0.0;
double overall_start = 0.0, overall_end = 0.0, overall_time = 0.0;
double total_start = 0.0, total_end = 0.0, total_time = 0.0;
int wait = 0;
int mpercore = 0;
double * data = NULL;


int64_t message_count_per_core = 0;
int64_t sizeA_per_core = 0;

const uint64_t lcgM = 6364136223846793005UL;
const uint64_t lcgB = 1442695040888963407UL;
uint64_t random_number = 0;

uint64_t next_random_number() {
  random_number = lcgM * random_number + lcgB;
  return random_number;
}


struct WR {
  ibv_sge sge;
  ibv_send_wr wr;
} __attribute__ ((aligned (64))); // cache-line align these structs


int main(int argc, char * argv[]) {
  with_verbs_do( &argc, &argv, [] ( Verbs & ib ) {
      random_number = FLAGS_seed + communicator.rank;
      next_random_number();

      communicator.barrier();

      double * left_data = NULL;
      MPI_Win left_win;
      MPI_CHECK( MPI_Win_allocate_shared( sizeof(double) * FLAGS_n, 1, MPI_INFO_NULL, communicator.locale_comm, &left_data, &left_win ) );
      verbs::MemoryRegion left_mr( communicator, ib, left_data, sizeof(double) * FLAGS_n );
      std::memset( left_data, 0, sizeof(double) * FLAGS_n );

      double * right_data = NULL;
      MPI_Win right_win;
      MPI_CHECK( MPI_Win_allocate_shared( sizeof(double) * FLAGS_n, 1, MPI_INFO_NULL, communicator.locale_comm, &right_data, &right_win ) );
      verbs::MemoryRegion right_mr( communicator, ib, right_data, sizeof(double) * FLAGS_n );
      std::memset( right_data, 0, sizeof(double) * FLAGS_n );

      WR * wrs = NULL;
      MPI_Win wr_win;
      MPI_CHECK( MPI_Win_allocate_shared( sizeof(WR) * FLAGS_n, 1, MPI_INFO_NULL, communicator.locale_comm, &wrs, &wr_win ) );
      std::memset( wrs, 0, sizeof(WR) * FLAGS_n );

      MPI_Request requests[FLAGS_n];

      *left_data = (double) communicator.rank;
      MPI_CHECK( MPI_Barrier( communicator.locale_comm ) );

      double * next_ptr = NULL;
      MPI_Aint size;
      int disp;
      MPI_CHECK( MPI_Win_shared_query( left_win, (communicator.locale_rank+1) % communicator.locale_size, &size, &disp, &next_ptr ) );
      double * computed_next_ptr = left_data - FLAGS_n * communicator.locale_rank + FLAGS_n * ((communicator.locale_rank+1) % communicator.locale_size);
      LOG(INFO) << "at " << (void*) left_data << " my rank " << *left_data << " next rank at " << next_ptr << "=" << *next_ptr << " " << computed_next_ptr << "=" << *computed_next_ptr  << " " << sizeof(MPI_Aint);

      // allocate data
      mpercore = FLAGS_m / communicator.size;
      if( mpercore == 0 ) mpercore = 1;
      MPI_CHECK( MPI_Alloc_mem( FLAGS_n * (mpercore) * sizeof(double), MPI_INFO_NULL, &data ) );

      double * left_datas[communicator.size];
      MPI_CHECK( MPI_Allgather( &left_data, 1, MPI_UINT64_T, &left_datas, 1, MPI_UINT64_T, communicator.grappa_comm ) );
      for( int i = 0; i < communicator.size; ++i ) {
        DVLOG(3) << "Rank " << i << " left_data " << left_datas[i];
      }

      MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );

      total_start = MPI_Wtime();
      for( int iter = 0; iter < FLAGS_iters; ++iter ) {

        overall_start = MPI_Wtime();
        MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
        percore_start = MPI_Wtime();
        wait = 0;

        // prepare
        for( int i = 0; i < FLAGS_n; ++i ) {
          if( communicator.rank == 0 ) {
            left_data[i] = (double) i;
          } else {
            left_data[i] = 0.0;
          }
        }

        int first_j = communicator.rank * mpercore;
        for( int j = 0; j < mpercore; ++j ) {
          int actual_j = j + first_j;
          data[j] = (double) actual_j;
        }

        MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );

        for( int i = 1; i < FLAGS_n; ++i ) {

          // if not on leftmost edge, wait for data from left
          if( communicator.rank > 0 ) { 
            DVLOG(4) << "checking tag " << i << " at address " << (void*) &left_data[i];
            if( left_data[i] == 0.0 ) { 
              wait++;
              while( left_data[i] == 0.0 ) { 
                asm volatile("": : :"memory");
                DVLOG(4) << "Waiting for data at " << i;
                // MPI_Status s;
                // MPI_CHECK( MPI_Probe( communicator.rank-1, MPI_ANY_TAG, MPI_COMM_WORLD, &s ) );
                // int count;
                // MPI_CHECK( MPI_Get_count( &s, MPI_DOUBLE, &count) );
                // DVLOG(4) << "Got data at " << i << " with " << count << " elements";
                // MPI_CHECK( MPI_Recv( &left_data[s.MPI_TAG], count, MPI_DOUBLE, communicator.rank-1, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE ) );
              }
            }
            DVLOG(4) << "received " << left_data[i] << " from " << communicator.rank-1 << " with tag " << i;
          }
          // if( work_start == 0.0 ) {
          //   work_start = MPI_Wtime();
          // }

          // do work
          double left = left_data[i];
          double diag = left_data[i-1];
          double up = 0.0;
          double current = left;

          for( int j = 0; j < mpercore; ++j ) {
            int actual_j = j + first_j;
            if( actual_j > 0 ) {
              // compute this cell's value
              up = data[ (i-1)*mpercore + j ];
              current = up + left - diag;

              // update for next iteration
              diag = up;
              left = current;

              // write value
              data[ (i)*mpercore + j ] = current;
            }
          }
          DVLOG(6) << "at (" << i << ", " << first_j+mpercore << ") current is " << current;
          right_data[i] = current;

          // send data to right
          if( communicator.rank+1 != communicator.size ) { // not rightmost edge
            DVLOG(4) << "ready to send for " << i;
            // {
            // intra-node communication; works fine with no batching!
            if( (communicator.locale_rank + 1) < communicator.locale_size ) { 

              left_data[i+FLAGS_n] = right_data[i];

            } else {

              if( (((i+1) % FLAGS_batch_size) == 0) || ((i+1) == FLAGS_n)) {
                DVLOG(6) << "sending for " << i;
                int tosend = FLAGS_batch_size;
                
                if( (i+1) == FLAGS_n && FLAGS_n % FLAGS_batch_size != 0 ){
                  tosend = FLAGS_n % FLAGS_batch_size;
                }
                DVLOG(4) << "sending " << tosend << " elements to " << communicator.rank+1 << " at base " << (i+1)-tosend;
                //LOG(INFO) << "sending " << tosend << " elements to " << communicator.rank+1 << " at base " << (i+1)-tosend;
                // MPI_CHECK( MPI_Isend( &right_data[(i+1)-tosend], tosend, MPI_DOUBLE, communicator.rank+1, (i+1)-tosend, MPI_COMM_WORLD, &requests[(i+1)-tosend] ) );

                WR * s = &wrs[i];
                s->sge.addr = (uintptr_t) &right_data[(i+1)-tosend];
                s->sge.length = sizeof(double) * tosend;
                s->sge.lkey = right_mr.lkey();

                s->wr.wr_id = 0; //(i+1)-tosend;
                s->wr.next = NULL;
                s->wr.sg_list = &s->sge;
                s->wr.num_sge = 1;
                //s->wr.send_flags = IBV_SEND_INLINE ;//| IBV_SEND_SIGNALED;
                s->wr.send_flags = 0;
                //s->wr.send_flags = IBV_SEND_SIGNALED;
                s->wr.opcode = IBV_WR_RDMA_WRITE;
                //s->wr.wr.rdma.remote_addr = (uintptr_t) (&left_data[(i+1)-tosend] - &left_data[0] + left_datas[communicator.rank+1]);
                //s->wr.wr.rdma.remote_addr = (uintptr_t) (&left_datas[communicator.rank+1][(i+1)-tosend]);
                s->wr.wr.rdma.remote_addr = (uintptr_t) (&left_data[(i+1)-tosend] + left_mr.offset_for_core<double>(communicator.rank+1));
                s->wr.wr.rdma.rkey = left_mr.rkey_on_core( communicator.rank+1 );

                DVLOG(3) << "writing " << tosend << " elements to remote addr " << (void*) s->wr.wr.rdma.remote_addr;
                for( int x = 0; x < tosend; ++x ) {
                  DVLOG(3) << "Sending " << x << ": " << right_data[(i+1)-tosend+x];
                }
                ib.post_send( communicator.rank+1, &s->wr );
              }

            }
          }

        }

        // work_end = MPI_Wtime();
        // percore_end = MPI_Wtime();
        // MPI_CHECK( MPI_Barrier( MPI_COMM_WORLD ) );
        // overall_end = MPI_Wtime();

        // overall_time = overall_end - overall_start;
        // percore_time = percore_end - percore_start;
        // work_time = work_end - work_start;
        // DVLOG(1) << "Iter " << iter
        //          << " core " << communicator.rank
        //          << ": overall " << overall_time
        //          << " percore " << percore_time
        //          << " work " << work_time
        //          << " wait " << wait
        //   ;
      }
      total_end = MPI_Wtime();
      total_time = total_end - total_start;
      double avg_time = total_time / FLAGS_iters;
      double total_rate = (FLAGS_n) / avg_time;

      if( communicator.rank == 0 ) {
        LOG(INFO) << FLAGS_iters * FLAGS_n << " communications in " << overall_time << " seconds: "
                  << total_rate << " syncs/sec, " 
                  << 1.0E-06 * 2 * ((double)(FLAGS_m-1)*(FLAGS_n-1)) / avg_time << " MFlops/s." 
                 ;
        LOG(INFO) << "HEADER, type, iters, m, n, batch_size, avg_time, MFlops";
        LOG(INFO) << "DATA, jacob_rdma"
                  << ", " <<  FLAGS_iters
                  << ", " << FLAGS_m
                  << ", " << FLAGS_n
                  << ", " << FLAGS_batch_size
                  << ", " << avg_time
                  << ", " << 1.0E-06 * 2 * ((double)(FLAGS_m-1)*(FLAGS_n-1)) / avg_time;
      }

    });
}


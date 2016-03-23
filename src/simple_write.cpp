
#include "Verbs.hpp"
#include "MemoryRegion.hpp"
#include "common.hpp"
#include <cstring>

#include <xmmintrin.h>

using namespace RDMA;

struct WR {
  ibv_sge sge;
  ibv_send_wr wr;
} __attribute__ ((aligned (64))); // cache-line align these structs


int main(int argc, char * argv[]) {
  with_verbs_do( &argc, &argv, [] ( Verbs & ib ) {
      int n = 1024;

      WR wr[n];
      WR * wrs = &wr[0];

      double * data = NULL;
      //double data[n];
      //double * data = new double[n]; 
      MPI_Win win;
      MPI_CHECK( MPI_Win_allocate_shared( sizeof(double) * n, 1, MPI_INFO_NULL, communicator.locale_comm, &data, &win ) );

      verbs::MemoryRegion mr( communicator, ib, &data[0], sizeof(double) * n );

      //double right_data[n];
      double * right_data = new double[n];
      verbs::MemoryRegion right_mr( communicator, ib, &right_data[0], sizeof(double) * n );

      for(int i = 0; i < n; ++i) {
        data[i] = 0.0;
        right_data[i] = 1.0;
      }




      double * receive_data = new double[communicator.size];
      verbs::MemoryRegion receive_mr( communicator, ib, &receive_data[0], sizeof(double) * communicator.size );
      ibv_recv_wr receive_wrs[communicator.size];
      ibv_sge receive_sges[communicator.size];

      for( int i = 0; i < communicator.size; ++i ) {
        ibv_sge * sge = &receive_sges[i];
        ibv_recv_wr * wr = &receive_wrs[i];

        std::memset( sge, 0, sizeof(*sge) );
        std::memset( wr, 0, sizeof(*wr) );

        sge->addr = (uintptr_t) &receive_data[i];
        sge->length = sizeof(double);
        sge->lkey = receive_mr.lkey();
        
        wr->wr_id = 1000 + i;
        wr->next = NULL;
        wr->sg_list = sge;
        wr->num_sge = 1;

        ib.post_receive( i, wr );
      }

      communicator.barrier();

      // if not on leftmost edge, wait for data from left
      if( communicator.rank > 0 ) { 

        DVLOG(4) << "waiting for nonzero data at address " << (void*) &data[0];
        if( data[0] == 0.0 ) { 
          while( data[0] == 0.0 ) { 
            ib.poll();
            __sync_synchronize();
            asm volatile("": : :"memory");
          }
        }
        DVLOG(4) << "received " << data[0] << " from " << communicator.rank-1;

      } else if( communicator.rank+1 != communicator.size ) { // not rightmost edge

        DVLOG(4) << "sending to " << communicator.rank+1 << " with tag " << 0;
        WR * s = &wrs[0];
        std::memset( s, 0, sizeof(*s) );
        s->sge.addr = (uintptr_t) &right_data[0];
        s->sge.length = sizeof(double);
        s->sge.lkey = right_mr.lkey();
        
        s->wr.wr_id = 123;
        s->wr.next = NULL;
        s->wr.sg_list = &s->sge;
        s->wr.num_sge = 1;
        s->wr.imm_data = 12345;
        //s->wr.send_flags = IBV_SEND_INLINE ;//| IBV_SEND_SIGNALED;
        //s->wr.send_flags = IBV_SEND_SIGNALED;
        //s->wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
        //s->wr.opcode = IBV_WR_SEND;
        s->wr.opcode = IBV_WR_RDMA_WRITE;

        // //s->wr.wr.rdma.remote_addr = (uintptr_t) (&data[(i+1)-tosend] - &data[0] + ptrs[communicator.rank+1]);
        // //s->wr.wr.rdma.remote_addr = (uintptr_t) (&ptrs[communicator.rank+1][(i+1)-tosend]);
        // //s->wr.wr.rdma.remote_addr = (uintptr_t) (&data[(i+1)-tosend] + mr.offset_for_core<double>(communicator.rank+1));

        s->wr.wr.rdma.remote_addr = (uintptr_t) mr.base_on_core( communicator.rank+1 );
        s->wr.wr.rdma.rkey = mr.rkey_on_core( communicator.rank+1 );

        // s->wr.wr.rdma.remote_addr = (uintptr_t) &data[0]; 
        // s->wr.wr.rdma.rkey = mr.rkey();
        
        DVLOG(3) << "writing " << 1 << " element from " << (void*) s->sge.addr << " to remote addr " << (void*) s->wr.wr.rdma.remote_addr;
        ib.poll();
        LOG(INFO) << "data[0] == " << data[0];
        ib.post_send( communicator.rank+1, &s->wr );
        //ib.post_send( communicator.rank, &s->wr );
        //while(!ib.poll());
      }
      LOG(INFO) << "data[0] == " << data[0];
    
      LOG(INFO) << "Locally done";
    });
}


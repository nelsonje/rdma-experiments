//
// MPI / InfiniBand Verbs demo
//
// Run on Sampa cluster with command like:
//   make && srun --label --nodes=2 --ntasks-per-node=3 ./main

#include "MPIConnection.hpp"
#include "Verbs.hpp"

#include <sys/types.h>
#include <unistd.h>

int main( int argc, char * argv[] ) {

  MPIConnection m( &argc, &argv );
  Verbs v( m );
  
  m.barrier();

#ifdef VERBOSE
  std::cout << "hostname " << m.hostname()
            << " MPI rank " << m.rank
            << " ranks " << m.ranks
            << " locale " << m.locale
            << " locales " << m.locales
            << " locale rank " << m.locale_rank
            << " locale ranks " << m.locale_size
            << " pid " << getpid()
            << "\n";
#endif
  
  m.barrier();

  m.finalize();
  return 0; 
}

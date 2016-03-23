
#include "MPIConnection.hpp"
#include <sys/types.h>
#include <unistd.h>

int main( int argc, char * argv[] ) {

  MPIConnection m;
  m.init( &argc, &argv );

  m.barrier();

  std::cout << "hostname " << m.hostname()
            << " rank " << m.rank
            << " ranks " << m.ranks
            << " locale " << m.locale
            << " locales " << m.locales
            << " locale rank " << m.locale_rank
            << " locale ranks " << m.locale_size
            << " pid " << getpid()
            << "\n";
  
  m.barrier();

  m.finalize();
  return 0; 
}

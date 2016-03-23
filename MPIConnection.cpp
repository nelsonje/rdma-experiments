#include "MPIConnection.hpp"
#include <limits>

void MPIConnection::init( int * argc_p, char ** argv_p[] ) {

  //
  // MPI Boilerplate derived from Grappa
  //
  
  MPI_CHECK( MPI_Init( argc_p, argv_p ) ); 

  // get locale-local MPI communicator
  MPI_CHECK( MPI_Comm_split_type( MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &locale_communicator_ ) );
  MPI_CHECK( MPI_Comm_set_errhandler( locale_communicator_, MPI_ERRORS_RETURN ) );
  MPI_CHECK( MPI_Comm_rank( locale_communicator_, &locale_rank_ ) );
  MPI_CHECK( MPI_Comm_size( locale_communicator_, &locale_size_ ) );

  // get count of locales
  int32_t localesint = locale_rank == 0; // count one per locale and broadcast
  MPI_CHECK( MPI_Allreduce( MPI_IN_PLACE, &localesint, 1, MPI_INT32_T,
                            MPI_SUM, MPI_COMM_WORLD ) );
  locales_ = localesint;

  // get my locale
  int32_t mylocaleint = locale_rank == 0;  // count one per locale and sum
  MPI_CHECK( MPI_Scan( MPI_IN_PLACE, &mylocaleint, 1, MPI_INT32_T,
                       MPI_SUM, MPI_COMM_WORLD ) );
  // copy to all cores in locale
  MPI_CHECK( MPI_Bcast( &mylocaleint, 1, MPI_INT32_T,
                        0, locale_communicator_ ) );
  mylocaleint -= 1; // make zero-indexed
  locale_ = mylocaleint;
    
  // make new communicator with ranks laid out so that nodes hold adjacent ranks
  MPI_CHECK( MPI_Comm_split( MPI_COMM_WORLD, 0, mylocaleint, &main_communicator_ ) );
  MPI_CHECK( MPI_Comm_set_errhandler( main_communicator_, MPI_ERRORS_RETURN ) );
  int main_mycoreint = -1;
  int main_coresint = -1;
  MPI_CHECK( MPI_Comm_rank( main_communicator_, &main_mycoreint ) );
  MPI_CHECK( MPI_Comm_size( main_communicator_, &main_coresint ) );
  rank_ = main_mycoreint;
  size_ = main_coresint;
    
  // verify locale numbering is consistent with locales
  int32_t localemin = std::numeric_limits<int32_t>::max();
  int32_t localemax = std::numeric_limits<int32_t>::min();
  MPI_CHECK( MPI_Reduce( &mylocaleint, &localemin, 1, MPI_INT32_T,
                         MPI_MIN, 0, locale_communicator_ ) );
  MPI_CHECK( MPI_Reduce( &mylocaleint, &localemax, 1, MPI_INT32_T,
                         MPI_MAX, 0, locale_communicator_ ) );
  if( (0 == locale_rank_) && (localemin != localemax) ) {
    std::cerr << "Locale ID is not consistent across locale!\n";
    exit(1);
  }

  // verify locale core count is the same across job
  int32_t locale_coresmin = std::numeric_limits<int32_t>::max();
  int32_t locale_coresmax = std::numeric_limits<int32_t>::min();
  MPI_CHECK( MPI_Reduce( &locale_size_, &locale_coresmin, 1, MPI_INT32_T,
                         MPI_MIN, 0, main_communicator_ ) );
  MPI_CHECK( MPI_Reduce( &locale_size_, &locale_coresmax, 1, MPI_INT32_T,
                         MPI_MAX, 0, main_communicator_ ) );
  if( 0 == rank_ && ( locale_coresmin != locale_coresmax ) ) {
    std::cerr << "Number of cores per locale is not the same across job!\n";
    exit(1);
  }
  
  barrier();
}

void MPIConnection::finalize() {
  barrier();
  MPI_CHECK( MPI_Finalize() );
}

void MPIConnection::barrier() {
  MPI_CHECK( MPI_Barrier( main_communicator_ ) );
}

void MPIConnection::locale_barrier() {
  MPI_CHECK( MPI_Barrier( locale_communicator_ ) );
}

const char * MPIConnection::hostname() {
  static char name[ MPI_MAX_PROCESSOR_NAME ] = {0};
  static int name_size = 0;
  if( '\0' == name[0] ) {
    MPI_CHECK( MPI_Get_processor_name( &name[0], &name_size ) );
  }
  return &name[0];
}

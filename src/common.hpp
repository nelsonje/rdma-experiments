#pragma once

namespace impl {
  // A small helper for Google logging CHECK_NULL().
  template <typename T>
  inline T* CheckNull(const char *file, int line, const char *names, T* t) {
    if (t != NULL) {
      google::LogMessageFatal(file, line, new std::string(names));
    }
    return t;
  }
}

#define CHECK_NULL(val)                                              \
  impl::CheckNull(__FILE__, __LINE__, "'" #val "' Must be non NULL", (val))

#ifdef NDEBUG
#define DCHECK_NULL(val)                                              \
  impl::CheckNull(__FILE__, __LINE__, "'" #val "' Must be non NULL", (val))
#else
#define DCHECK_NULL(val)                        \
  ;
#endif  



//
// macro to deal with MPI errors
//
#define MPI_CHECK( mpi_call )                                           \
  do {                                                                  \
    int retval;                                                         \
    if( (retval = (mpi_call)) != 0 ) {                                  \
      char error_string[MPI_MAX_ERROR_STRING];                          \
      int length;                                                       \
      MPI_Error_string( retval, error_string, &length);                 \
      LOG(FATAL) << "MPI call failed: " #mpi_call ": "                  \
                 << error_string;                                       \
    }                                                                   \
  } while(0)

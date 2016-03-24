#pragma once
////////////////////////////////////////////////////////////////////////
// Copyright (c) 2010-2015, University of Washington and Battelle
// Memorial Institute.  All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//     * Redistributions of source code must retain the above
//       copyright notice, this list of conditions and the following
//       disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials
//       provided with the distribution.
//     * Neither the name of the University of Washington, Battelle
//       Memorial Institute, or the names of their contributors may be
//       used to endorse or promote products derived from this
//       software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// UNIVERSITY OF WASHINGTON OR BATTELLE MEMORIAL INSTITUTE BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
// OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
// USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.
////////////////////////////////////////////////////////////////////////

#include "SymmetricAllocatorImplementation.hpp"


/// This is a memory allocator that allocates at the same address on
/// all processes/MPI ranks in a job. It's just a wrapper around an
/// opensource malloc() implementation that uses mmap() collectively
/// to get blocks of memory at a particular virtual address.
class SymmetricAllocator {
  MPIConnection & m;

public:
  SymmetricAllocator( MPIConnection & m )
    : m(m)
  {
    Grappa::impl::global_mpi_connection_p = &m;
  }

  /// Allocate space for n T's at the same local address on all
  /// cores. All cores must call this function simultaneously.
  template< typename T >
  T * alloc( size_t n = 1 ) {
    auto byte_size = n * sizeof(T);
    void * p = Grappa::impl::dlmalloc( byte_size );
    m.barrier();
    return reinterpret_cast< T * >( p );
  }
  
  /// Free space allocated by symmetric_alloc. All cores must call
  /// this function simultaneously.
  template< typename T >
  void free( T * t ) {
    m.barrier();
    Grappa::impl::dlfree( t );
  }
};

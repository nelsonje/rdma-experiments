CXX=mpicxx
CC=mpicxx
LD=mpicxx

# compiler flags
CXXFLAGS+= -g -O2 -std=c++11 -Wall

# uncomment this to enable verbose logging
#CXXFLAGS+= -DVERBOSE

# linker flags (not libraries)
LDFLAGS+=

# libraries to link agains
LDLIBS+= -libverbs

# add header files and related things to this variable
COMMON_DEPS= Makefile MPIConnection.hpp Verbs.hpp MemoryRegion.hpp SymmetricAllocator.hpp SymmetricAllocatorImplementation.hpp 

# add object files everyone uses to this variable
COMMON_OBJS= MPIConnection.o Verbs.o SymmetricAllocator.o

# targets should be added to this variable (for cleanup)
TARGETS= 



#
# this Makefile uses implicit build rules.
#
# just add three lines like the following for any new app, and add any
# header dependences to COMMON_DEPS and object file dependences to
# COMMON_OBJS. They will be built automatically.
#

# rules for an application with a main() function
TARGETS+= simple_write
simple_write: simple_write.o $(COMMON_OBJS)
# ensure we rebuild object file if headers or Makefile change
simple_write.o: $(COMMON_DEPS)

TARGETS+= simple_atomic_increment
simple_atomic_increment: simple_atomic_increment.o $(COMMON_OBJS)
# ensure we rebuild object file if headers or Makefile change
simple_atomic_increment.o: $(COMMON_DEPS)

TARGETS+= simple_read
simple_read: simple_read.o $(COMMON_OBJS)
# ensure we rebuild object file if headers or Makefile change
simple_read.o: $(COMMON_DEPS)




# overly-broad rule to ensure object files are rebuilt if header files change
$(COMMON_OBJS): $(COMMON_DEPS)

clean:
	rm -f $(COMMON_OBJS) $(TARGETS) $(TARGETS:%=%.o)

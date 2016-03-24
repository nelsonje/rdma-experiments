

CXX=mpicxx
CC=mpicxx
LD=mpicxx

# compiler flags
CXXFLAGS+= -g -O2 -std=c++11 -Wall

# uncomment this to enable verbose logging
#CXXFLAGS+= -DVERBOSE

# linker flags (not libraries)
#LDFLAGS+=

# libraries to link agains
LDLIBS+= -libverbs

# add header files and related things here
DEPS= Makefile MPIConnection.hpp Verbs.hpp MemoryRegion.hpp

# add object files everyone uses
OBJ= MPIConnection.o Verbs.o

# add targets to be cleaned up here
TARGETS= main

#
# this Makefile uses implicit build rules.
#
# just add two rules like the following for any new app, and add any
# header dependences to DEPS and object file dependences to OBJ. They
# will be built automatically.
#

# rules for an application with a main() function
main: main.o $(OBJ)
# ensure we rebuild object file if headers or Makefile change
main.o: $(DEPS)


# overly-broad rule to ensure object files are rebuilt if header files change
$(OBJ): $(DEPS)

clean:
	rm -f $(OBJ) $(TARGETS)


CXX=mpicxx
CC=mpicxx
LD=mpicxx

# compiler flags
CXXFLAGS+= -g -O2 -std=c++11 -Wall

# verbose logging
CXXFLAGS+= -DVERBOSE

# linker flags (not libraries)
#LDFLAGS+=

# libraries to link agains
LDLIBS+= -libverbs

# add header files and related things here
DEPS= Makefile MPIConnection.hpp Verbs.hpp

# add object files everyone uses
OBJ= MPIConnection.o Verbs.o

# add targets to be cleaned up here
TARGETS= main

# rules for an application with a main() function
main: main.o $(OBJ)
# ensure we rebuild object file if headers or Makefile change
main.o: $(DEPS)


# overly-broad rule to ensure object files are rebuilt if header files change
$(OBJ): $(DEPS)

clean:
	rm -f $(OBJ) $(TARGETS)

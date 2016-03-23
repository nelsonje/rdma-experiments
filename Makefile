
CXX=mpicxx
CC=mpicxx
LD=mpicxx

# compiler flags
CXXFLAGS+= -g -O2 -std=c++11 -Wall

# linker flags (not libraries)
#LDFLAGS+=

# libraries to link agains
#LDLIBS+= -ldl -lrt -lpthread -libverbs

# add header files and related things here
DEPS= Makefile MPIConnection.hpp

# add object files everyone uses
OBJ= MPIConnection.o

# add targets to be cleaned up here
TARGETS= main

main: main.o $(OBJ)



# overly-broad rule to ensure object files are rebuilt if header files change
$(OBJ): $(DEPS)

clean:
	rm -f $(OBJ) $(TARGETS)

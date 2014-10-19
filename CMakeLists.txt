
# (2.8 has ExternalProject support)
cmake_minimum_required(VERSION 2.8)
include(ExternalProject)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

set(CMAKE_XCODE_ATTRIBUTE_GCC_VERSION "com.apple.compilers.clang.3_4")

project(RdmaExperiments)

set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG")

set(CMAKE_C_FLAGS_DEBUG "-O1 -DDEBUG -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -DDEBUG -g")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
#enable_testing()
# set(CMAKE_BUILD_TYPE RelWithDebInfo)
site_name(MACHINENAME)

# global C++ flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall")

# TODO: use -stdlib=libc++ too?

#########################################
# Determine third party tools to build
if(NOT DEFINED THIRD_PARTY_ROOT)
  set(THIRD_PARTY_ROOT "${CMAKE_BINARY_DIR}/third-party" CACHE TYPE PATH)
endif()




###########################
# Use RUNPATH if available
# use, i.e. don't skip the full RPATH for the build tree
set(CMAKE_SKIP_BUILD_RPATH  FALSE)
# use final INSTALL_RPATH even in build tree (this lets us manually add things to CMAKE_INSTALL_RPATH)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
# add the automatically determined parts of the RPATH
# which point to directories outside the build tree to the install RPATH
# set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

# the RPATH to be used when installing, but only if it's not a system directory
list(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_PREFIX}/lib" isSystemDir)
if("${isSystemDir}" STREQUAL "-1")
   set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
endif("${isSystemDir}" STREQUAL "-1")

## Not setting runpath because having MPI libs in LD_LIBRARY_PATH was messing up VampirTrace's ability to find its own libs. Maybe there's another way to fix this, but it just seems more robust (for now) to not allow LD_LIBRARY_PATH to affect our libs (after configure uses it to find them).
# set runpath, too
# if(NOT APPLE)
#   set(CMAKE_EXE_LINKER_FLAGS "-Wl,--enable-new-dtags")
# endif()

set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH}:${THIRD_PARTY_ROOT}/lib")




# pre-compiled external dependencies
include_directories("${THIRD_PARTY_ROOT}/include")
link_directories("${THIRD_PARTY_ROOT}/lib")
list(APPEND RDMA_EXPERIMENT_LIBS
  gflags
  glog
)



# TODO: actually check if PMI is available for this platform, possibly build it
if(NOT APPLE)
  list(APPEND RDMA_EXPERIMENT_LIBS pmi)
endif()

if(NOT APPLE)
  list(APPEND RDMA_EXPERIMENT_LIBS pthread rt)
endif()


find_library(VERBS_LIB
  NAMES ibverbs
  HINTS "/usr/lib64")

if(NOT APPLE)
  list(APPEND ROCEHAMMER_LIBS pthread rt)
endif()

# MPI (mostly for booting, but we also use MPI communication in some places now)
find_package(MPI REQUIRED)
include_directories(${MPI_INCLUDE_PATH})

#####################################
# Boost
#####################################
set(BOOST_COMPONENTS filesystem system unit_test_framework)

# hint for sampa cluster
set(BOOST_ROOT /sampa/share/gcc-4.8.2/src/boost_1_55_0)

set(Boost_USE_MULTITHREADED OFF)
find_package(Boost 1.51 COMPONENTS ${BOOST_COMPONENTS} QUIET)

if(NOT Boost_FOUND)
  set(Boost_USE_MULTITHREADED ON)
  find_package(Boost 1.51 COMPONENTS ${BOOST_COMPONENTS} QUIET)
endif()
if(Boost_FOUND)
  include_directories(${Boost_INCLUDE_DIR})
  link_directories(${Boost_LIBRARY_DIRS})
  string(REGEX REPLACE "/include" "" BOOST_PREFIX ${Boost_INCLUDE_DIR} )
  message("-- Boost found:  ${Boost_MAJOR_VERSION}.${Boost_MINOR_VERSION}.${Boost_SUBMINOR_VERSION} -- ${BOOST_PREFIX}")
  # Boost_LIBRARIES set with the libraries to link against
  foreach(lib ${Boost_LIBRARY_DIRS})
    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH}:${lib}")
  endforeach()
  
else()
  message("-- Boost not found.")
  message("   !! Will download and build Boost, which may take a while.")
endif()


macro(add_rdma_exe target exe )
  add_executable(${target} EXCLUDE_FROM_ALL ${ARGN})
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_NAME "${exe}")
  target_link_libraries(${target} 
    RdmaExperiments
    ${MPI_CXX_LIBRARIES}
    ${Boost_LIBRARIES}
  )
endmacro(add_rdma_exe)

macro(add_rdma_application name)
  add_rdma_exe(${name} ${name} ${ARGN})
  set_property(TARGET ${name} PROPERTY FOLDER "Applications") # For organization in Xcode project
endmacro(add_rdma_application)


## put RDMA experiment src directory on include path for everything following this
include_directories(src)

####################################
# subdirectories
add_subdirectory(third-party)
add_subdirectory(src)

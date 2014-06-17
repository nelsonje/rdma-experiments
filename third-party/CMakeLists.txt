# don't clean custom targets in this directory
SET_DIRECTORY_PROPERTIES(PROPERTIES CLEAN_NO_CUSTOM 1)

###########################################
# External projects
###########################################
set(EXTERN_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/build")
set(EXTERN_STAMP_DIR "${EXTERN_BUILD_DIR}/stamps")

message("-- Using {${BASE_C_COMPILER},${BASE_CXX_COMPILER}} to build third-party dependencies (if needed).")


if(APPLE)
  # set(APPLE_CXXFLAGS CXXFLAGS=-stdlib=libstdc++)
endif()







find_library(THIRD_PARTY_GFLAGS "gflags" "${THIRD_PARTY_ROOT}/lib")
if( NOT THIRD_PARTY_GFLAGS )
  message("     gflags: <building>")
  ExternalProject_Add( third-party-gflags
    URL                "http://gflags.googlecode.com/files/gflags-2.0-no-svn-files.tar.gz"
    SOURCE_DIR         ${EXTERN_BUILD_DIR}/src/gflags-2.0
    PREFIX             ${EXTERN_BUILD_DIR}/gflags
    STAMP_DIR          ${EXTERN_STAMP_DIR}/gflags
    BINARY_DIR         ${EXTERN_BUILD_DIR}/gflags
    INSTALL_DIR        ${THIRD_PARTY_ROOT}
    CONFIGURE_COMMAND
      <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> CC=${BASE_C_COMPILER} CXX=${BASE_CXX_COMPILER} CFLAGS=-g ${APPLE_CXXFLAGS} 
    BUILD_COMMAND      make -j
    INSTALL_COMMAND    make install
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
  )
  list(APPEND tool_list third-party-gflags)
else()
  message("     gflags: ${THIRD_PARTY_GFLAGS}")
endif()




find_library(THIRD_PARTY_GLOG "glog" "${THIRD_PARTY_ROOT}/lib")
if( NOT THIRD_PARTY_GLOG )
  message("     glog:   <building>")
  ExternalProject_Add( third-party-glog
    URL                http://google-glog.googlecode.com/files/glog-0.3.3.tar.gz
    # URL_HASH           SHA1=ed40c26ecffc5ad47c618684415799ebaaa30d65
    #SOURCE_DIR         ${EXTERN_BUILD_DIR}/src/glog-0.3.3
    SOURCE_DIR         ${EXTERN_BUILD_DIR}/src/glog
    PREFIX             ${EXTERN_BUILD_DIR}/glog
    STAMP_DIR          ${EXTERN_STAMP_DIR}/glog
    BINARY_DIR         ${EXTERN_BUILD_DIR}/glog
    INSTALL_DIR        ${THIRD_PARTY_ROOT}
    CONFIGURE_COMMAND  <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> CXXFLAGS=-g\ -I${THIRD_PARTY_ROOT}/include LDFLAGS=-L${THIRD_PARTY_ROOT}/lib CC=${BASE_C_COMPILER} CXX=${BASE_CXX_COMPILER} CFLAGS=-g ${APPLE_CXXFLAGS}
    BUILD_COMMAND      make -j
    INSTALL_COMMAND    make install
    DEPENDS third-party-gflags
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
  )
  list(APPEND tool_list third-party-glog)
else()
  message("     glog:   ${THIRD_PARTY_GLOG}")
endif()






# set(third-party-targets  third-party-gasnet third-party-gflags third-party-glog third-party-libcrafter )
add_custom_target(all-third-party DEPENDS ${tool_list} )
set_property(TARGET all-third-party PROPERTY FOLDER "Third Party")

foreach(tool_target ${tool_list})
  set_property(TARGET ${tool_target} PROPERTY FOLDER "Third Party")
endforeach()

add_custom_target(clean-third-party
  rm -rf ${THIRD_PARTY_ROOT}
)
set_property(TARGET clean-third-party PROPERTY FOLDER "Third Party")

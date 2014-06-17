
#
# set up library 
#
set(LIBRARY_SOURCES
  # nothing yet
)
add_library(RdmaExperiments
  ${LIBRARY_SOURCES}
)
target_link_libraries(RdmaExperiments ${RDMA_EXPERIMENT_LIBS})
add_dependencies(RdmaExperiments all-third-party)





add_rdma_application(helloworld.exe "helloworld.cpp")


#
# set up library 
#
set(LIBRARY_SOURCES
  Verbs.cpp
  MemoryRegion.cpp
)
add_library(RdmaExperiments
  ${LIBRARY_SOURCES}
)
target_link_libraries(RdmaExperiments ${RDMA_EXPERIMENT_LIBS} ${VERBS_LIB})
add_dependencies(RdmaExperiments all-third-party)





add_rdma_application(helloworld.exe "helloworld.cpp")
add_rdma_application(experiments.exe "experiments.cpp")
add_rdma_application(p2p.exe "p2p.cpp")
add_rdma_application(simple_write.exe "simple_write.cpp")


#
# set up library 
#
set(LIBRARY_SOURCES
  Verbs.cpp
)
add_library(RdmaExperiments
  ${LIBRARY_SOURCES}
)
target_link_libraries(RdmaExperiments ${RDMA_EXPERIMENT_LIBS} ${VERBS_LIB})
add_dependencies(RdmaExperiments all-third-party)





add_rdma_application(helloworld.exe "helloworld.cpp")
add_rdma_application(experiments.exe "experiments.cpp")

proto_library(auto_parallel_proto SRCS auto_parallel.proto)

collect_srcs(core_srcs SRCS device_mesh.cc process_mesh.cc dist_attr.cc
             dist_mapper.cc)

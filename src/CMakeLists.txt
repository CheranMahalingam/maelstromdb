file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/protogen")

foreach(target log raft)
  get_filename_component(proto "../src/protos/${target}.proto" ABSOLUTE)
  get_filename_component(proto_path "${proto}" PATH)
  set(proto_hdrs "${CMAKE_BINARY_DIR}/protogen/${target}.pb.h")
  set(proto_srcs "${CMAKE_BINARY_DIR}/protogen/${target}.pb.cc")
  set(grpc_hdrs "${CMAKE_BINARY_DIR}/protogen/${target}.grpc.pb.h")
  set(grpc_srcs "${CMAKE_BINARY_DIR}/protogen/${target}.grpc.pb.cc")

  add_custom_command(
    OUTPUT "${proto_srcs}" "${proto_hdrs}" "${grpc_srcs}" "${grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_BINARY_DIR}/protogen"
    --cpp_out "${CMAKE_BINARY_DIR}/protogen"
      -I "${proto_path}"
      --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
      "${proto}"
    DEPENDS "${proto}")

  add_library("grpc_${target}_proto"
    ${grpc_srcs}
    ${grpc_hdrs}
    ${proto_srcs}
    ${proto_hdrs})

  target_link_libraries("grpc_${target}_proto"
    gRPC::grpc++_reflection
    gRPC::grpc++
    protobuf::libprotoc
    protobuf::libprotobuf)
endforeach()

add_library(maelstromdb_lib
  raft/consensus_module.cpp
  raft/global_ctx_manager.cpp
  raft/raft_client.cpp
  raft/raft_server.cpp
  raft/storage.cpp
  raft/cluster_configuration.cpp
  raft/leader_proxy.cpp
  raft/session_cache.cpp
  raft/state_machine.cpp
  core/async_executor.cpp
  core/timer.cpp
  core/inmemory_store.cpp
  cli/command_parser.cpp
  cli/create.cpp
  cli/reconfigure.cpp
  cli/query.cpp
  cli/write.cpp)
target_link_libraries(maelstromdb_lib
  PUBLIC
  grpc_raft_proto
  grpc_log_proto
  glog::glog)
target_include_directories(maelstromdb_lib
  PUBLIC
  core
  engine
  protos
  raft
  utils
  "${CMAKE_BINARY_DIR}/protogen")

add_executable(maelstromcli
  cli/maelstromcli.cpp)
target_link_libraries(maelstromcli
  maelstromdb_lib)



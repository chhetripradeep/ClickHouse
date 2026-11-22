cmake_minimum_required(VERSION 3.20)
project(nuraft_manual_probe LANGUAGES CXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The probe's thread instrumentation requires Linux")
endif ()
if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "This probe currently requires GCC and static libstdc++ for instrumentation")
endif ()

get_filename_component(CLICKHOUSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(NURAFT "${CLICKHOUSE_ROOT}/contrib/NuRaft")
if (NOT EXISTS "${NURAFT}/src/raft_server.cxx")
    message(FATAL_ERROR "Initialize the pinned contrib/NuRaft submodule first")
endif ()

# Mirror the core source list in contrib/nuraft-cmake, without Asio or its launcher.
set(CORE_SOURCES
    handle_priority buffer_serializer peer global_mgr buffer handle_client_request
    raft_server snapshot handle_commit error_code crc32 handle_snapshot_sync stat_mgr
    handle_join_leave handle_user_cmd handle_custom_notification handle_vote log_entry
    srv_config snapshot_sync_req snapshot_sync_ctx handle_timeout handle_append_entries
    cluster_config client_req_stream)
list(TRANSFORM CORE_SOURCES PREPEND "${NURAFT}/src/")
list(TRANSFORM CORE_SOURCES APPEND ".cxx")

find_package(Threads REQUIRED)
add_library(nuraft_probe_core OBJECT ${CORE_SOURCES}
    "${NURAFT}/examples/in_memory_log_store.cxx"
    "${CLICKHOUSE_ROOT}/src/Coordination/SessionExpiryQueue.cpp")
target_compile_features(nuraft_probe_core PUBLIC cxx_std_17)
target_compile_definitions(nuraft_probe_core PUBLIC USE_CLICKHOUSE_THREADS=0)
target_include_directories(nuraft_probe_core PUBLIC
    "${NURAFT}/include" "${NURAFT}/include/libnuraft" "${NURAFT}/examples"
    "${CLICKHOUSE_ROOT}/src")
target_link_libraries(nuraft_probe_core PUBLIC Threads::Threads)
# A shared libstdc++ would hide its pthread calls from the linker wrapper.
target_link_options(nuraft_probe_core INTERFACE
    -static-libstdc++ -static-libgcc
    "-Wl,--wrap=pthread_create"
    "-Wl,--wrap=_ZNSt6chrono3_V212steady_clock3nowEv"
    "-Wl,--wrap=_ZNSt6chrono3_V212system_clock3nowEv")

add_executable(nuraft_manual_probe probe.cpp)
target_link_libraries(nuraft_manual_probe PRIVATE nuraft_probe_core)
add_executable(nuraft_figure8_probe figure8.cpp)
target_link_libraries(nuraft_figure8_probe PRIVATE nuraft_probe_core)

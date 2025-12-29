cmake_minimum_required(VERSION 3.14)

project(bench_libfork LANGUAGES CXX C)

include(../cmake/project-is-top-level.cmake)
include(../cmake/folders.cmake)

# ---- 1. Dependencies ----

if(PROJECT_IS_TOP_LEVEL)
  find_package(libfork REQUIRED)
endif()

find_package(benchmark REQUIRED)

include(FetchContent)

# External Lace
FetchContent_Declare(lace GIT_REPOSITORY https://github.com/trolando/lace.git GIT_TAG v2.0.3)
set(LACE_BUILD_TESTS OFF CACHE INTERNAL "")
set(LACE_BUILD_BENCHMARKS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(lace)

# TMC
FetchContent_Declare(tmc GIT_REPOSITORY https://github.com/tzcnt/TooManyCooks.git GIT_TAG df81ef9f493e88df6b7ac08f0490e968866fa10b CONFIGURE_COMMAND "" BUILD_COMMAND "")
FetchContent_GetProperties(tmc)
if(NOT tmc_POPULATED)
  FetchContent_Populate(tmc)
endif()

# ---- 2. Separate Sources ----

# Common Sources (Compile these ONCE)
file(GLOB_RECURSE BENCH_C_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/*.c")
file(GLOB_RECURSE BENCH_SERIAL CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/serial.cpp")
file(GLOB_RECURSE BENCH_LACE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/lace*.cpp")
file(GLOB_RECURSE BENCH_tmc CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/tmc.cpp")

set(BENCH_COMMON_SOURCES
        ${BENCH_C_SOURCES} ${BENCH_SERIAL} ${BENCH_LACE} ${BENCH_tmc}
        "${CMAKE_CURRENT_SOURCE_DIR}/source/calibrate.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/tmc.cpp"
)

# Libfork Sources (Compile these 3 TIMES)
file(GLOB_RECURSE BENCH_LIBFORK CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/libfork.cpp")

# ---- 3. Create Common Library (The Optimization) ----

add_library(bench_common OBJECT ${BENCH_COMMON_SOURCES})

target_link_libraries(bench_common PUBLIC libfork::libfork benchmark::benchmark_main lace)

target_include_directories(bench_common PUBLIC ${tmc_SOURCE_DIR}/include)
target_compile_definitions(bench_common PUBLIC TMC_WORK_ITEM=CORO TMC_PRIORITY_COUNT=1 TMC_USE_HWLOC)

# Add Optional Deps to Common Lib
find_package(TBB QUIET)
if(TBB_FOUND)
  file(GLOB_RECURSE BENCH_TBB CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/tbb.cpp")
  target_sources(bench_common PRIVATE ${BENCH_TBB})
  target_link_libraries(bench_common PUBLIC TBB::tbb)
endif()

find_package(OpenMP QUIET)
if(OpenMP_FOUND)
  file(GLOB_RECURSE BENCH_OpenMP CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/omp.cpp")
  target_sources(bench_common PRIVATE ${BENCH_OpenMP})
  target_link_libraries(bench_common PUBLIC OpenMP::OpenMP_CXX)
endif()

find_package(Taskflow QUIET)
if(Taskflow_FOUND)
  file(GLOB_RECURSE BENCH_Taskflow CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/taskflow.cpp")
  target_sources(bench_common PRIVATE ${BENCH_Taskflow})
  target_link_libraries(bench_common PUBLIC Taskflow::Taskflow)
endif()

find_package(concurrencpp QUIET)
if(concurrencpp_FOUND)
  file(GLOB_RECURSE BENCH_concurrencpp CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/**/ccpp.cpp")
  target_sources(bench_common PRIVATE ${BENCH_concurrencpp})
  target_link_libraries(bench_common PUBLIC concurrencpp::concurrencpp)
endif()


# ---- 4. Define Variants ----

function(add_deque_benchmark TARGET_NAME DEQUE_FLAG)
  # Only compile the libfork-specific files here
  add_executable(${TARGET_NAME} ${BENCH_LIBFORK} $<TARGET_OBJECTS:bench_common>)

  # Link the pre-compiled common library + libfork
  target_link_libraries(${TARGET_NAME} PRIVATE bench_common libfork::libfork)

  # Apply the unique flag
  target_compile_definitions(${TARGET_NAME} PRIVATE ${DEQUE_FLAG})

  if(LF_NO_CHECK)
    target_compile_definitions(${TARGET_NAME} PRIVATE LF_NO_CHECK)
  endif()
endfunction()

# Create the 3 Executables
add_deque_benchmark(bench_chase_lev  LF_BENCH_CHASE_LEV)
add_deque_benchmark(bench_blocking   LF_BENCH_BLOCKING)
add_deque_benchmark(bench_lace       LF_BENCH_LACE)

# ---- 5. Coro Bench ----

add_executable(coro_bench ${CMAKE_CURRENT_SOURCE_DIR}/source/coroutine/incremental.cpp)
target_link_libraries(coro_bench PRIVATE libfork::libfork benchmark::benchmark_main)

add_folders(benchmarks)
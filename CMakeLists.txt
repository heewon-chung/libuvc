cmake_minimum_required(VERSION 3.10...3.31)

project (libsnark-uvc)

# Detect non-x86 architectures (e.g. Apple Silicon) and set portable defaults
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  set(DEFAULT_CURVE "ALT_BN128")
  set(DEFAULT_WITH_SUPERCOP OFF)
  set(DEFAULT_WITH_PROCPS OFF)
  set(DEFAULT_USE_ASM OFF)
else()
  set(DEFAULT_CURVE "BN128")
  set(DEFAULT_WITH_SUPERCOP ON)
  set(DEFAULT_WITH_PROCPS ON)
  set(DEFAULT_USE_ASM ON)
endif()

# On macOS, procps is not available
if(APPLE)
  set(DEFAULT_WITH_PROCPS OFF)
endif()

set(
  CURVE
  "${DEFAULT_CURVE}"
  CACHE
  STRING
  "Default curve: one of ALT_BN128, BN128, EDWARDS, MNT4, MNT6"
)

option(
  DEBUG
  "Enable debugging mode"
  OFF
)
option(
  LOWMEM
  "Limit the size of multi-exponentiation tables, for low-memory platforms"
  OFF
)
option(
  MULTICORE
  "Enable parallelized execution, using OpenMP"
  OFF
)
option(
  BINARY_OUTPUT
  "In serialization, output raw binary data (instead of decimal), which is smaller and faster."
  ON
)
option(
  MONTGOMERY_OUTPUT
  "Serialize Fp elements as their Montgomery representations (faster but not human-readable)"
  ON
)
option(
  USE_PT_COMPRESSION
  "Use point compression"
  ON
)
option(
  PROFILE_OP_COUNTS
  "Collect counts for field and curve operations"
  OFF
)
option(
  USE_MIXED_ADDITION
  "Convert each element of the key pair to affine coordinates"
  OFF
)

option(
  WITH_SUPERCOP
  "Support for Ed25519 signatures required by ADSNARK"
  ${DEFAULT_WITH_SUPERCOP}
)

option(
  WITH_PROCPS
  "Use procps for memory profiling"
  ${DEFAULT_WITH_PROCPS}
)

option(
  CPPDEBUG
  "Enable debugging of C++ STL (does not imply DEBUG)"
  OFF
)

option(
  PERFORMANCE
  "Enable link-time and aggressive optimizations"
  OFF
)

option(
  USE_ASM
  "Use architecture-specific optimized assembly code"
  ${DEFAULT_USE_ASM}
)

set(
  OPT_FLAGS
  ""
  CACHE
  STRING
  "Override C++ compiler optimization flags"
)

if("${DEPENDS_DIR}" STREQUAL "")
  set(
    DEPENDS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/depends"
    CACHE
    STRING
    "Optionally specify the dependency installation directory relative to the source directory (default: inside dependency folder)"
  )
else()
  set(DEPENDS_DIR "${DEPENDS_DIR}")
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  # Common compilation flags and warning configuration
  set(
    CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wextra -Wfatal-errors"
  )
  if("${MULTICORE}")
    if(APPLE)
      # Apple Clang requires -Xpreprocessor -fopenmp and Homebrew libomp
      execute_process(
        COMMAND brew --prefix libomp
        OUTPUT_VARIABLE LIBOMP_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(LIBOMP_PREFIX)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Xpreprocessor -fopenmp")
        include_directories("${LIBOMP_PREFIX}/include")
        link_directories("${LIBOMP_PREFIX}/lib")
        link_libraries(omp)
      else()
        message(WARNING "MULTICORE=ON but libomp not found. Install with: brew install libomp")
      endif()
    else()
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp")
    endif()
  endif()
  # Default optimizations flags (to override, use -DOPT_FLAGS=...)
  if("${OPT_FLAGS}" STREQUAL "")
    set(
      OPT_FLAGS
      "-ggdb3 -O2 -march=native -mtune=native"
    )
  endif()
endif()

set(
  CMAKE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS} ${OPT_FLAGS}"
)

# GMP
find_path(GMP_INCLUDE_DIR NAMES gmp.h)
find_library(GMP_LIBRARIES NAMES gmp libgmp)
find_library(GMPXX_LIBRARIES NAMES gmpxx libgmpxx)

include(FindPkgConfig)
pkg_check_modules(
  CRYPTO
  REQUIRED

  libcrypto
)
if(CRYPTO_LIBRARY_DIRS)
  link_directories(${CRYPTO_LIBRARY_DIRS})
endif()

if("${WITH_PROCPS}")
  pkg_check_modules(
    PROCPS
    REQUIRED

    libprocps
  )
else()
  add_definitions(
    -DNO_PROCPS
  )
endif()

# Enable Boost for program_options
FIND_PACKAGE( Boost 1.40 COMPONENTS program_options REQUIRED )
INCLUDE_DIRECTORIES( ${Boost_INCLUDE_DIR} )

add_definitions(
  -DCURVE_${CURVE}
)

enable_testing()

include_directories(src)

if(${CURVE} STREQUAL "BN128")
  add_definitions(
    -DBN_SUPPORT_SNARK=1
  )
endif()

if("${DEBUG}")
  add_definitions(-DDEBUG=1)
endif()

if("${LOWMEM}")
  add_definitions(-DLOWMEM=1)
endif()

if("${MULTICORE}")
  add_definitions(-DMULTICORE=1)
endif()

if("${BINARY_OUTPUT}")
  add_definitions(-DBINARY_OUTPUT)
endif()

if("${MONTGOMERY_OUTPUT}")
  add_definitions(-DMONTGOMERY_OUTPUT)
endif()

if(NOT "${USE_PT_COMPRESSION}")
  add_definitions(-DNO_PT_COMPRESSION=1)
endif()

if("${PROFILE_OP_COUNTS}")
  add_definitions(-DPROFILE_OP_COUNTS=1)
endif()

if("${USE_MIXED_ADDITION}")
  add_definitions(-DUSE_MIXED_ADDITION=1)
endif()

if("${CPPDEBUG}")
  add_definitions(-D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC)
endif()

if("${PERFORMANCE}")
  add_definitions(-DNDEBUG)
  set(
    CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -flto -fuse-linker-plugin"
  )
  set(
    CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -flto"
  )
endif()

if("${USE_ASM}")
  add_definitions(-DUSE_ASM)
endif()

if("${USE_LINKED_LIBRARIES}")
  # libfqfft
  find_path(LIBFQFFT_INCLUDE_DIR NAMES libfqfft)
  set(LIBFQFFT_DIRECTORY ${LIBFQFFT_INCLUDE_DIR}/libfqfft)
  include_directories(${LIBFQFFT_DIRECTORY})

  # libff
  find_path(LIBFF_INCLUDE_DIR NAMES libff)
  include_directories(${LIBFF_INCLUDE_DIR}/libff)
  find_library(LIBFF_LIBRARIES NAMES ff libff)
endif()

find_program(
  MARKDOWN

  markdown_py
  DOC "Path to markdown_py binary"
)
if(MARKDOWN-NOTFOUND)
else()
   add_custom_target(
     doc
     ${MARKDOWN} -f ${CMAKE_CURRENT_BINARY_DIR}/README.html -x toc -x extra --noisy ${CMAKE_CURRENT_SOURCE_DIR}/README.md
     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
     COMMENT "Translating from markdown to HTML" VERBATIM
   )
endif()

# Configure CCache if available
find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif(CCACHE_FOUND)

# Add a `make check` target that builds and tests
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND})

add_subdirectory(depends)

################################################################################
# Library, demos, profiling, and tests (merged from src/libsnark/CMakeLists.txt)
################################################################################

set(SNARK_EXTRALIBS)
if(${CURVE} STREQUAL "BN128")
  set(
    SNARK_EXTRALIBS

    ${SNARK_EXTRALIBS}
    ${PROCPS_LIBRARIES}
    zm
  )
endif()

add_library(
  snark
  STATIC

  src/libsnark/common/data_structures/integer_permutation.cpp
  src/libsnark/common/data_structures/set_commitment.cpp
  src/libsnark/common/default_types/r1cs_ppzkpcd_pp.cpp
  src/libsnark/common/default_types/tinyram_ppzksnark_pp.cpp
  src/libsnark/common/default_types/tinyram_zksnark_pp.cpp
  src/libsnark/common/routing_algorithms/as_waksman_routing_algorithm.cpp
  src/libsnark/common/routing_algorithms/benes_routing_algorithm.cpp
  src/libsnark/gadgetlib1/constraint_profiling.cpp
  src/libsnark/gadgetlib2/adapters.cpp
  src/libsnark/gadgetlib2/constraint.cpp
  src/libsnark/gadgetlib2/gadget.cpp
  src/libsnark/gadgetlib2/infrastructure.cpp
  src/libsnark/gadgetlib2/integration.cpp
  src/libsnark/gadgetlib2/pp.cpp
  src/libsnark/gadgetlib2/protoboard.cpp
  src/libsnark/gadgetlib2/variable.cpp
  src/libsnark/relations/circuit_satisfaction_problems/tbcs/tbcs.cpp
  src/libsnark/relations/ram_computations/memory/memory_store_trace.cpp
  src/libsnark/relations/ram_computations/memory/ra_memory.cpp
  src/libsnark/relations/ram_computations/rams/fooram/fooram_aux.cpp
  src/libsnark/relations/ram_computations/rams/tinyram/tinyram_aux.cpp
)
if("${USE_LINKED_LIBRARIES}")
  target_link_libraries(
    snark

    ff
    ${GMP_LIBRARIES}
    ${GMPXX_LIBRARIES}
    ${CRYPTO_LIBRARIES}
    ${PROCPS_LIBRARIES}
    ${LIBFF_LIBRARIES}
    ${SNARK_EXTRALIBS}
  )
else()
  target_link_libraries(
    snark

    ff
    ${GMP_LIBRARIES}
    ${GMPXX_LIBRARIES}
    ${CRYPTO_LIBRARIES}
    ${PROCPS_LIBRARIES}
    ${SNARK_EXTRALIBS}
  )
  target_include_directories(
    snark

    PUBLIC
    ${DEPENDS_DIR}/libff
    ${DEPENDS_DIR}/libfqfft
  )
endif()

install(
  DIRECTORY "src/libsnark/" DESTINATION "include/libsnark"
  FILES_MATCHING
  PATTERN "*.hpp"
  PATTERN "*.tcc"
  PATTERN "tests" EXCLUDE
  PATTERN "examples" EXCLUDE
)

install(
  TARGETS
  snark

  DESTINATION lib
)

if("${WITH_SUPERCOP}")
  add_library(
    snark_adsnark

    src/libsnark/common/default_types/r1cs_ppzkadsnark_pp.cpp
  )
  target_link_libraries(
    snark_adsnark

    snark
    snark_supercop
  )
  install(
    TARGETS
    snark_adsnark

    DESTINATION lib
  )
endif()

# Demos
add_executable(
  demo_arithmetization
  EXCLUDE_FROM_ALL

  src/libsnark/reductions/ram_to_r1cs/examples/demo_arithmetization.cpp
)
target_link_libraries(
  demo_arithmetization

  snark
  ${Boost_LIBRARIES}
)

add_executable(
  demo_ram_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/ram_ppzksnark/examples/demo_ram_ppzksnark.cpp
)
target_link_libraries(
  demo_ram_ppzksnark

  snark
  ${Boost_LIBRARIES}
)

add_executable(
  demo_ram_ppzksnark_generator
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/ram_ppzksnark/examples/demo_ram_ppzksnark_generator.cpp
)
target_link_libraries(
  demo_ram_ppzksnark_generator

  snark
  ${Boost_LIBRARIES}
)

add_executable(
  demo_ram_ppzksnark_prover
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/ram_ppzksnark/examples/demo_ram_ppzksnark_prover.cpp
)
target_link_libraries(
  demo_ram_ppzksnark_prover

  snark
  ${Boost_LIBRARIES}
)

add_executable(
  demo_ram_ppzksnark_verifier
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/ram_ppzksnark/examples/demo_ram_ppzksnark_verifier.cpp
)
target_link_libraries(
  demo_ram_ppzksnark_verifier

  snark
  ${Boost_LIBRARIES}
)

if("${WITH_SUPERCOP}")
  add_executable(
    demo_r1cs_ppzkadsnark
    EXCLUDE_FROM_ALL

    src/libsnark/zk_proof_systems/ppzkadsnark/r1cs_ppzkadsnark/examples/demo_r1cs_ppzkadsnark.cpp
  )
  target_link_libraries(
    demo_r1cs_ppzkadsnark

    snark_adsnark
  )
endif()

# Profiling
add_executable(
  profile_routing_algorithms
  EXCLUDE_FROM_ALL

  src/libsnark/common/routing_algorithms/profiling/profile_routing_algorithms.cpp
)
target_link_libraries(
  profile_routing_algorithms

  snark
)

add_executable(
  profile_routing_gadgets
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/gadgets/routing/profiling/profile_routing_gadgets.cpp
)
target_link_libraries(
  profile_routing_gadgets

  snark
)

add_executable(
  profile_r1cs_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_ppzksnark/profiling/profile_r1cs_ppzksnark.cpp
)
target_link_libraries(
  profile_r1cs_ppzksnark

  snark
)

add_executable(
  profile_r1cs_se_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_se_ppzksnark/profiling/profile_r1cs_se_ppzksnark.cpp
)
target_link_libraries(
  profile_r1cs_se_ppzksnark

  snark
)

add_executable(
  profile_r1cs_mp_ppzkpcd
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/pcd/r1cs_pcd/r1cs_mp_ppzkpcd/profiling/profile_r1cs_mp_ppzkpcd.cpp
)
target_link_libraries(
  profile_r1cs_mp_ppzkpcd

  snark
)

add_executable(
  profile_r1cs_sp_ppzkpcd
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/pcd/r1cs_pcd/r1cs_sp_ppzkpcd/profiling/profile_r1cs_sp_ppzkpcd.cpp
)
target_link_libraries(
  profile_r1cs_sp_ppzkpcd

  snark
)

add_executable(
  profile_bacs_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/bacs_ppzksnark/profiling/profile_bacs_ppzksnark.cpp
)
target_link_libraries(
  profile_bacs_ppzksnark

  snark
)

add_executable(
  profile_r1cs_gg_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_gg_ppzksnark/profiling/profile_r1cs_gg_ppzksnark.cpp
)
target_link_libraries(
  profile_r1cs_gg_ppzksnark

  snark
)

add_executable(
  profile_ram_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/ram_ppzksnark/profiling/profile_ram_ppzksnark.cpp
)
target_link_libraries(
  profile_ram_ppzksnark

  snark
)

add_executable(
  profile_tbcs_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/relations/circuit_satisfaction_problems/tbcs/examples/tbcs_examples.cpp
  src/libsnark/zk_proof_systems/ppzksnark/tbcs_ppzksnark/profiling/profile_tbcs_ppzksnark.cpp
)
target_link_libraries(
  profile_tbcs_ppzksnark

  snark
)

add_executable(
  profile_uscs_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/uscs_ppzksnark/profiling/profile_uscs_ppzksnark.cpp
)
target_link_libraries(
  profile_uscs_ppzksnark

  snark
)

add_executable(
  profile_ram_zksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/zksnark/ram_zksnark/profiling/profile_ram_zksnark.cpp
)
target_link_libraries(
  profile_ram_zksnark

  snark
  ${Boost_LIBRARIES}
)

# Tests
add_executable(
  common_routing_algorithms_test
  EXCLUDE_FROM_ALL

  src/libsnark/common/routing_algorithms/tests/test_routing_algorithms.cpp
)
target_link_libraries(
  common_routing_algorithms_test

  snark
)

add_executable(
  gadgetlib1_simple_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/tests/gadgetlib1_test.cpp
)
target_link_libraries(
  gadgetlib1_simple_test

  snark
  gtest_main
)

add_executable(
  gadgetlib1_fooram_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/gadgets/cpu_checkers/fooram/examples/test_fooram.cpp
)
target_link_libraries(
  gadgetlib1_fooram_test

  snark
)

add_executable(
  gadgetlib1_r1cs_ppzksnark_verifier_gadget_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/gadgets/verifiers/tests/test_r1cs_ppzksnark_verifier_gadget.cpp
)
target_link_libraries(
  gadgetlib1_r1cs_ppzksnark_verifier_gadget_test

  snark
)

add_executable(
  gadgetlib2_adapters_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib2/tests/adapters_UTEST.cpp
)
target_link_libraries(
  gadgetlib2_adapters_test

  snark
  gtest_main
)

add_executable(
  gadgetlib2_constraint_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib2/tests/constraint_UTEST.cpp
)
target_link_libraries(
  gadgetlib2_constraint_test

  snark
  gtest_main
)

add_executable(
  gadgetlib2_gadget_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib2/tests/gadget_UTEST.cpp
)
target_link_libraries(
  gadgetlib2_gadget_test

  snark
  gtest_main
)

add_executable(
  gadgetlib2_integration_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib2/examples/simple_example.hpp
  src/libsnark/gadgetlib2/tests/integration_UTEST.cpp
  src/libsnark/relations/constraint_satisfaction_problems/r1cs/examples/r1cs_examples.hpp
  src/libsnark/relations/constraint_satisfaction_problems/r1cs/examples/r1cs_examples.tcc
  src/libsnark/zk_proof_systems/ppzksnark/r1cs_ppzksnark/examples/run_r1cs_ppzksnark.hpp
  src/libsnark/zk_proof_systems/ppzksnark/r1cs_ppzksnark/examples/run_r1cs_ppzksnark.tcc
  src/libsnark/gadgetlib2/examples/simple_example.cpp
  src/libsnark/gadgetlib2/examples/simple_example.hpp
)
target_link_libraries(
  gadgetlib2_integration_test

  snark
  gtest_main
)

add_executable(
  gadgetlib2_protoboard_test
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib2/tests/protoboard_UTEST.cpp
)
target_link_libraries(
  gadgetlib2_protoboard_test

  snark
  gtest_main
)

add_executable(
  gadgetlib2_variable_test
  EXCLUDE_FROM_ALL


  src/libsnark/gadgetlib2/tests/variable_UTEST.cpp
)
target_link_libraries(
  gadgetlib2_variable_test

  snark
  gtest_main
)

add_executable(
  relations_qap_test
  EXCLUDE_FROM_ALL

  src/libsnark/relations/arithmetic_programs/qap/tests/test_qap.cpp
)
target_link_libraries(
  relations_qap_test

  snark
)

add_executable(
  relations_sap_test
  EXCLUDE_FROM_ALL

  src/libsnark/relations/arithmetic_programs/sap/tests/test_sap.cpp
)
target_link_libraries(
  relations_sap_test

  snark
)

add_executable(
  relations_ssp_test
  EXCLUDE_FROM_ALL

  src/libsnark/relations/arithmetic_programs/ssp/tests/test_ssp.cpp
)
target_link_libraries(
  relations_ssp_test

  snark
)

add_executable(
  zk_proof_systems_r1cs_mp_ppzkpcd_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/pcd/r1cs_pcd/r1cs_mp_ppzkpcd/tests/test_r1cs_mp_ppzkpcd.cpp
)
target_link_libraries(
  zk_proof_systems_r1cs_mp_ppzkpcd_test

  snark
)

add_executable(
  zk_proof_systems_r1cs_sp_ppzkpcd_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/pcd/r1cs_pcd/r1cs_sp_ppzkpcd/tests/test_r1cs_sp_ppzkpcd.cpp
)
target_link_libraries(
  zk_proof_systems_r1cs_sp_ppzkpcd_test

  snark
)

add_executable(
  zk_proof_systems_bacs_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/bacs_ppzksnark/tests/test_bacs_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_bacs_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_r1cs_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_ppzksnark/tests/test_r1cs_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_r1cs_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_r1cs_se_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_se_ppzksnark/tests/test_r1cs_se_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_r1cs_se_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_r1cs_gg_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_gg_ppzksnark/tests/test_r1cs_gg_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_r1cs_gg_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_r1cs_vc_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_vc_ppzksnark/tests/test_r1cs_vc_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_r1cs_vc_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_r1cs_uvc_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/tests/test_r1cs_uvc_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_r1cs_uvc_ppzksnark_test

  snark
)

add_executable(
  test_build_composed_cs
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/tests/test_build_composed_cs.cpp
)
target_link_libraries(
  test_build_composed_cs

  snark
)

add_executable(
  test_build_composed_assignment
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/tests/test_build_composed_assignment.cpp
)
target_link_libraries(
  test_build_composed_assignment

  snark
)

add_executable(
  test_uvc_generator
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/tests/test_uvc_generator.cpp
)
target_link_libraries(
  test_uvc_generator

  snark
)

add_executable(
  test_uvc_prover_verifier
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/tests/test_uvc_prover_verifier.cpp
)
target_link_libraries(
  test_uvc_prover_verifier

  snark
)

add_executable(
  bench_r1cs_uvc_ppzksnark
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/tests/bench_r1cs_uvc_ppzksnark.cpp
)
target_link_libraries(
  bench_r1cs_uvc_ppzksnark

  snark
)

add_executable(
  test_complex_circuits
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/tests/test_complex_circuits.cpp
)
target_link_libraries(
  test_complex_circuits

  snark
)

add_executable(
  bench_comparative
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/r1cs_uvc_ppzksnark/benchmarks/bench_comparative.cpp
)
target_link_libraries(
  bench_comparative

  snark
)

add_executable(
  zk_proof_systems_ram_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/ram_ppzksnark/tests/test_ram_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_ram_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_tbcs_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/relations/circuit_satisfaction_problems/tbcs/examples/tbcs_examples.cpp
  src/libsnark/zk_proof_systems/ppzksnark/tbcs_ppzksnark/tests/test_tbcs_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_tbcs_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_uscs_ppzksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/ppzksnark/uscs_ppzksnark/tests/test_uscs_ppzksnark.cpp
)
target_link_libraries(
  zk_proof_systems_uscs_ppzksnark_test

  snark
)

add_executable(
  zk_proof_systems_ram_zksnark_test
  EXCLUDE_FROM_ALL

  src/libsnark/zk_proof_systems/zksnark/ram_zksnark/tests/test_ram_zksnark.cpp
)
target_link_libraries(
  zk_proof_systems_ram_zksnark_test

  snark
)
add_executable(
  test_knapsack_gadget
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/gadgets/hashes/knapsack/tests/test_knapsack_gadget.cpp
)
target_link_libraries(
  test_knapsack_gadget

  snark
)

add_executable(
  test_merkle_tree_gadgets
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/gadgets/merkle_tree/tests/test_merkle_tree_gadgets.cpp
)
target_link_libraries(
  test_merkle_tree_gadgets

  snark
)

add_executable(
  test_set_commitment_gadget
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/gadgets/set_commitment/tests/test_set_commitment_gadget.cpp
)
target_link_libraries(
  test_set_commitment_gadget

  snark
)

add_executable(
  test_sha256_gadget
  EXCLUDE_FROM_ALL

  src/libsnark/gadgetlib1/gadgets/hashes/sha256/tests/test_sha256_gadget.cpp
)
target_link_libraries(
  test_sha256_gadget

  snark
)

include(CTest)
add_test(
  NAME common_routing_algorithms_test
  COMMAND common_routing_algorithms_test
)
add_test(
  NAME gadgetlib1_simple_test
  COMMAND gadgetlib1_simple_test
)
add_test(
  NAME gadgetlib1_r1cs_ppzksnark_verifier_gadget_test
  COMMAND gadgetlib1_r1cs_ppzksnark_verifier_gadget_test
)
add_test(
  NAME gadgetlib2_adapters_test
  COMMAND gadgetlib2_adapters_test
)
add_test(
  NAME gadgetlib2_constraint_test
  COMMAND gadgetlib2_constraint_test
)
add_test(
  NAME gadgetlib2_gadget_test
  COMMAND gadgetlib2_gadget_test
)
add_test(
  NAME gadgetlib2_integration_test
  COMMAND gadgetlib2_integration_test
)
add_test(
  NAME gadgetlib2_protoboard_test
  COMMAND gadgetlib2_protoboard_test
)
add_test(
  NAME gadgetlib2_variable_test
  COMMAND gadgetlib2_variable_test
)
add_test(
  NAME relations_qap_test
  COMMAND relations_qap_test
)
add_test(
  NAME relations_sap_test
  COMMAND relations_sap_test
)
add_test(
  NAME relations_ssp_test
  COMMAND relations_ssp_test
)
add_test(
  NAME zk_proof_systems_bacs_ppzksnark_test
  COMMAND zk_proof_systems_bacs_ppzksnark_test
)
add_test(
  NAME zk_proof_systems_r1cs_ppzksnark_test
  COMMAND zk_proof_systems_r1cs_ppzksnark_test
)
add_test(
  NAME zk_proof_systems_r1cs_se_ppzksnark_test
  COMMAND zk_proof_systems_r1cs_se_ppzksnark_test
)
add_test(
  NAME zk_proof_systems_r1cs_gg_ppzksnark_test
  COMMAND zk_proof_systems_r1cs_gg_ppzksnark_test
)
add_test(
  NAME zk_proof_systems_r1cs_vc_ppzksnark_test
  COMMAND zk_proof_systems_r1cs_vc_ppzksnark_test
)
add_test(
  NAME zk_proof_systems_r1cs_uvc_ppzksnark_test
  COMMAND zk_proof_systems_r1cs_uvc_ppzksnark_test
)
add_test(
  NAME test_build_composed_cs
  COMMAND test_build_composed_cs
)
add_test(
  NAME test_build_composed_assignment
  COMMAND test_build_composed_assignment
)
add_test(
  NAME test_uvc_generator
  COMMAND test_uvc_generator
)
add_test(
  NAME test_uvc_prover_verifier
  COMMAND test_uvc_prover_verifier
)
add_test(
  NAME test_complex_circuits
  COMMAND test_complex_circuits
)
add_test(
  NAME zk_proof_systems_ram_ppzksnark_test
  COMMAND zk_proof_systems_ram_ppzksnark_test
)
add_test(
  NAME zk_proof_systems_tbcs_ppzksnark_test
  COMMAND zk_proof_systems_tbcs_ppzksnark_test
)
add_test(
  NAME zk_proof_systems_uscs_ppzksnark_test
  COMMAND zk_proof_systems_uscs_ppzksnark_test
)
add_test(
  NAME test_knapsack_gadget
  COMMAND test_knapsack_gadget
)
add_test(
  NAME test_merkle_tree_gadgets
  COMMAND test_merkle_tree_gadgets
)
add_test(
  NAME test_set_commitment_gadget
  COMMAND test_set_commitment_gadget
)
add_test(
  NAME test_sha256_gadget
  COMMAND test_sha256_gadget
)

add_dependencies(check common_routing_algorithms_test)
add_dependencies(check gadgetlib1_simple_test)
add_dependencies(check gadgetlib1_r1cs_ppzksnark_verifier_gadget_test)
add_dependencies(check gadgetlib2_adapters_test)
add_dependencies(check gadgetlib2_constraint_test)
add_dependencies(check gadgetlib2_gadget_test)
add_dependencies(check gadgetlib2_integration_test)
add_dependencies(check gadgetlib2_protoboard_test)
add_dependencies(check gadgetlib2_variable_test)
add_dependencies(check relations_qap_test)
add_dependencies(check relations_sap_test)
add_dependencies(check relations_ssp_test)
add_dependencies(check zk_proof_systems_bacs_ppzksnark_test)
add_dependencies(check zk_proof_systems_r1cs_ppzksnark_test)
add_dependencies(check zk_proof_systems_r1cs_se_ppzksnark_test)
add_dependencies(check zk_proof_systems_r1cs_gg_ppzksnark_test)
add_dependencies(check zk_proof_systems_r1cs_vc_ppzksnark_test)
add_dependencies(check zk_proof_systems_r1cs_uvc_ppzksnark_test)
add_dependencies(check test_build_composed_cs)
add_dependencies(check test_build_composed_assignment)
add_dependencies(check test_uvc_generator)
add_dependencies(check test_uvc_prover_verifier)
add_dependencies(check test_complex_circuits)
add_dependencies(check zk_proof_systems_ram_ppzksnark_test)
add_dependencies(check zk_proof_systems_tbcs_ppzksnark_test)
add_dependencies(check zk_proof_systems_uscs_ppzksnark_test)
add_dependencies(check test_knapsack_gadget)
add_dependencies(check test_merkle_tree_gadgets)
add_dependencies(check test_set_commitment_gadget)
add_dependencies(check test_sha256_gadget)

# TODO (howardwu): Resolve runtime on targets:
# gadgetlib1_fooram_test, zk_proof_systems_r1cs_mp_ppzkpcd_test, zk_proof_systems_r1cs_sp_ppzkpcd_test, zk_proof_systems_ram_zksnark_test

# add_test(
#   NAME gadgetlib1_fooram_test
#   COMMAND gadgetlib1_fooram_test
# )
# add_test(
#   NAME zk_proof_systems_r1cs_mp_ppzkpcd_test
#   COMMAND zk_proof_systems_r1cs_mp_ppzkpcd_test
# )
# add_test(
#   NAME zk_proof_systems_r1cs_sp_ppzkpcd_test
#   COMMAND zk_proof_systems_r1cs_sp_ppzkpcd_test
# )
# add_test(
#   NAME zk_proof_systems_ram_zksnark_test
#   COMMAND zk_proof_systems_ram_zksnark_test
# )
# add_dependencies(check gadgetlib1_fooram_test)
# add_dependencies(check zk_proof_systems_r1cs_mp_ppzkpcd_test)
# add_dependencies(check zk_proof_systems_r1cs_sp_ppzkpcd_test)
# add_dependencies(check zk_proof_systems_ram_zksnark_test)

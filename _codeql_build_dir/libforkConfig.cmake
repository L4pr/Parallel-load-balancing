
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was install-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)

####################################################################################

include(CMakeFindDependencyMacro)

find_dependency(Threads)

if (0)
    find_dependency(REQUIRED hwloc)
elseif()
    find_dependency(PkgConfig)
    pkg_search_module(HWLOC REQUIRED IMPORTED_TARGET hwloc)
else()
    message(WARNING "This installation of libfork was built without NUMA support!")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/libforkTargets.cmake")

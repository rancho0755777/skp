## Add the dependencies of our library
get_filename_component(skp_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
include(CMakeFindDependencyMacro)

find_dependency(Threads REQUIRED)
if(@SKP_USES_OPENSSL@)
find_dependency(OpenSSL REQUIRED)
endif()

## Import the targets
if (NOT TARGET skp::skp)
    include("${skp_CMAKE_DIR}/skp-targets.cmake")
endif()

set(SKP_LIBRARIES skp::skp)
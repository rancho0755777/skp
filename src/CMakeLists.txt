FindThread()
if(ENABLE_SSL)
    FindOpenssl()
endif()

set(LIBNAME skp)
set(LIBNAMESPACE skp)

get_cpu_cores(OS_CPU_CORES)
configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/skp-config.h.cmake
    ${PROJECT_SOURCE_DIR}/include/skp/utils/config.h
        NEWLINE_STYLE UNIX)

aux_source_directory(mm SKP_SOURCES)
aux_source_directory(adt SKP_SOURCES)
aux_source_directory(utils SKP_SOURCES)
aux_source_directory(server SKP_SOURCES)
aux_source_directory(process SKP_SOURCES)
aux_source_directory(algorithm SKP_SOURCES)

file(GLOB_RECURSE SKP_HEADERS ${PROJECT_SOURCE_DIR}/include/skp/*.h)
source_group("Header Files" FILES ${SKP_HEADERS})
source_group("Source Files" FILES ${SKP_SOURCES})

add_library(${LIBNAME} ${SKP_SOURCES} ${SKP_HEADERS})
if(WERROR)
    target_compile_options(${LIBNAME} PRIVATE -Werror)
endif()

target_include_directories(${LIBNAME}
        PUBLIC
            $<INSTALL_INTERFACE:include>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
			${THREADS_INCLUDE_DIR})

target_link_libraries(${LIBNAME} PUBLIC threads_internal)
if(ENABLE_SSL)
    target_link_libraries(${LIBNAME} PUBLIC openssl_internal)
endif()

set(SKP_TARGETS ${LIBNAME})
set(SKP_USES_OPENSSL OFF)
set(SKP_INSTALL_CFGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${LIBNAME})

if (TARGET threads_internal)
    list(APPEND SKP_TARGETS threads_internal)
endif()

if (TARGET openssl_internal)
    set(SKP_USES_OPENSSL ON)
    list(APPEND SKP_TARGETS openssl_internal)
endif()

install(TARGETS ${SKP_TARGETS}
        EXPORT ${LIBNAME}-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/skp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

include(CMakePackageConfigHelpers)
configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/${LIBNAME}-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/${LIBNAME}-config.cmake
        INSTALL_DESTINATION ${SKP_INSTALL_CFGDIR}
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cmake/${LIBNAME}-config.cmake
        DESTINATION ${SKP_INSTALL_CFGDIR})

install(EXPORT ${LIBNAME}-targets
        FILE ${LIBNAME}-targets.cmake
        NAMESPACE ${LIBNAMESPACE}::
        DESTINATION ${SKP_INSTALL_CFGDIR})
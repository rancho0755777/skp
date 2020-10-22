macro(set_unix_options)
    message(STATUS "Setting options")

    add_compile_options(-ffunction-sections -fdata-sections
	    -Wall -Wno-format -Wno-unused-function -Wno-deprecated-declarations)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -Wstrict-prototypes -Wmissing-prototypes")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++11")

    if(BUILD_SHARED_LIBS)
        message(STATUS "build shared library")
    else()
        message(STATUS "build static library")
    endif()
    
    if(INSTALL_TEMP)
        set(INSTALL_ROOT "/tmp/install")
        set(CMAKE_INSTALL_PREFIX ${INSTALL_ROOT})
        set(CMAKE_INSTALL_LIBDIR ${INSTALL_ROOT}/lib)
        # 与 $<INSTALL_INTERFACE:include> 匹配
        set(CMAKE_INSTALL_INCLUDEDIR ${INSTALL_ROOT}/include)
    else()
        include(GNUInstallDirs)
    endif()

    message(STATUS "install libdir:" ${CMAKE_INSTALL_LIBDIR})
    message(STATUS "install includedir:" ${CMAKE_INSTALL_INCLUDEDIR})
endmacro()

function (get_cpu_cores OUTPUT_VAR_NAME)
    # Direct all the files into one folder
    set(WORK_FOLDER "${PROJECT_BINARY_DIR}/GetCpuCores")
    file(MAKE_DIRECTORY ${WORK_FOLDER})

    set(SRC ${PROJECT_SOURCE_DIR}/cmake/GetCpuCores.c)
    set(COMPILE_OUTPUT_FILE "${WORK_FOLDER}/GetCpuCores.log")

    try_run(RUN_RESULT COMPILE_RESULT
            "${WORK_FOLDER}"
            "${SRC}"
            COMPILE_OUTPUT_VARIABLE COMPILE_OUTPUT
            RUN_OUTPUT_VARIABLE RUN_OUTPUT
            )

    if(NOT COMPILE_RESULT)
        file(WRITE ${COMPILE_OUTPUT_FILE} ${COMPILE_OUTPUT})
        message(FATAL_ERROR "GetCpuCores failed compilation see ${COMPILE_OUTPUT_FILE}")
    endif()

    if("${RUN_RESULT}" STREQUAL "FAILED_TO_RUN")
        message(FATAL_ERROR "GetCpuCores failed to run executable")
    endif()

    message(STATUS "OS cpu cores ${RUN_OUTPUT}")
    set(${OUTPUT_VAR_NAME} ${RUN_OUTPUT} PARENT_SCOPE)
endfunction ()
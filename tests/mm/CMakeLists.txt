foreach(name IN ITEMS
		test-buddy_base
		test-slab_base
		test-buddy_perf
		test-glibc_alloc
		test-malloc
	)
	add_skp_executable(${name})
endforeach()

include (CheckIncludeFiles)

set(CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES} /usr/local/include")
check_include_files(jemalloc/jemalloc.h HAS_JEMALLOC)
check_include_files(gperftools/tcmalloc.h HAS_TCMALLOC)

if (HAS_JEMALLOC)
	add_executable(test-jemalloc_perf test-slab_perf.c)
	target_compile_definitions(test-jemalloc_perf PRIVATE JEMALLOC)
	target_link_libraries(test-jemalloc_perf skp jemalloc ${SKP_LINK_LIBS})
endif()

if (HAS_TCMALLOC)
	add_executable(test-tcmalloc_perf test-slab_perf.c)
	target_compile_definitions(test-tcmalloc_perf PRIVATE TCMALLOC)
	target_link_libraries(test-tcmalloc_perf skp tcmalloc ${SKP_LINK_LIBS})
endif()

add_executable(test-ptmalloc_perf test-slab_perf.c)
target_compile_definitions(test-ptmalloc_perf PRIVATE PTMALLOC)
target_link_libraries(test-ptmalloc_perf skp ${SKP_LINK_LIBS})

add_executable(test-slab_perf test-slab_perf.c)
target_compile_definitions(test-slab_perf PRIVATE UMALLOC)
target_link_libraries(test-slab_perf skp ${SKP_LINK_LIBS})

add_executable(test-umem_cache_perf test-slab_perf.c)
target_compile_definitions(test-umem_cache_perf PRIVATE UMALLOC UMEM_CACHE_TEST)
target_link_libraries(test-umem_cache_perf skp ${SKP_LINK_LIBS})

add_test(NAME buddy-benchmark COMMAND test-buddy_base)
add_test(NAME slab-benchmark COMMAND test-slab_base)
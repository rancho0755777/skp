foreach(name IN ITEMS
		test-utils
		test-urefs
		test-bitopt
		test-bitmap
		test-biglock
		test-spinlock
		test-mutex
		test-rwlock
		test-rwsem
		test-pthread_mutex
		test-pbuff
		test-random
		test-locker
		test-hash
		test-atomic
		test-fastcall
		test-fchg
	)
	add_skp_executable(${name})
endforeach()

if (NOT APPLE)
	add_skp_executable(test-pthread_rwsem)
endif()

# 仅用于编译CXX测试
add_executable(test-cxx test-cxx.cpp)
set_target_properties(test-cxx PROPERTIES INSTALL_RPATH "${LIBRARY_OUTPUT_PATH}")
target_link_libraries(test-cxx skp threads_internal)

add_test(NAME utils-benchmark COMMAND test-utils)
add_test(NAME urefs-benchmark COMMAND test-urefs)
add_test(NAME bitopt-benchmark COMMAND test-bitopt)
add_test(NAME bitmap-benchmark COMMAND test-bitmap)
add_test(NAME atomic-benchmark COMMAND test-atomic)
add_test(NAME biglock-benchmark COMMAND test-biglock)
add_test(NAME spinlock-benchmark COMMAND test-spinlock)
add_test(NAME mutex-benchmark COMMAND test-mutex)
add_test(NAME rwlock-benchmark COMMAND test-rwlock)
add_test(NAME rwsem-benchmark COMMAND test-rwsem)
add_test(NAME pbuff-benchmark COMMAND test-pbuff)
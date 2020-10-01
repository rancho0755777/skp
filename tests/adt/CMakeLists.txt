foreach(name IN ITEMS
		test-idt
		test-idr
		test-vector
		test-dict_iter
		test-dict_perf
		test-rbtree_perf
		test-rbtree_iter
		test-radix_tree_perf
		test-radix_tree_iter
		test-ring_ss
		test-ring_mm
		test-ring_sm
		test-ring_ms
		test-ring_base
	)
	add_skp_executable(${name})
endforeach()

add_test(NAME idt-benchmark COMMAND test-idt)
add_test(NAME idr-benchmark COMMAND test-idr)
add_test(NAME vector-benchmark COMMAND test-vector)
add_test(NAME dict-benchmark COMMAND test-dict_iter)
add_test(NAME dict-perf COMMAND test-dict_perf)
add_test(NAME radixtree-benchmark COMMAND test-radix_tree_iter)
add_test(NAME radixtree-perf COMMAND test-radix_tree_perf)
add_test(NAME rbtree-benchmark COMMAND test-rbtree_iter)
add_test(NAME rbtree-perf COMMAND test-rbtree_perf)
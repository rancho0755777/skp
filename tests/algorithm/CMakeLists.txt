foreach(name IN ITEMS
		test-list_sort
		test-heap_sort
	)
	add_skp_executable(${name})
endforeach()

add_test(NAME list_sort COMMAND test-list_sort)
add_test(NAME heap_sort COMMAND test-heap_sort)
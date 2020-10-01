foreach(name IN ITEMS
		test-completion
		test-create_wq
		test-delayed_work
		test-event
		test-order_wq
		test-rcu
		test-system_wq
		test-thread
		test-timer
		test-wait_bit
		test-wait
		test-signal
	)
	add_skp_executable(${name})
endforeach()

#add_test(NAME completion-benchmark COMMAND test-completion)
#add_test(NAME create_wq-benchmark COMMAND test-create_wq)
#add_test(NAME delayed_work-benchmark COMMAND test-delayed_work)
#add_test(NAME event-benchmark COMMAND test-event)
#add_test(NAME order_wq-benchmark COMMAND test-order_wq)
#add_test(NAME rcu-benchmark COMMAND test-rcu)
#add_test(NAME system_wq-benchmark COMMAND test-system_wq)
#add_test(NAME thread-benchmark COMMAND test-thread)
#add_test(NAME timer-benchmark COMMAND test-timer)
#add_test(NAME wait_bit-benchmark COMMAND test-wait_bit)
#add_test(NAME wait-benchmark COMMAND test-wait)
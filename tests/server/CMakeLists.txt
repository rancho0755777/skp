foreach(name IN ITEMS
		test-socket
		test-xprt_client
		test-xprt_server
	)
	add_skp_executable(${name})
endforeach()

if (ENABLE_SSL)
add_skp_executable(test-ssl_client)
add_skp_executable(test-ssl_server)
endif()
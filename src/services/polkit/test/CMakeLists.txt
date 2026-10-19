function (qs_test name)
	add_executable(${name} ${ARGN})
	# Static archives resolve left to right: the service library first, then
	# the core and window libraries it uses.
	target_link_libraries(${name} PRIVATE
		Qt::Quick Qt::Test quickshell-service-polkit
		quickshell-core quickshell-window quickshell-ui quickshell-io
		PkgConfig::glib PkgConfig::gobject PkgConfig::polkit_agent PkgConfig::polkit
	)
	add_test(NAME ${name} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" COMMAND $<TARGET_FILE:${name}>)
endfunction()

qs_test(polkit-listener listener.cpp ../listener.cpp)
qs_test(polkit-agent agent.cpp)

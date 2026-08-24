cmake_minimum_required(VERSION 3.20)

project(uinput_helper)

add_executable(uinput_test
	uinput_test.c
)

add_executable(input_server
	input_server.c
)

add_executable(input_client
	input_client.c
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)

add_executable(input_dump
	input_dump.c
)
target_link_libraries(input_dump PRIVATE PkgConfig::LIBEVDEV)



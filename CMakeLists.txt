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



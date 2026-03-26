# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PROGRAM_NAME server)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(server C CXX ASM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(GLOB_RECURSE HTTPD_CONTENT_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/html_files/*
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_LIST_DIR}/htmldata.c
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/makefsdata.py
    DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/makefsdata.py
        ${HTTPD_CONTENT_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    VERBATIM
)

add_custom_target(generate_http_content
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/htmldata.c
)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(server
    server.c
    dhcpserver.c
    dnserver.c
    )

add_dependencies(server generate_http_content)

target_include_directories(${PROGRAM_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_set_program_name(server "server")
pico_set_program_version(server "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(server 0)
pico_enable_stdio_usb(server 1)

# Add the standard library to the build
target_link_libraries(server
        pico_stdlib
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip
        pico_lwip_http
        )

# Add the standard include files to the build
target_include_directories(server PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(server 
)

pico_add_extra_outputs(server)

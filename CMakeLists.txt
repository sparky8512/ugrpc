cmake_minimum_required(VERSION 3.13)

project(ugrpc LANGUAGES C)

add_definitions(-std=gnu99)
# to test non-gcc portability (can do c11, except for some libubox headers):
#add_definitions(-std=c23 -D_POSIX_C_SOURCE=200809L)
add_definitions(-g3 -Wall -Wextra -Wno-unused-parameter)
find_path(ubox_headers libubox/avl.h)
find_path(json_c_headers json-c/json_object.h)
include_directories(${ubox_headers} ${json_c_headers})

add_executable(ugrpc)
target_sources(ugrpc PRIVATE src/pb_decode.c src/symbol_db.c src/fd_decode.c
    src/reflection_v1_fd.c src/jobj_xcode.c src/decode_common.c src/main.c)
set_target_properties(ugrpc PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
set_target_properties(ugrpc PROPERTIES INTERPROCEDURAL_OPTIMIZATION_DEBUG FALSE)
find_library(libjson_c json-c)
find_library(libubox ubox)
target_link_libraries(ugrpc ${libjson_c} ${libubox})

option(ENABLE_UCLIENT_SERVICE "support for service command using uclient" ON)
if(ENABLE_UCLIENT_SERVICE)
    find_path(uclient_headers libubox/uclient.h)
    include_directories(${uclient_headers})
    add_definitions(-DUCLIENT_SERVICE=1)
    target_sources(ugrpc PRIVATE src/uclient_service.c)
    find_library(libuclient uclient)
    target_link_libraries(ugrpc ${libuclient})
endif()

option(ENABLE_PRETTY_COLOR "use color when pretty printing JSON" OFF)
if(ENABLE_PRETTY_COLOR)
    add_definitions(-DUSE_PRETTY_COLOR=1)
endif()

option(ENABLE_LEAK_CHECK "don't intentionally leak any memory on exit" OFF)
if(ENABLE_LEAK_CHECK)
    add_definitions(-DCLEANUP_SYMBOL_DB=1)
endif()

install(TARGETS ugrpc)

cmake_minimum_required(VERSION 3.20)
project(shibadb_external_consumer LANGUAGES C CXX)

find_package(ShibaDB 0.1 CONFIG REQUIRED)

add_executable(consumer_shared main.c)
target_link_libraries(consumer_shared PRIVATE ShibaDB::shared)
target_compile_features(consumer_shared PRIVATE c_std_11)

add_executable(consumer_static main.c)
target_link_libraries(consumer_static PRIVATE ShibaDB::core)
target_compile_features(consumer_static PRIVATE c_std_11)

add_executable(consumer_cpp main.cpp)
target_link_libraries(consumer_cpp PRIVATE ShibaDB::shared)
target_compile_features(consumer_cpp PRIVATE cxx_std_17)

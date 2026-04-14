cmake_minimum_required(VERSION 3.20)
project(webtransport_examples CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(webtransport CONFIG REQUIRED)

add_executable(wt_server server.cc)
target_link_libraries(wt_server PRIVATE webtransport::webtransport)

add_executable(wt_client client.cc)
target_link_libraries(wt_client PRIVATE webtransport::webtransport)
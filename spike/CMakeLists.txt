cmake_minimum_required(VERSION 3.21)
project(olo_vcpkg_spike CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)

add_executable(spike_consumer main.cpp)
target_link_libraries(spike_consumer PRIVATE glm::glm spdlog::spdlog Jolt::Jolt)

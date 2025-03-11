# Copyright 2025 Stefan Zellmann
# SPDX-License-Identifier: Apache-2.0

project(anari-multi-threading-test LANGUAGES C CXX)
cmake_minimum_required(VERSION 3.12)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(anari 0.13.0 REQUIRED)

add_executable(${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE external)
target_sources(${PROJECT_NAME} PRIVATE main.cpp)
target_link_libraries(${PROJECT_NAME} PUBLIC anari::anari)

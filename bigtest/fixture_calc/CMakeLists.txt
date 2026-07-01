cmake_minimum_required(VERSION 3.21)
project(calc_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(calc_app
    src/main.cpp
    src/calc.cpp
)

cmake_minimum_required(VERSION 3.22)
project(OpenSection VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OPENSECTION_TOOLS "Build command-line renderer / benchmark" ON)

add_library(opensection_engine STATIC
  engine/src/Engine.cpp
  engine/src/Model.cpp
  engine/src/BuiltinModel.cpp)
target_include_directories(opensection_engine PUBLIC engine/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(opensection_engine PRIVATE -O3 -ffast-math -Wall -Wextra)
elseif(MSVC)
  target_compile_options(opensection_engine PRIVATE /O2 /fp:fast)
endif()
set_target_properties(opensection_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(OPENSECTION_TOOLS)
  add_executable(opensection-render tools/render/main.cpp)
  target_link_libraries(opensection-render PRIVATE opensection_engine)
endif()

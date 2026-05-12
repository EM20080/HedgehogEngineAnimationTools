cmake_minimum_required(VERSION 3.21)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "X86 architecture is not supported!")
endif()

project(HedgehogEngineAnimationTools VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(HEAT_HAVOKLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/HavokLib-blender")

set(PYTHON_MODULE OFF CACHE BOOL "Build HavokLib Python module." FORCE)
set(TOOLSET OFF CACHE BOOL "Build HavokLib tools." FORCE)
set(ODR_TEST OFF CACHE BOOL "Enable HavokLib ODR tests." FORCE)
set(NO_OBJECTS OFF CACHE BOOL "Disable HavokLib objects." FORCE)

add_subdirectory("${HEAT_HAVOKLIB_DIR}" "${CMAKE_BINARY_DIR}/extern/HavokLib-blender")

if(NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(spike-interface INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
endif()

if(NOT WIN32)
  foreach(HEAT_PIC_TARGET IN ITEMS havok-objects spike-objects pugixml-objects)
    if(TARGET ${HEAT_PIC_TARGET})
      set_target_properties(${HEAT_PIC_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
  endforeach()
endif()

add_subdirectory(src "${CMAKE_BINARY_DIR}/cmake/heat-src")

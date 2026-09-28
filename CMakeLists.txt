cmake_minimum_required(VERSION 3.16)
project(voltage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# JNI headers (host JDK 21, same major as MC 1.21.4 runtime)
find_path(JNI_INCLUDE_DIR jni.h
  PATHS /usr/lib/jvm/java-21-openjdk-amd64/include
        /usr/lib/jvm/java-17-openjdk-amd64/include
  NO_DEFAULT_PATH)
if(NOT JNI_INCLUDE_DIR)
  message(FATAL_ERROR "jni.h not found. Install openjdk-21-jdk-headless")
endif()
message(STATUS "JNI include: ${JNI_INCLUDE_DIR}")

# Vendored Dear ImGui (ClickGUI, roadmap phase 3)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
)

add_library(voltage SHARED
  src/inject.cpp
  src/jvm.cpp
  src/gl_hook.cpp
  src/esp.cpp
  src/menu.cpp
  ${IMGUI_SOURCES}
)

target_include_directories(voltage PRIVATE
  src
  ${JNI_INCLUDE_DIR}
  ${JNI_INCLUDE_DIR}/linux
  ${IMGUI_DIR}
)
target_compile_definitions(voltage PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(voltage PRIVATE dl pthread GL)
target_compile_options(voltage PRIVATE -Wall -Wextra -fvisibility=hidden)

# ImGui is third-party: silence its warnings, keep ours strict.
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

set_target_properties(voltage PROPERTIES OUTPUT_NAME voltage PREFIX "lib" SUFFIX ".so")

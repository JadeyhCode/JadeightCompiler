cmake_minimum_required(VERSION 3.22)
project(JadeightCompiler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFFI REQUIRED libffi)
find_package(Threads REQUIRED)

set(JADEIGHT_ASM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../JadeightAbstractionCode)
set(JADEIGHT_VM_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../Jadeight2ReWrite)

# ---------- j8c：Jadeight C 风格编译器（协议 / ECS / 优化） ----------
# 目标 ISA = ISA v3（合并式指令集，定义在 Jadeight2ReWrite/isa.hpp）
add_executable(j8c
    src/driver.cpp
    src/lexer.cpp
    src/parser.cpp
    src/sema.cpp
    src/optimize.cpp
    src/codegen.cpp
)
target_include_directories(j8c PRIVATE src ${JADEIGHT_ASM_DIR} ${JADEIGHT_VM_DIR})
target_compile_options(j8c PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------- j8run：在 ISA v3 VM 上运行 .bc 模块的宿主运行器 ----------
add_executable(j8run runtime/j8run.cpp)
target_include_directories(j8run PRIVATE ${JADEIGHT_VM_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(j8run PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(j8run PRIVATE ${LIBFFI_LIBRARIES} dl Threads::Threads)

add_custom_target(tools DEPENDS j8c j8run)

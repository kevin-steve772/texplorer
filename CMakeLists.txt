cmake_minimum_required(VERSION 3.16)
project(TExplorer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSESW REQUIRED ncursesw)

add_executable(TExplorer src/main.cpp)
target_include_directories(TExplorer PRIVATE ${NCURSESW_INCLUDE_DIRS})
target_compile_options(TExplorer PRIVATE ${NCURSESW_CFLAGS_OTHER} -Wall -Wextra)
target_link_directories(TExplorer PRIVATE ${NCURSESW_LIBRARY_DIRS})
target_link_libraries(TExplorer PRIVATE ${NCURSESW_LIBRARIES})

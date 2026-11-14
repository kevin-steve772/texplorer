cmake_minimum_required(VERSION 3.16)
project(TExplorer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(TExplorer src/main.cpp src/platform.cpp)

if(WIN32)

    # PDCursesMod provides a wide-character, ncurses-compatible curses
    # implementation for the Windows console.
    include(FetchContent)

    set(PDC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(PDC_WIDE ON CACHE BOOL "" FORCE)
    set(PDC_UTF8 ON CACHE BOOL "" FORCE)
    set(PDC_SDL2_BUILD OFF CACHE BOOL "" FORCE)
    set(PDC_SDL2_DEPS_BUILD OFF CACHE BOOL "" FORCE)
    set(PDC_GL_BUILD OFF CACHE BOOL "" FORCE)
    set(PDC_X11_BUILD OFF CACHE BOOL "" FORCE)

    set(TEXPLORER_PDCURSES_SOURCE "" CACHE PATH
        "Path to a local PDCursesMod source tree (optional)")

    if(TEXPLORER_PDCURSES_SOURCE)
        # Use a recent PDCursesMod checkout (the v4.5.4 release tag is not
        # relocatable: its CMake files read curses.h and modules relative
        # to CMAKE_SOURCE_DIR).
        list(APPEND CMAKE_MODULE_PATH "${TEXPLORER_PDCURSES_SOURCE}/cmake")
        add_subdirectory("${TEXPLORER_PDCURSES_SOURCE}"
                         "${CMAKE_BINARY_DIR}/pdcurses")
    else()
        # Pinned to a commit after v4.5.4: the v4.5.4 CMake files use
        # CMAKE_SOURCE_DIR for their module path, which breaks when the
        # project is vendored via FetchContent.
        FetchContent_Declare(pdcurses
            GIT_REPOSITORY https://github.com/Bill-Gray/PDCursesMod.git
            GIT_TAG 0b262da44c861c0012a735d3456f5a33e53b9708
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(pdcurses)
    endif()

    target_compile_definitions(TExplorer PRIVATE
        PDC_WIDE
        PDC_FORCE_UTF8
        PDC_NCMOUSE
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0601
        WINVER=0x0601)

    if(MSVC)
        target_compile_options(TExplorer PRIVATE /utf-8 /W4)
    else()
        target_compile_options(TExplorer PRIVATE -Wall -Wextra)
    endif()

    target_link_libraries(TExplorer PRIVATE
        wincon_pdcursesstatic
        shell32
        ole32
        user32
        advapi32
        winmm)

else()

    if(APPLE)
        find_package(Curses REQUIRED)

        target_include_directories(TExplorer PRIVATE ${CURSES_INCLUDE_DIRS})
        target_compile_options(TExplorer PRIVATE -Wall -Wextra)
        target_link_libraries(TExplorer PRIVATE ${CURSES_LIBRARIES})
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(NCURSESW QUIET ncursesw)
        endif()

        if(NCURSESW_FOUND)
            target_include_directories(TExplorer PRIVATE ${NCURSESW_INCLUDE_DIRS})
            target_compile_options(TExplorer PRIVATE ${NCURSESW_CFLAGS_OTHER} -Wall -Wextra)
            target_link_directories(TExplorer PRIVATE ${NCURSESW_LIBRARY_DIRS})
            target_link_libraries(TExplorer PRIVATE ${NCURSESW_LIBRARIES})
        else()
            find_package(Curses REQUIRED)

            target_include_directories(TExplorer PRIVATE ${CURSES_INCLUDE_DIRS})
            target_compile_options(TExplorer PRIVATE -Wall -Wextra)
            target_link_libraries(TExplorer PRIVATE ${CURSES_LIBRARIES})
        endif()
    endif()

endif()

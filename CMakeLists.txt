cmake_minimum_required(VERSION 3.20)
project(EverythingSearchExplorer LANGUAGES CXX RC)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project builds x64 only (Explorer is 64-bit on Windows 11).")
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything lands in dist/bin so install-nse.ps1 has a single source folder.
# The per-config variants keep the same path with the multi-config Visual Studio
# generator; the base variables cover single-config generators.
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/dist/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/dist/bin")
endforeach()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/bin")

add_subdirectory(nse)

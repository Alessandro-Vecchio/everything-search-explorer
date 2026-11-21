add_library(EverythingSearchNse SHARED
    dllmain.cpp
    pidl.cpp
    bridge.cpp
    folder.cpp
    version.rc
    nse.def)

target_include_directories(EverythingSearchNse PRIVATE "${CMAKE_SOURCE_DIR}/shared")
target_compile_definitions(EverythingSearchNse PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(EverythingSearchNse PRIVATE shell32 shlwapi ole32 oleaut32 uuid propsys gdi32)

if (MSVC)
    target_compile_options(EverythingSearchNse PRIVATE /W4 /utf-8 /EHsc)
    # Reproducible binary: the published SHA256 must describe the same bytes
    # every time the same source is built.
    target_link_options(EverythingSearchNse PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/nse.def" /Brepro)
else ()
    target_compile_options(EverythingSearchNse PRIVATE -Wall -Wextra)
endif ()

set_target_properties(EverythingSearchNse PROPERTIES OUTPUT_NAME "EverythingSearchNse")

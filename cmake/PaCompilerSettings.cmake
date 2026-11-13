# Shared compiler settings for the Linux and Windows builds.
# Included from the top-level CMakeLists.txt after project().

if(MSVC)
    # /utf-8 is mandatory: the sources contain UTF-8 Chinese text.
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()

if(WIN32)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Windows builds require an x64 MSVC toolchain and matching Qt kit.")
    endif()
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    # Keep all executables (app and tests) in a predictable bin/<Config> folder.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

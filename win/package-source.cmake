# Run from any directory: cmake -P win/package-source.cmake
# Packages the working tree, including uncommitted/new source files.
cmake_minimum_required(VERSION 3.16)
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB_RECURSE sources RELATIVE "${repo}"
    "${repo}/src/*.cpp" "${repo}/src/*.h" "${repo}/src/*.ui"
    "${repo}/tests/*.cpp" "${repo}/tests/*.h"
    "${repo}/third_party/Spectrum/include/*.h"
    "${repo}/third_party/concurrentqueue/*.h"
    "${repo}/docs/*.md")
list(APPEND sources
    CMakeLists.txt README.md LICENSE CHANGELOG.md CMakePresets.json
    .gitignore .gitattributes .clang-format .editorconfig
    .github/workflows/ci.yml
    depth_palettes.png
    cmake/PaCompilerSettings.cmake cmake/PaTestHelpers.cmake
    linux/build.sh linux/README.md linux/lib/README.md
    third_party/Spectrum/README.md third_party/Spectrum/SHA256SUMS.txt
    win/README.md win/build.ps1 win/package-source.cmake win/lib/README.md)
list(SORT sources)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
file(MAKE_DIRECTORY "${repo}/win/source-dist")
set(stage "${repo}/win/source-dist/source-stage-${suffix}")
set(package "${stage}/PA_Microscope")
file(MAKE_DIRECTORY "${package}")
set(manifest "")
foreach(relative IN LISTS sources)
    get_filename_component(parent "${relative}" DIRECTORY)
    file(COPY "${repo}/${relative}" DESTINATION "${package}/${parent}")
    file(SHA256 "${repo}/${relative}" digest)
    string(APPEND manifest "${digest}  ${relative}\n")
endforeach()
file(WRITE "${package}/SOURCE_SHA256SUMS.txt" "${manifest}")
set(archive "${repo}/win/source-dist/PA_Microscope_Source.zip")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --format=zip PA_Microscope
    WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Source packaging failed: ${result}")
endif()
# Keep the uniquely named staging copy as a reviewable snapshot; no deletion.
file(SHA256 "${archive}" archive_hash)
file(WRITE "${archive}.sha256" "${archive_hash}  PA_Microscope_Source.zip\n")
message(STATUS "Source archive: ${archive}")
message(STATUS "SHA256: ${archive_hash}")
message(STATUS "Cross-platform source package (Linux + Windows).")
message(STATUS "Spectrum/Qt binaries, build caches, settings and acquisitions are NOT included.")

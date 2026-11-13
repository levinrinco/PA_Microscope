# Helpers shared by the offline test targets.

# The tests link Qt, so ctest has to find Qt6Core.dll and friends on Windows.
# Prepend Qt's bin directory instead of relying on the caller's PATH.
function(pa_qt_test_path target)
    if(NOT WIN32)
        return()
    endif()
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        set_property(TEST ${target} PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    else()
        set_property(TEST ${target} PROPERTY ENVIRONMENT
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\\;$ENV{PATH}")
    endif()
endfunction()

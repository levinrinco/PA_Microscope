cmake_minimum_required(VERSION 3.16)
project(PA_Microscope VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PA_BUILD_APP "Build the hardware application (requires Spectrum SDK binaries)" ON)

include(cmake/PaCompilerSettings.cmake)
include(cmake/PaTestHelpers.cmake)

# ---- Qt 6.x ----
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    OpenGL
    OpenGLWidgets
    SerialPort
)

# ---- Spectrum SDK ----
# Both platforms use the same vendor headers and application sources.
set(SPECTRUM_SDK_DIR     "${CMAKE_SOURCE_DIR}/third_party/Spectrum")
set(SPECTRUM_INCLUDE_DIR "${SPECTRUM_SDK_DIR}/include")
if(PA_BUILD_APP)
if(WIN32)
    set(SPECTRUM_WINDOWS_LIB_DIR "${CMAKE_SOURCE_DIR}/win/lib" CACHE PATH
        "Directory containing the x64 Spectrum import library and runtime DLL")
    set(SPECTRUM_LIB "${SPECTRUM_WINDOWS_LIB_DIR}/spcm_win64_msvcpp.lib")
    set(SPECTRUM_DLL "${SPECTRUM_WINDOWS_LIB_DIR}/spcm_win64.dll")
    foreach(sdk_file IN ITEMS "${SPECTRUM_LIB}" "${SPECTRUM_DLL}")
        if(NOT EXISTS "${sdk_file}")
            message(FATAL_ERROR "Missing Spectrum SDK file: ${sdk_file}\nSee win/README.md, or use -DPA_BUILD_APP=OFF for offline tests.")
        endif()
    endforeach()
    add_library(SpectrumSDK SHARED IMPORTED)
    set_target_properties(SpectrumSDK PROPERTIES
        IMPORTED_IMPLIB "${SPECTRUM_LIB}"
        IMPORTED_LOCATION "${SPECTRUM_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${SPECTRUM_INCLUDE_DIR}")
else()
set(SPECTRUM_LIB_DIR     "${CMAKE_SOURCE_DIR}/linux/lib")
set(SPECTRUM_LIB         "${SPECTRUM_LIB_DIR}/libspcm_linux.so")

if(NOT EXISTS "${SPECTRUM_LIB}")
    message(FATAL_ERROR
        "Spectrum SDK library not found: ${SPECTRUM_LIB}\n"
        "Fetch libspcm_linux.so from the official Spectrum SDK and place it in "
        "${SPECTRUM_LIB_DIR}/ (see linux/README.md).")
endif()

add_library(SpectrumSDK UNKNOWN IMPORTED)
set_target_properties(SpectrumSDK PROPERTIES
    IMPORTED_LOCATION "${SPECTRUM_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SPECTRUM_INCLUDE_DIR}"
)
endif()
endif()

# ---- OpenGL ----
find_package(OpenGL REQUIRED)

# ---- Threads (std::thread / std::mutex) ----
find_package(Threads REQUIRED)

# ---- Sources ----
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/ScanController.cpp
    src/FpgaSerial.cpp
    src/SerialDebugDialog.cpp
    src/SpectrumFifo.cpp
    src/DataPipeline.cpp
    src/AlineWidget.cpp
    src/BscanWidget.cpp
    src/CscanWidget.cpp
    src/DepthRulerWidget.cpp
    src/FileWriter.cpp
    src/ConfigManager.cpp
    src/ColorBar.cpp
    src/ColorMap.cpp
    src/DepthColorBar.cpp
    src/DepthEncode.cpp
    src/Logger.cpp
    src/TrajectoryLoader.cpp
)

set(HEADERS
    src/MainWindow.h
    src/ScanController.h
    src/FpgaSerial.h
    src/SerialDebugDialog.h
    src/SpectrumFifo.h
    src/DataPipeline.h
    src/AlineWidget.h
    src/BscanWidget.h
    src/CscanWidget.h
    src/DepthRulerWidget.h
    src/FileWriter.h
    src/ConfigManager.h
    src/ColorBar.h
    src/ColorMap.h
    src/DepthColorBar.h
    src/DepthEncode.h
    src/Types.h
    src/Logger.h
    src/TrajectoryLoader.h
)

# ---- Executable ----
if(PA_BUILD_APP)
# WIN32 subsystem on Windows: a GUI application without a stray console window.
# Diagnostic output goes to <exe dir>/logs/ (see Logger), not to stdout.
if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})
else()
    add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
endif()

target_link_libraries(${PROJECT_NAME}
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::SerialPort
    SpectrumSDK
    OpenGL::GL
    Threads::Threads
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${SPECTRUM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/concurrentqueue
)

# ---- Runtime library search path ----
# libspcm_linux.so lives in linux/lib; embed its directory in the build-tree
# RPATH so the executable runs without LD_LIBRARY_PATH. The install RPATH keeps
# a portable layout working when the binary and lib/ sit side by side.
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SpectrumSDK>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        VERBATIM)
else()
set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "${SPECTRUM_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN/../lib"
)
endif()
endif()

# ---- Offline core verification (no acquisition card required) ----
include(CTest)
if(BUILD_TESTING)
    add_executable(PA_CoreTests
        tests/CoreTests.cpp
        src/FileWriter.cpp
        src/TrajectoryLoader.cpp
        src/ColorMap.cpp
        src/DepthEncode.cpp
        src/ConfigManager.cpp
        src/FileWriter.h
        src/TrajectoryLoader.h
        src/ColorMap.h
        src/DepthEncode.h
        src/ConfigManager.h
        src/Types.h
    )
    target_include_directories(PA_CoreTests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR})
    target_link_libraries(PA_CoreTests Qt6::Core Qt6::Gui Threads::Threads)
    add_test(NAME PA_CoreTests COMMAND PA_CoreTests)
    pa_qt_test_path(PA_CoreTests)

    add_executable(PA_PipelineTests
        tests/PipelineTests.cpp
        src/DataPipeline.cpp src/DataPipeline.h
        src/ScanController.cpp src/ScanController.h
        src/FpgaSerial.cpp src/FpgaSerial.h
        src/FileWriter.cpp src/FileWriter.h
        src/ColorMap.cpp
        src/DepthEncode.cpp src/DepthEncode.h
    )
    target_include_directories(PA_PipelineTests PRIVATE
        ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/concurrentqueue)
    # PipelineTests substitutes its own SpectrumFifo fake, so the Spectrum SDK
    # is deliberately not linked here.
    target_link_libraries(PA_PipelineTests Qt6::Core Qt6::Gui Qt6::SerialPort Threads::Threads)
    add_test(NAME PA_PipelineTests COMMAND PA_PipelineTests)
    pa_qt_test_path(PA_PipelineTests)
    set_tests_properties(PA_PipelineTests PROPERTIES TIMEOUT 30)

    add_executable(PA_BscanTests tests/BscanTests.cpp
        src/BscanWidget.cpp src/BscanWidget.h src/ColorMap.cpp
        src/CscanWidget.cpp src/CscanWidget.h src/DepthEncode.cpp src/DepthEncode.h)
    target_include_directories(PA_BscanTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PA_BscanTests Qt6::Widgets Qt6::OpenGLWidgets OpenGL::GL)
    add_test(NAME PA_BscanTests COMMAND PA_BscanTests)
    pa_qt_test_path(PA_BscanTests)
    set_tests_properties(PA_BscanTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

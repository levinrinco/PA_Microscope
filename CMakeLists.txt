cmake_minimum_required(VERSION 3.16)
project(PA_Spiral_PAM VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

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
set(SPECTRUM_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/Spectrum")
set(SPECTRUM_INCLUDE_DIR "${SPECTRUM_SDK_DIR}/include")
set(SPECTRUM_LIB_DIR "${SPECTRUM_SDK_DIR}/lib")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SPECTRUM_LIB "${SPECTRUM_LIB_DIR}/spcm_win64_msvcpp.lib")
else()
    set(SPECTRUM_LIB "${SPECTRUM_LIB_DIR}/spcm_win32_msvcpp.lib")
endif()

add_library(SpectrumSDK STATIC IMPORTED)
set_target_properties(SpectrumSDK PROPERTIES
    IMPORTED_LOCATION "${SPECTRUM_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SPECTRUM_INCLUDE_DIR}"
)

# ---- OpenGL ----
if(WIN32)
    set(OPENGL_LIB opengl32)
else()
    find_package(OpenGL REQUIRED)
    set(OPENGL_LIB OpenGL::GL)
endif()

# ---- Sources ----
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/ScanController.cpp
    src/FpgaSerial.cpp
    src/SpectrumFifo.cpp
    src/DataPipeline.cpp
    src/AlineWidget.cpp
    src/CscanWidget.cpp
    src/FileWriter.cpp
    src/ConfigManager.cpp
    src/ColorBar.cpp
    src/ColorMap.cpp
    src/Logger.cpp
    src/TrajectoryLoader.cpp
)

set(HEADERS
    src/MainWindow.h
    src/ScanController.h
    src/FpgaSerial.h
    src/SpectrumFifo.h
    src/DataPipeline.h
    src/AlineWidget.h
    src/CscanWidget.h
    src/FileWriter.h
    src/ConfigManager.h
    src/ColorBar.h
    src/Types.h
    src/Logger.h
    src/TrajectoryLoader.h
)

# ---- Executable ----
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})

target_link_libraries(${PROJECT_NAME}
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::SerialPort
    SpectrumSDK
    ${OPENGL_LIB}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${SPECTRUM_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/concurrentqueue
)

# ---- Compiler settings (MSVC) ----
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# ---- Deploy ----
if(WIN32)
    add_custom_target(deploy
        COMMAND "${CMAKE_SOURCE_DIR}/deploy.bat"
        COMMENT "Packaging portable distribution to dist/"
        DEPENDS ${PROJECT_NAME}
    )
endif()

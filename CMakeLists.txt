cmake_minimum_required(VERSION 3.20)
project(RtmpDesktopStreamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W3 /MP /utf-8 /EHsc)
    add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
endif()

set(SOURCES
    src/main.cpp
    src/UI.cpp
    src/DebugLog.cpp
    src/DesktopCapture.cpp
    src/AudioCapture.cpp
    src/VideoEncoder.cpp
    src/AudioEncoder.cpp
    src/FlvMuxer.cpp
    src/RtmpProtocol.cpp
    src/RtmpServer.cpp
)

set(HEADERS
    src/Common.h
    src/UI.h
    src/DebugLog.h
    src/DesktopCapture.h
    src/AudioCapture.h
    src/VideoEncoder.h
    src/AudioEncoder.h
    src/FlvMuxer.h
    src/RtmpProtocol.h
    src/RtmpServer.h
)

add_executable(RtmpDesktopStreamer WIN32 ${SOURCES} ${HEADERS})

target_link_libraries(RtmpDesktopStreamer PRIVATE
    d3d11
    dxgi
    dxguid
    mf
    mfplat
    mfreadwrite
    mfuuid
    wmcodecdspuuid
    ole32
    uuid
    user32
    gdi32
    comdlg32
    comctl32
    avrt
    winmm
    ws2_32
    iphlpapi
)

set_target_properties(RtmpDesktopStreamer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    WIN32_EXECUTABLE TRUE
)

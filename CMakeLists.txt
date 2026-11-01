cmake_minimum_required(VERSION 3.20)
project(BehringerUM2ASIODriver VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Caminho do Steinberg ASIO SDK (fora do repo; sobrescreva com -DASIO_SDK_DIR=...)
set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk/asiosdk_2.3.3_2019-06-14/common"
    CACHE PATH "Pasta common/ do Steinberg ASIO SDK")

# Segredo HMAC da licenca: header local, gitignorado (formato em src/UM2LicenseSecret.example.h)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/UM2LicenseSecret.h")
    message(FATAL_ERROR "src/UM2LicenseSecret.h ausente. Copie o header real do cofre "
                        "(formato em src/UM2LicenseSecret.example.h). Ele nunca entra no Git.")
endif()

set(SOURCES
    src/dllmain.cpp
    src/AsioDriver.cpp
    src/UM2License.cpp
    Driver.def
)

add_library(BehringerUM2ASIODriver SHARED ${SOURCES})

target_include_directories(BehringerUM2ASIODriver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ASIO_SDK_DIR}"
)

target_link_libraries(BehringerUM2ASIODriver PRIVATE
    setupapi
    ksuser
    avrt
    ole32
    advapi32
    shell32
)

if(MSVC)
    target_compile_options(BehringerUM2ASIODriver PRIVATE /W4 /EHsc /permissive-)
endif()

# Host de teste para validacao standalone
add_executable(test_um2_host test_um2_host.cpp)
target_include_directories(test_um2_host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ASIO_SDK_DIR}"
)
target_link_libraries(test_um2_host PRIVATE ole32)

# Motor Standalone & Discord Virtual Mic Bridge
add_library(BehringerUM2Bridge SHARED
    src/UM2Bridge.cpp
)
target_include_directories(BehringerUM2Bridge PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(BehringerUM2Bridge PRIVATE
    avrt
    ole32
    advapi32
)
if(MSVC)
    target_compile_options(BehringerUM2Bridge PRIVATE /W4 /EHsc /permissive-)
endif()

# ---------------------------------------------------------------------------
# Measurement tools (measure/, docs/MEASUREMENT-PROTOCOL.md). Not shipped.
# ---------------------------------------------------------------------------
add_executable(um2_drift measure/tools/um2_drift.cpp)
target_link_libraries(um2_drift PRIVATE ole32 avrt)

add_executable(raw_capture_test tools/raw_capture_test.cpp)
target_compile_definitions(raw_capture_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(raw_capture_test PRIVATE ole32)

add_executable(um2_asio_probe measure/tools/um2_asio_probe.cpp)
target_include_directories(um2_asio_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/measure/tools" "${ASIO_SDK_DIR}")
target_link_libraries(um2_asio_probe PRIVATE ole32)

# Diagnostico: quem muda o volume do endpoint de captura da UM2 (so leitura)
add_executable(volume_watch tools/volume_watch.cpp)
target_link_libraries(volume_watch PRIVATE ole32)

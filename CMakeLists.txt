cmake_minimum_required(VERSION 3.15)
project(GoldenDictLite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# ============================================================================
# Dependencies
# ============================================================================

set(NLOHMANN_JSON_DIR "${CMAKE_SOURCE_DIR}/TauriCPP/third_party/nlohmann")
if(NOT EXISTS "${NLOHMANN_JSON_DIR}/json.hpp")
    message(STATUS "Downloading nlohmann/json.hpp...")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        "${NLOHMANN_JSON_DIR}/json.hpp"
        SHOW_PROGRESS
    )
endif()

set(WEBVIEW2_SEARCH_PATHS
    "$ENV{VCPKG_ROOT}/installed/x64-windows-static"
    "$ENV{VCPKG_ROOT}/installed/x64-windows"
    "C:/vcpkg/installed/x64-windows-static"
    "C:/vcpkg/installed/x64-windows"
    "${CMAKE_SOURCE_DIR}/TauriCPP/third_party/WebView2"
)

find_path(WEBVIEW2_INCLUDE_DIR
    NAMES WebView2.h
    PATH_SUFFIXES include
    PATHS ${WEBVIEW2_SEARCH_PATHS}
)

find_library(WEBVIEW2_LIBRARY
    NAMES WebView2LoaderStatic WebView2Loader.dll.lib
    PATH_SUFFIXES lib
    PATHS ${WEBVIEW2_SEARCH_PATHS}
)

if(NOT WEBVIEW2_INCLUDE_DIR)
    message(FATAL_ERROR
        "WebView2 SDK not found!\n"
        "Install via: vcpkg install webview2 --classic\n"
        "Or download from: https://www.nuget.org/packages/Microsoft.Web.WebView2"
    )
endif()

# Find zlib (static)
find_library(ZLIB_STATIC_LIBRARY NAMES zlibstatic zlib)
if(ZLIB_STATIC_LIBRARY)
    set(ZLIB_LIBRARIES ${ZLIB_STATIC_LIBRARY})
else()
    find_package(ZLIB REQUIRED)
    set(ZLIB_LIBRARIES ZLIB::ZLIB)
endif()
find_path(ZLIB_INCLUDE_DIR zlib.h
    PATHS
        "$ENV{VCPKG_ROOT}/installed/x64-windows-static/include"
        "C:/vcpkg/installed/x64-windows-static/include"
        "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
        "C:/vcpkg/installed/x64-windows/include"
)
message(STATUS "ZLIB static library: ${ZLIB_LIBRARIES}")

# ============================================================================
# TauriCPP static library
# ============================================================================
set(TAURICPP_DIR "${CMAKE_SOURCE_DIR}/TauriCPP")
add_library(tauricpp STATIC
    ${TAURICPP_DIR}/src/app.cpp
    ${TAURICPP_DIR}/src/bridge.cpp
    ${TAURICPP_DIR}/src/clipboard.cpp
    ${TAURICPP_DIR}/src/dialog.cpp
    ${TAURICPP_DIR}/src/embedded_dll.cpp
    ${TAURICPP_DIR}/src/virtual_fs.cpp
    ${TAURICPP_DIR}/src/window.cpp
)

target_include_directories(tauricpp PUBLIC
    ${TAURICPP_DIR}/include
    ${NLOHMANN_JSON_DIR}/..
    ${WEBVIEW2_INCLUDE_DIR}
)

target_link_libraries(tauricpp PUBLIC
    shlwapi
    shell32
    comdlg32
    ole32
    ${WEBVIEW2_LIBRARY}
)

# ============================================================================
# GoldenDict parsing library
# ============================================================================
add_library(goldendict_parse STATIC
    src/goldendict/mdictparser.cc
    src/goldendict/decompress.cc
    src/goldendict/iconv.cc
    src/goldendict/htmlescape.cc
    src/goldendict/ripemd.cc
    src/goldendict/text.cc
    src/goldendict/filetype.cc
    src/dictionary_manager.cpp
)

target_include_directories(goldendict_parse PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/goldendict
    ${ZLIB_INCLUDE_DIR}
    ${NLOHMANN_JSON_DIR}/..
)

target_link_libraries(goldendict_parse PUBLIC
    ${ZLIB_LIBRARIES}
)

# ============================================================================
# Frontend resource packing
# ============================================================================
set(FRONTEND_DIR "${CMAKE_SOURCE_DIR}/frontend")
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
set(RESOURCES_RC "${GEN_DIR}/resources.rc")
set(RESOURCE_MAP_CPP "${GEN_DIR}/resource_map.cpp")

find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
    file(GLOB_RECURSE FRONTEND_FILES "${FRONTEND_DIR}/*")
    add_custom_command(
        OUTPUT "${RESOURCES_RC}" "${RESOURCE_MAP_CPP}"
        COMMAND ${Python3_EXECUTABLE} "${TAURICPP_DIR}/tools/pack_resources.py"
                "${FRONTEND_DIR}" -o "${RESOURCES_RC}" -t "TAURI_RES"
        DEPENDS "${TAURICPP_DIR}/tools/pack_resources.py" ${FRONTEND_FILES}
        COMMENT "Packing frontend resources into .rc file"
        VERBATIM
    )
else()
    file(WRITE "${RESOURCES_RC}" "// Auto-generated resources\n")
    file(WRITE "${RESOURCE_MAP_CPP}" "// Auto-generated\n")
endif()

# ============================================================================
# Main application
# ============================================================================
set(APP_ADDITIONAL_SOURCES "")
if(MSVC)
    set(DPI_MANIFEST_XML "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>
<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>
  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level='asInvoker' uiAccess='false' />
      </requestedPrivileges>
    </security>
  </trustInfo>
  <application>
    <windowsSettings>
      <dpiAware xmlns='http://schemas.microsoft.com/SMI/2005/WindowsSettings'>true/pm</dpiAware>
      <dpiAwareness xmlns='http://schemas.microsoft.com/SMI/2016/WindowsSettings'>PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
  <compatibility xmlns='urn:schemas-microsoft-com:compatibility.v1'>
    <application>
      <supportedOS Id='{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}'/>
    </application>
  </compatibility>
</assembly>")
    file(WRITE "${GEN_DIR}/dpi_awareness.manifest" "${DPI_MANIFEST_XML}")
    list(APPEND APP_ADDITIONAL_SOURCES "${GEN_DIR}/dpi_awareness.manifest")
endif()

add_executable(GoldenDictLite WIN32
    src/main.cpp
    src/icon.rc
    "${RESOURCE_MAP_CPP}"
    "${RESOURCES_RC}"
    ${APP_ADDITIONAL_SOURCES}
)

target_include_directories(GoldenDictLite PRIVATE "${GEN_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(GoldenDictLite PRIVATE tauricpp goldendict_parse)

add_dependencies(GoldenDictLite pack_resources)

add_custom_target(pack_resources
    DEPENDS "${RESOURCES_RC}" "${RESOURCE_MAP_CPP}"
    COMMENT "Pack frontend resources"
)

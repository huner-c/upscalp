if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
  message(FATAL_ERROR "The Upscalp IddCx package requires an x64 Visual Studio build.")
endif()

set(iddcx_version 1.4)
set(umdf_version 2.25)
set(wdk_version 10.0.26100.0)
set(wdk "${UPSCALP_WDK_ROOT}")
set(iddcx_headers "${wdk}/Include/${wdk_version}/um/iddcx/${iddcx_version}")
set(umdf_headers "${wdk}/Include/wdf/umdf/${umdf_version}")
if(NOT EXISTS "${iddcx_headers}/IddCx.h" OR NOT EXISTS "${umdf_headers}/wdf.h")
  message(FATAL_ERROR "IddCx and UMDF headers were not found under ${wdk}.")
endif()

add_library(UpscalpIdd SHARED virtual_display.cpp)
target_compile_features(UpscalpIdd PRIVATE cxx_std_20)
target_compile_definitions(UpscalpIdd PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  UMDF_DRIVER UMDF_VERSION_MAJOR=2 UMDF_VERSION_MINOR=25 IDDCX_VERSION_MAJOR=1 IDDCX_VERSION_MINOR=4)
target_compile_options(UpscalpIdd PRIVATE /W4 /utf-8 /EHsc)
target_link_options(UpscalpIdd PRIVATE /INCLUDE:FxDriverEntryUm)
target_include_directories(UpscalpIdd SYSTEM PRIVATE "${iddcx_headers}" "${umdf_headers}")
target_link_libraries(UpscalpIdd PRIVATE d3d11 dxgi ntdll
  "${wdk}/Lib/${wdk_version}/um/x64/iddcx/${iddcx_version}/iddcxstub.lib"
  "${wdk}/Lib/wdf/umdf/x64/${umdf_version}/WdfDriverStubUm.lib")
set_target_properties(UpscalpIdd PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(UpscalpIddDevice ../tools/idd_device.cpp)
target_compile_features(UpscalpIddDevice PRIVATE cxx_std_20)
target_compile_definitions(UpscalpIddDevice PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(UpscalpIddDevice PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(UpscalpIddDevice PRIVATE swdevice cfgmgr32 user32)
set_target_properties(UpscalpIddDevice PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

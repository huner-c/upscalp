cmake_minimum_required(VERSION 3.24)
project(Upscalp VERSION 0.4.1 LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Upscalp requires Windows and the Microsoft C++ toolchain.")
endif()

set(UPSCALP_WDK_ROOT "" CACHE PATH "Windows Driver Kit content directory for the optional IddCx driver")
if(UPSCALP_WDK_ROOT)
  add_subdirectory(driver)
endif()
add_executable(Upscalp WIN32
  src/main.cpp src/renderer.cpp src/capture.cpp src/displays.cpp src/input_bridge.cpp src/self_test.cpp
  resources/app.rc)
target_compile_features(Upscalp PRIVATE cxx_std_20)
target_compile_definitions(Upscalp PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(Upscalp PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_options(Upscalp PRIVATE /MANIFEST:NO)
target_include_directories(Upscalp PRIVATE src resources)
target_link_libraries(Upscalp PRIVATE d3d11 dxgi d3dcompiler windowsapp dwmapi
  comctl32 shell32 windowscodecs uxtheme)
set_property(TARGET Upscalp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
enable_testing()
add_test(NAME gpu_pipeline COMMAND Upscalp --self-test --report "${CMAKE_BINARY_DIR}/self-test.json")

# Optional, read-only native AMD backend probe. Headers remain in the ignored
# build cache; the installed AMD runtime is loaded from System32 at runtime.
set(UPSCALP_ADLX_INCLUDE "${CMAKE_BINARY_DIR}/_deps/adlx/SDK/Include" CACHE PATH "Official ADLX SDK include directory")
if(EXISTS "${UPSCALP_ADLX_INCLUDE}/ADLX.h")
  add_executable(UpscalpAmdProbe tools/amd_probe.cpp src/displays.cpp)
  target_compile_features(UpscalpAmdProbe PRIVATE cxx_std_20)
  target_compile_definitions(UpscalpAmdProbe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(UpscalpAmdProbe PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_include_directories(UpscalpAmdProbe PRIVATE src "${UPSCALP_ADLX_INCLUDE}")
  target_link_libraries(UpscalpAmdProbe PRIVATE user32)
  set_property(TARGET UpscalpAmdProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

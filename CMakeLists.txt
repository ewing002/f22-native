cmake_minimum_required(VERSION 3.20)
project(f22native CXX)

# F-22 Lightning II – natív újraépítés. Csak a Windows SDK könyvtárait használja
# (user32, gdi32, opengl32), külső függőség nincs.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(f22native WIN32
  src/main.cpp src/world.cpp src/res.cpp src/hud.cpp src/gl.cpp src/sim.cpp src/ui.cpp src/menu.cpp src/audio.cpp src/fx.cpp src/kdv.cpp src/aivm.cpp src/aiunit.cpp src/aibridge.cpp src/imgio.cpp)
target_include_directories(f22native PRIVATE third_party)   # Khronos GL/glext.h, GL/wglext.h (MIT), KHR/khrplatform.h: MSVC's SDK lacks them
target_compile_definitions(f22native PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(f22native PRIVATE /utf-8 /MP)
  set_property(TARGET f22native PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: no Visual C++ redistributable needed
endif()
target_link_libraries(f22native PRIVATE opengl32 gdi32 user32 winmm)
if(MINGW)
  target_link_options(f22native PRIVATE -static)   # a futtatáshoz ne kelljen MinGW DLL
endif()

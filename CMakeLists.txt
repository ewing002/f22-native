cmake_minimum_required(VERSION 3.20)
project(f22native CXX)

# F-22 Lightning II native rewrite. Uses only the Windows SDK libraries (user32, gdi32, opengl32, winmm;
# the launcher also comctl32, shell32, ole32); no external dependencies.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(f22native WIN32
  src/main.cpp src/world.cpp src/res.cpp src/hud.cpp src/gl.cpp src/sim.cpp src/ui.cpp src/menu.cpp src/audio.cpp src/fx.cpp src/kdv.cpp src/aivm.cpp src/aiunit.cpp src/aibridge.cpp src/imgio.cpp)
target_include_directories(f22native PRIVATE third_party)   # Khronos GL/glext.h, GL/wglext.h (MIT), KHR/khrplatform.h: MSVC's SDK lacks them
target_compile_definitions(f22native PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(f22native PRIVATE opengl32 gdi32 user32 winmm)

# The launcher (src/launcher/launcher.cpp): game folder, settings, texture export; starts f22native.exe
if(MSVC)
  add_executable(f22launcher WIN32 src/launcher/launcher.cpp src/launcher/launcher.manifest)
else()
  enable_language(RC)
  add_executable(f22launcher WIN32 src/launcher/launcher.cpp src/launcher/launcher_mingw.rc)
  target_link_options(f22launcher PRIVATE -municode)   # wWinMain
endif()
target_compile_definitions(f22launcher PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(f22launcher PRIVATE comctl32 shell32 ole32 user32 gdi32)

foreach(t f22native f22launcher)
  if(MSVC)
    target_compile_options(${t} PRIVATE /utf-8 /MP)
    set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: no Visual C++ redistributable needed
  endif()
  if(MINGW)
    target_link_options(${t} PRIVATE -static)   # no MinGW DLLs needed at run time
  endif()
endforeach()

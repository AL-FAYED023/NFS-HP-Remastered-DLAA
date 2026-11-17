cmake_minimum_required(VERSION 3.20)
project(NFS_DLAA C CXX)

# Extracted NVIDIA DLSS SDK folder (defaults to the DLSS folder next to this file)
set(DLSS_SDK "${CMAKE_SOURCE_DIR}/DLSS" CACHE PATH "NVIDIA DLSS SDK root")
# Optional: game folder the build output is copied to, e.g.
#   -DGAME_DIR="C:/Program Files (x86)/Steam/steamapps/common/Need For Speed Hot Pursuit Remastered"
set(GAME_DIR "" CACHE PATH "Game folder (optional)")

if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
  message(FATAL_ERROR "DLSS SDK not found: ${DLSS_SDK}\n"
                      "Extract the NVIDIA DLSS SDK into a folder named 'DLSS' next to CMakeLists.txt (see README).")
endif()

# Use the x64 libraries explicitly (not Windows_aarch64)
set(NGX_LIB     "${DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
set(NGX_LIB_DBG "${DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib")
set(NGX_DLL     "${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
foreach(f IN ITEMS "${NGX_LIB}" "${NGX_LIB_DBG}" "${NGX_DLL}")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "File not found: ${f}")
  endif()
endforeach()
message(STATUS "NGX lib: ${NGX_LIB}")

# nvsdk_ngx_s.lib is built for the static CRT (/MT)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/imgui")
add_library(NFS_DLAA SHARED src/nfs_dlaa.cpp src/vtbl_index.c
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
set_target_properties(NFS_DLAA PROPERTIES SUFFIX ".asi" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(NFS_DLAA PRIVATE /utf-8 /W3)
target_compile_definitions(NFS_DLAA PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_include_directories(NFS_DLAA PRIVATE "${DLSS_SDK}/include" "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(NFS_DLAA PRIVATE d3d11 d3dcompiler dxgi version setupapi shlwapi advapi32 dwmapi imm32
  "$<$<CONFIG:Debug>:${NGX_LIB_DBG}>" "$<$<NOT:$<CONFIG:Debug>>:${NGX_LIB}>")

# After building, copy asi, ini and nvngx_dlss.dll to the game folder
if(GAME_DIR AND EXISTS "${GAME_DIR}")
  add_custom_command(TARGET NFS_DLAA POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:NFS_DLAA>" "${GAME_DIR}/"
    COMMENT "NFS_DLAA.asi -> game folder")
  if(NOT EXISTS "${GAME_DIR}/NFS_DLAA.ini")
    add_custom_command(TARGET NFS_DLAA POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/NFS_DLAA.ini" "${GAME_DIR}/")
  endif()
  add_custom_command(TARGET NFS_DLAA POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NGX_DLL}" "${GAME_DIR}/")
endif()

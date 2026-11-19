cmake_minimum_required(VERSION 3.20)
project(tmnt2_netplay C CXX)

# TMNT2.exe is a 32-bit process: everything that goes into it must be x86.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure with -A Win32: TMNT2.exe is a 32-bit process")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: players do not need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# --- third_party: MinHook (x86 hooks) ----------------------------------------
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# --- third_party: SDL2 (gamepads: XInput, DualShock/DualSense, Switch over HIDAPI) --
# The source tree of the TMNT1 project is reused; SDL is linked statically into
# tmnt2net.asi and only its game controller part is used.
set(TMNT1_DIR "${CMAKE_SOURCE_DIR}/../tmnt2003-netplay" CACHE PATH "tmnt2003-netplay checkout (third_party/SDL)")
if(NOT EXISTS ${TMNT1_DIR}/third_party/SDL/CMakeLists.txt)
  message(FATAL_ERROR "SDL source not found in ${TMNT1_DIR}/third_party/SDL")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
foreach(sub AUDIO RENDER SENSOR LOCALE POWER FILESYSTEM)
  set(SDL_${sub} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt OPENGL OPENGLES VULKAN RENDER_D3D WASAPI DIRECTX_AUDIO)
  set(SDL_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${TMNT1_DIR}/third_party/SDL ${CMAKE_BINARY_DIR}/sdl EXCLUDE_FROM_ALL)

# --- third_party: ENet (UDP with reliable/unreliable channels), from the TMNT1 tree --
set(ENET_DIR ${TMNT1_DIR}/third_party/enet)
add_library(enet STATIC
  ${ENET_DIR}/callbacks.c ${ENET_DIR}/compress.c ${ENET_DIR}/host.c ${ENET_DIR}/list.c ${ENET_DIR}/packet.c
  ${ENET_DIR}/peer.c ${ENET_DIR}/protocol.c ${ENET_DIR}/win32.c)
target_include_directories(enet PUBLIC ${ENET_DIR}/include)
target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(enet PRIVATE /wd5287)
target_link_libraries(enet PUBLIC ws2_32 winmm)

# --- our code ------------------------------------------------------------------
set(OUR_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS= _WINSOCK_DEPRECATED_NO_WARNINGS UNICODE _UNICODE)
set(OUR_OPTS /W4 /permissive- /utf-8)

# common: log, hashing; the net code shared with tmntnet (TMNT1): sockets, ENet and
# Steam transports, Steam lobbies, the lab mock of Steam, join codes.
add_library(tmnt2net_common STATIC
  src/common/log.cpp
  src/common/sha256.cpp
  src/common/net.cpp
  src/common/joincode.cpp
  src/common/transport_enet.cpp
  src/common/transport_steam.cpp
  src/common/steam_boot.cpp
  src/common/steam_mock.cpp)
target_include_directories(tmnt2net_common PUBLIC src)
target_compile_definitions(tmnt2net_common PUBLIC ${OUR_DEFS})
target_compile_options(tmnt2net_common PUBLIC ${OUR_OPTS})
target_link_libraries(tmnt2net_common PUBLIC enet bcrypt ws2_32 iphlpapi)

# --- optional: Steam (Steamworks SDK; never part of the repo) -----------------------
#   cmake -B build -A Win32 -DSTEAMWORKS_SDK_DIR=C:/sdk/steamworks_sdk/sdk
# Without the SDK everything still builds: LAN / IP (ENet) and the lab mock only.
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/../steamworks_sdk_165/sdk" CACHE PATH "Steamworks SDK (public/steam, redistributable_bin)")
set(STEAM_API_DLL ${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.dll)
if(EXISTS ${STEAMWORKS_SDK_DIR}/public/steam/isteamnetworkingmessages.h AND
   EXISTS ${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.lib AND EXISTS ${STEAM_API_DLL})
  message(STATUS "Steam: ON (${STEAMWORKS_SDK_DIR})")
  add_library(steam_api SHARED IMPORTED)
  set_target_properties(steam_api PROPERTIES
    IMPORTED_IMPLIB ${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.lib
    IMPORTED_LOCATION ${STEAM_API_DLL}
    INTERFACE_INCLUDE_DIRECTORIES ${STEAMWORKS_SDK_DIR}/public)
  target_sources(tmnt2net_common PRIVATE src/common/steam_real.cpp src/common/lobby_steam.cpp)
  target_compile_definitions(tmnt2net_common PUBLIC TMNTNET_STEAM=1)
  file(STRINGS ${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h STEAM_INITEX REGEX "SteamAPI_InitEx")
  if(STEAM_INITEX)
    target_compile_definitions(tmnt2net_common PRIVATE TMNTNET_STEAM_INITEX=1)
  endif()
  target_link_libraries(tmnt2net_common PUBLIC steam_api delayimp)
  set(TMNT2NET_STEAM ON)
else()
  message(WARNING "Steam: OFF (no Steamworks SDK at ${STEAMWORKS_SDK_DIR}); LAN / IP only")
  target_sources(tmnt2net_common PRIVATE src/common/steam_stub.cpp)
  set(TMNT2NET_STEAM OFF)
endif()

# tmnt2net.asi: loaded into TMNT2.exe by Ultimate ASI Loader (winmm.dll)
add_library(tmnt2net MODULE
  src/mod/dllmain.cpp
  src/mod/config.cpp
  src/mod/patch.cpp
  src/mod/fixes.cpp
  src/mod/video.cpp
  src/mod/postfx.cpp
  src/mod/widescreen.cpp
  src/mod/input.cpp
  src/mod/pads.cpp
  src/mod/sim.cpp
  src/mod/overlay.cpp
  src/mod/netplay.cpp
  src/mod/gamefont.cpp
  src/mod/menus.cpp
  src/mod/netpage.cpp
  src/mod/pageui.cpp
  src/mod/coop.cpp
  src/mod/health.cpp
  src/mod/camera.cpp
  src/mod/results.cpp
  src/mod/midjoin.cpp)
target_link_libraries(tmnt2net PRIVATE tmnt2net_common minhook d3d9 dxguid winmm SDL2::SDL2-static)
if(TMNT2NET_STEAM)
  # steam_api.dll is loaded only when a session uses Steam (and from the game folder).
  target_link_options(tmnt2net PRIVATE /DELAYLOAD:steam_api.dll)
endif()
set_target_properties(tmnt2net PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "tmnt2net")

get_filename_component(MSVC_BIN_DIR ${CMAKE_LINKER} DIRECTORY)
find_program(DUMPBIN dumpbin HINTS ${MSVC_BIN_DIR})

# --- dist: what goes into the game folder, in build/dist/<config> ----------------
# Ultimate ASI Loader (ThirteenAG) as winmm.dll: TMNT2.exe imports winmm at
# start-up. The 32-bit build from the TMNT1 project is reused.
set(UAL_DLL "${CMAKE_SOURCE_DIR}/../tmnt2003-netplay/third_party/ultimate-asi-loader/bin/Win32/Release/dinput8.dll"
    CACHE FILEPATH "32-bit Ultimate ASI Loader dll (shipped as winmm.dll)")
set(DIST ${CMAKE_BINARY_DIR}/dist/$<CONFIG>)
add_custom_command(TARGET tmnt2net POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmnt2net> ${DIST}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/tmnt2net.ini ${DIST}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/README.md ${DIST}/tmnt2net-README.md
  COMMAND ${CMAKE_COMMAND} -DDUMPBIN=${DUMPBIN} -DFILE=$<TARGET_FILE:tmnt2net> -P ${CMAKE_SOURCE_DIR}/tools/check_x86.cmake)
if(TMNT2NET_STEAM)
  add_custom_command(TARGET tmnt2net POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STEAM_API_DLL} ${DIST}/steam_api.dll)
endif()
if(EXISTS ${UAL_DLL})
  add_custom_command(TARGET tmnt2net POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UAL_DLL} ${DIST}/winmm.dll)
else()
  message(WARNING "Ultimate ASI Loader not found at ${UAL_DLL}: dist has no winmm.dll")
endif()

# --- tmnt2net-setup.exe: one-file installer / uninstaller with everything inside ---
if(EXISTS ${UAL_DLL})
  set(PAYLOAD_DIR ${CMAKE_BINARY_DIR}/payload)
  set(PAYLOAD_FILES ${PAYLOAD_DIR}/winmm.dll ${PAYLOAD_DIR}/tmnt2net.asi ${PAYLOAD_DIR}/tmnt2net.ini
                    ${PAYLOAD_DIR}/README.md)
  set(PAYLOAD_STEAM_RC "")
  set(PAYLOAD_STEAM_COPY "")
  if(TMNT2NET_STEAM)
    list(APPEND PAYLOAD_FILES ${PAYLOAD_DIR}/steam_api.dll)
    set(PAYLOAD_STEAM_RC "RES_STEAM RCDATA \"${PAYLOAD_DIR}/steam_api.dll\"")
    set(PAYLOAD_STEAM_COPY COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STEAM_API_DLL} ${PAYLOAD_DIR}/)
  endif()
  add_custom_target(setup_payload
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PAYLOAD_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UAL_DLL} ${PAYLOAD_DIR}/winmm.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmnt2net> ${PAYLOAD_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/tmnt2net.ini ${PAYLOAD_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/README.md ${PAYLOAD_DIR}/
    ${PAYLOAD_STEAM_COPY}
    BYPRODUCTS ${PAYLOAD_FILES})
  add_dependencies(setup_payload tmnt2net)
  configure_file(src/setup/payload.rc.in ${CMAKE_BINARY_DIR}/payload.rc @ONLY)
  set_source_files_properties(${CMAKE_BINARY_DIR}/payload.rc PROPERTIES OBJECT_DEPENDS "${PAYLOAD_FILES}")
  add_executable(tmnt2net_setup WIN32 src/setup/setup.cpp src/common/sha256.cpp ${CMAKE_BINARY_DIR}/payload.rc)
  target_include_directories(tmnt2net_setup PRIVATE src)
  target_compile_definitions(tmnt2net_setup PRIVATE ${OUR_DEFS})
  target_compile_options(tmnt2net_setup PRIVATE ${OUR_OPTS})
  target_link_libraries(tmnt2net_setup PRIVATE comctl32 ole32 shell32 advapi32 bcrypt)
  target_link_options(tmnt2net_setup PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
  set_target_properties(tmnt2net_setup PROPERTIES OUTPUT_NAME "tmnt2net-setup")
  add_dependencies(tmnt2net_setup setup_payload)
  add_custom_command(TARGET tmnt2net_setup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tmnt2net_setup> ${CMAKE_BINARY_DIR}/dist/$<CONFIG>/)
endif()

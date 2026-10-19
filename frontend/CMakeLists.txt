cmake_minimum_required(VERSION 3.28...3.30)

add_subdirectory(api)

if(NOT ENABLE_FRONTEND)
  target_disable_feature(obs "User Interface")
  return()
else()
  target_enable_feature(obs "User Interface")
endif()

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil avformat)
find_package(CURL REQUIRED)

# AkvariumOBS: local speech recognition for replay buffer voice commands
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# GPU offload for recognition when the Vulkan SDK is available (AMD/Intel/NVIDIA).
if(DEFINED ENV{VULKAN_SDK})
  set(GGML_VULKAN ON CACHE BOOL "" FORCE)
  # obs-deps also ships old Vulkan headers; prefer the installed SDK so the
  # ggml-vulkan sources (PipelineRobustness, cooperative matrix) compile.
  list(PREPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
  set(Vulkan_INCLUDE_DIR "$ENV{VULKAN_SDK}/Include" CACHE PATH "" FORCE)
  set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib" CACHE FILEPATH "" FORCE)
  set(Vulkan_GLSLC_EXECUTABLE "$ENV{VULKAN_SDK}/Bin/glslc.exe" CACHE FILEPATH "" FORCE)
  # obs-deps ships an old vulkan_core.h; put the SDK headers in front of it.
  include_directories(BEFORE "$ENV{VULKAN_SDK}/Include")
  message(STATUS "AkvariumOBS: Vulkan backend enabled for whisper (VULKAN_SDK=$ENV{VULKAN_SDK})")
else()
  set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
  message(STATUS "AkvariumOBS: Vulkan SDK not found, whisper will run on CPU")
endif()
if(NOT TARGET whisper)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/whisper.cpp" whisper_cpp EXCLUDE_FROM_ALL)
endif()

# whisper.cpp is third-party C code; OBS builds with /WX, so relax warnings there.
if(MSVC)
  foreach(_whisper_target IN ITEMS whisper ggml ggml-base ggml-cpu ggml-blas ggml-vulkan)
    if(TARGET ${_whisper_target})
      target_compile_options(${_whisper_target} PRIVATE /WX- /W0)
    endif()
  endforeach()
endif()

if(NOT TARGET OBS::json11)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/json11" json11)
endif()

if(NOT TARGET OBS::libobs)
  add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" libobs)
endif()

if(NOT TARGET OBS::bpm)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/bpm" bpm)
endif()

add_executable(obs-studio)
add_executable(OBS::studio ALIAS obs-studio)

target_link_libraries(
  obs-studio
  PRIVATE
    CURL::libcurl
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::avformat
    OBS::libobs
    OBS::frontend-api
    OBS::json11
    OBS::bpm
    whisper
)

# AkvariumOBS: MCI playback for the replay buffer clip sound.
if(OS_WINDOWS)
  target_link_libraries(obs-studio PRIVATE winmm)
endif()

include(cmake/ui-components.cmake)
include(cmake/ui-dialogs.cmake)
include(cmake/ui-docks.cmake)
include(cmake/feature-importers.cmake)
include(cmake/feature-plugin-manager.cmake)
include(cmake/ui-models.cmake)
include(cmake/ui-oauth.cmake)
include(cmake/feature-browserpanels.cmake)
include(cmake/ui-qt.cmake)
include(cmake/ui-settings.cmake)
include(cmake/ui-utility.cmake)
include(cmake/ui-widgets.cmake)
include(cmake/ui-wizards.cmake)

if(NOT OAUTH_BASE_URL)
  set(OAUTH_BASE_URL "https://auth.obsproject.com/" CACHE STRING "Default OAuth base URL")
  mark_as_advanced(OAUTH_BASE_URL)
endif()
include(cmake/feature-twitch.cmake)
include(cmake/feature-restream.cmake)
include(cmake/feature-youtube.cmake)
include(cmake/feature-whatsnew.cmake)
include(cmake/feature-idian-playground.cmake)

configure_file(cmake/templates/ui-config.h.in ui-config.h)

target_sources(
  obs-studio
  PRIVATE obs-main.cpp OBSStudioAPI.cpp OBSStudioAPI.hpp OBSApp.cpp OBSApp.hpp OBSApp_Themes.cpp ui-config.h
)

if(OS_WINDOWS)
  include(cmake/os-windows.cmake)
elseif(OS_MACOS)
  include(cmake/os-macos.cmake)
elseif(OS_LINUX)
  include(cmake/os-linux.cmake)
elseif(OS_FREEBSD OR OS_OPENBSD)
  include(cmake/os-freebsd.cmake)
endif()

foreach(graphics_library IN ITEMS opengl metal d3d11)
  string(TOUPPER ${graphics_library} graphics_library_U)
  if(TARGET OBS::libobs-${graphics_library})
    target_compile_definitions(
      obs-studio
      PRIVATE
        DL_${graphics_library_U}="$<$<IF:$<PLATFORM_ID:Windows>,TARGET_FILE_NAME,TARGET_SONAME_FILE_NAME>:OBS::libobs-${graphics_library}>"
    )
  else()
    target_compile_definitions(obs-studio PRIVATE DL_${graphics_library_U}="")
  endif()
endforeach()

get_property(obs_module_list GLOBAL PROPERTY OBS_MODULES_ENABLED)
list(JOIN obs_module_list "|" SAFE_MODULES)
target_compile_definitions(obs-studio PRIVATE "SAFE_MODULES=\"${SAFE_MODULES}\"")

get_target_property(target_sources obs-studio SOURCES)
set(target_cpp_sources ${target_sources})
set(target_hpp_sources ${target_sources})
set(target_qt_sources ${target_sources})
list(FILTER target_cpp_sources INCLUDE REGEX ".+\\.(cpp|mm|c|m)")
list(SORT target_cpp_sources COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)
list(FILTER target_hpp_sources INCLUDE REGEX ".+\\.(hpp|h)")
list(SORT target_hpp_sources COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)
list(FILTER target_qt_sources INCLUDE REGEX ".+\\.(ui|qrc)")
list(SORT target_qt_sources COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${target_cpp_sources})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Header Files" FILES ${target_hpp_sources})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Qt Files" FILES ${target_qt_sources})

set_target_properties_obs(obs-studio PROPERTIES FOLDER frontend OUTPUT_NAME "$<IF:$<PLATFORM_ID:Windows>,AkvariumOBS,akvariumobs>")

# AkvariumOBS: offline recognition self-test tool (not part of the default build).
if(TARGET whisper AND TARGET Qt::Core)
  add_executable(akvarium-voice-selftest EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tools/akvarium-voice-selftest.cpp")
  target_include_directories(akvarium-voice-selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(akvarium-voice-selftest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(akvarium-voice-selftest PRIVATE whisper Qt::Core)
  set_target_properties_obs(akvarium-voice-selftest PROPERTIES FOLDER tools)
endif()

if(TARGET Qt::Core)
  add_executable(akvarium-phrase-match-test EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tools/phrase-match-test.cpp")
  target_include_directories(akvarium-phrase-match-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(akvarium-phrase-match-test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(akvarium-phrase-match-test PRIVATE Qt::Core)
  set_target_properties_obs(akvarium-phrase-match-test PROPERTIES FOLDER tools)
endif()

# AkvariumOBS: headless test for the replay buffer clip sound (folder, config, MCI).
if(TARGET Qt::Widgets AND OS_WINDOWS)
  add_executable(
    akvarium-replay-sound-test EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tools/akvarium-replay-sound-test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/utility/AkvariumReplaySound.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/utility/AkvariumReplaySound.hpp"
  )
  target_include_directories(akvarium-replay-sound-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(akvarium-replay-sound-test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(akvarium-replay-sound-test PRIVATE Qt::Widgets OBS::libobs winmm)
  set_target_properties_obs(akvarium-replay-sound-test PROPERTIES FOLDER tools)
  set_target_properties(akvarium-replay-sound-test PROPERTIES AUTOMOC TRUE)
endif()

add_library(dolphin_libretro SHARED
  libretro.cpp
)

target_include_directories(dolphin_libretro BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/Externals/libretro
)

target_link_libraries(dolphin_libretro PRIVATE
  core
  uicommon
)

set_target_properties(dolphin_libretro PROPERTIES OUTPUT_NAME dolphin_libretro)

if(MSVC)
  target_link_libraries(dolphin_libretro PRIVATE use_pch)
endif()

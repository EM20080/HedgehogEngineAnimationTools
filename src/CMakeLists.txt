add_library(HedgehogEngineAnimationTools SHARED main.cpp)

target_link_libraries(HedgehogEngineAnimationTools PRIVATE havok)
target_include_directories(HedgehogEngineAnimationTools PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${HEAT_HAVOKLIB_DIR}/include
  ${HEAT_HAVOKLIB_DIR}/source
)

if(WIN32)
  set(HEAT_LIBRARY_PLATFORM windows)
  set(HEAT_LIBRARY_EXTENSION dll)
elseif(APPLE)
  set(HEAT_LIBRARY_PLATFORM macos)
  set(HEAT_LIBRARY_EXTENSION dylib)
elseif(UNIX)
  set(HEAT_LIBRARY_PLATFORM linux)
  set(HEAT_LIBRARY_EXTENSION dll)
else()
  set(HEAT_LIBRARY_PLATFORM unknown)
  set(HEAT_LIBRARY_EXTENSION dll)
endif()

set(HEAT_OUTPUT_DIR "${CMAKE_BINARY_DIR}/src")
set(HEAT_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/lib")
set(HEAT_ADDON_NAME "HedgehogEngineAnimationTools")
set(HEAT_ADDON_STAGE_DIR "${CMAKE_BINARY_DIR}/addon/${HEAT_ADDON_NAME}")
set(HEAT_ADDON_ZIP "${CMAKE_BINARY_DIR}/${HEAT_ADDON_NAME}.zip")

set_target_properties(HedgehogEngineAnimationTools PROPERTIES
  PREFIX ""
  OUTPUT_NAME "HedgehogEngineAnimationTools.${HEAT_LIBRARY_PLATFORM}"
  SUFFIX ".${HEAT_LIBRARY_EXTENSION}"
  RUNTIME_OUTPUT_DIRECTORY "${HEAT_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${HEAT_OUTPUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${HEAT_ARCHIVE_DIR}"
)

foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(HedgehogEngineAnimationTools PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${HEAT_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${HEAT_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${HEAT_ARCHIVE_DIR}"
  )
endforeach()

add_custom_command(TARGET HedgehogEngineAnimationTools POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/__init__.py"
    "${HEAT_OUTPUT_DIR}/__init__.py"
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "${HEAT_ADDON_STAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/__init__.py"
    "${HEAT_ADDON_STAGE_DIR}/__init__.py"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:HedgehogEngineAnimationTools>"
    "${HEAT_ADDON_STAGE_DIR}/$<TARGET_FILE_NAME:HedgehogEngineAnimationTools>"
)

add_custom_target(package_addon
  COMMAND ${CMAKE_COMMAND} -E rm -f "${HEAT_ADDON_ZIP}"
  COMMAND ${CMAKE_COMMAND} -E tar cfv "${HEAT_ADDON_ZIP}" --format=zip "${HEAT_ADDON_NAME}"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/addon"
  DEPENDS HedgehogEngineAnimationTools
  COMMENT "Packaging ${HEAT_ADDON_ZIP}"
)

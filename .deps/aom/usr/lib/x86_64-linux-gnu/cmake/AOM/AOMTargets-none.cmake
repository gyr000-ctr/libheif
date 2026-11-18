#----------------------------------------------------------------
# Generated CMake target import file for configuration "None".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "AOM::aom" for configuration "None"
set_property(TARGET AOM::aom APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(AOM::aom PROPERTIES
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libaom.so.3.13.1"
  IMPORTED_SONAME_NONE "libaom.so.3"
  )

list(APPEND _cmake_import_check_targets AOM::aom )
list(APPEND _cmake_import_check_files_for_AOM::aom "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libaom.so.3.13.1" )

# Import target "AOM::aom_static" for configuration "None"
set_property(TARGET AOM::aom_static APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(AOM::aom_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NONE "ASM_NASM;C"
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libaom.a"
  )

list(APPEND _cmake_import_check_targets AOM::aom_static )
list(APPEND _cmake_import_check_files_for_AOM::aom_static "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libaom.a" )

# Import target "AOM::aomdec" for configuration "None"
set_property(TARGET AOM::aomdec APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(AOM::aomdec PROPERTIES
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/bin/aomdec"
  )

list(APPEND _cmake_import_check_targets AOM::aomdec )
list(APPEND _cmake_import_check_files_for_AOM::aomdec "${_IMPORT_PREFIX}/bin/aomdec" )

# Import target "AOM::aomenc" for configuration "None"
set_property(TARGET AOM::aomenc APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(AOM::aomenc PROPERTIES
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/bin/aomenc"
  )

list(APPEND _cmake_import_check_targets AOM::aomenc )
list(APPEND _cmake_import_check_files_for_AOM::aomenc "${_IMPORT_PREFIX}/bin/aomenc" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)

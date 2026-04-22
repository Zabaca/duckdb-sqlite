cmake_minimum_required(VERSION 3.5...3.29)
set(TARGET_NAME sqlite_scanner)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
project(${TARGET_NAME})

include_directories(src/sqlite)

add_subdirectory(src)
set(EXTENSION_OBJECT_FILES ${ALL_OBJECT_FILES})
add_library(${EXTENSION_NAME} STATIC ${EXTENSION_OBJECT_FILES})

# ---- spike: Corrosion + libsql-probe ----
include(FetchContent)
FetchContent_Declare(
  Corrosion
  GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
  GIT_TAG        v0.5.2
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(
  MANIFEST_PATH "/Users/uptown/Projects/zabaca/libsql-probe/libsql-probe/Cargo.toml"
  CRATES        "libsql-probe"
  PROFILE       release
)

set(LIBSQL_PROBE_SYSLIBS "")
if(APPLE)
  set(LIBSQL_PROBE_SYSLIBS
      "-framework Security"
      "-framework CoreFoundation"
      iconv)
elseif(UNIX)
  set(LIBSQL_PROBE_SYSLIBS ssl crypto)
endif()

target_link_libraries(${EXTENSION_NAME}
    libsql_probe-static
    ${LIBSQL_PROBE_SYSLIBS})
# ---- end spike ----

set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${EXTENSION_OBJECT_FILES})

# Loadable (.duckdb_extension) target also needs the Rust lib + sys libs
if(TARGET ${TARGET_NAME}_loadable_extension)
  target_link_libraries(${TARGET_NAME}_loadable_extension
      libsql_probe-static
      ${LIBSQL_PROBE_SYSLIBS})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

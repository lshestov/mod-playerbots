# Included by the core's modules/CMakeLists.txt after the `modules` target exists.
#
# The core links the MySQL client library privately into its `database` target,
# so its include directories do not reach module code. PlayerbotsDatabase.cpp
# needs the complete MySQLPreparedStatement type (which pulls in mysql.h) and
# Playerbots.cpp uses ER_BAD_DB_ERROR from mysqld_error.h. Linking the imported
# `mysql` target here propagates those include directories to the module build.
target_link_libraries(modules
  PRIVATE
    mysql)

# The core's module PCH hook is disabled. Reuse its stable core headers without
# putting frequently changed playerbot headers in the precompiled header.
option(PLAYERBOTS_USE_PCH "Use precompiled headers for the MSVC modules target" OFF)
if (PLAYERBOTS_USE_PCH AND MSVC AND USE_SCRIPTPCH)
  target_precompile_headers(modules PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../ModulesPCH.h")
endif()

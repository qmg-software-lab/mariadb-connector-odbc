#
#  Copyright (C) 2013-2016 MariaDB Corporation AB
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the COPYING-CMAKE-SCRIPTS file.
#

#
# This file contains settings for the following layouts:
#
# - RPM
# Built with default prefix=/usr
#
#
# The following va+riables are used and can be overwritten
#
# INSTALL_LAYOUT     installation layout (DEFAULT = standard for tar.gz and zip packages
#                                         RPM packages
#
# INSTALL_BINDIR    location of binaries (mariadb_config)
# INSTALL_LIBDIR    location of libraries
# INSTALL_PLUGINDIR location of plugins
# INSTALL_DOCDIR    location of docs
# INSTALL_LICENSEDIR location of license

IF(DEB)
  SET(INSTALL_LAYOUT "DEB")
ENDIF()

IF(RPM)
  SET(INSTALL_LAYOUT "RPM")
ENDIF()

IF(NOT INSTALL_LAYOUT)
  SET(INSTALL_LAYOUT "DEFAULT")
ENDIF()

SET(INSTALL_LAYOUT ${INSTALL_LAYOUT} CACHE
  STRING "Installation layout. Currently supported options are DEFAULT (tar.gz and zip), RPM and DEB")

# On Windows we only provide zip and .msi. Latter one uses a different packager.
IF(UNIX)
  IF(INSTALL_LAYOUT MATCHES "RPM|DEB")
    SET(libmaodbc_prefix "/usr")
  ELSEIF(INSTALL_LAYOUT STREQUAL "DEFAULT")
    SET(libmaodbc_prefix ${CMAKE_INSTALL_PREFIX})
  ENDIF()
ENDIF()

IF(CMAKE_DEFAULT_PREFIX_INITIALIZED_BY_DEFAULT)
  SET(CMAKE_DEFAULT_PREFIX ${libmariadb_prefix} CACHE PATH "Installation prefix" FORCE)
ENDIF()

# check if the specified installation layout is valid
SET(VALID_INSTALL_LAYOUTS "DEFAULT" "RPM" "DEB")
LIST(FIND VALID_INSTALL_LAYOUTS "${INSTALL_LAYOUT}" layout_no)
IF(layout_no EQUAL -1)
  MESSAGE(FATAL_ERROR "Invalid installation layout ${INSTALL_LAYOUT}. Please specify one of the following layouts: ${VALID_INSTALL_LAYOUTS}")
ENDIF()

# This has been done before C/C cmake scripts are included
IF(NOT DEFINED INSTALL_LIB_SUFFIX)
  SET(INSTALL_LIB_SUFFIX "lib" CACHE STRING "Directory, under which to install libraries, e.g. lib or lib64")
  IF("${CMAKE_SIZEOF_VOID_P}" EQUAL "8" AND EXISTS "/usr/lib64/" AND "${INSTALL_LAYOUT}" EQUAL "RPM")
    SET(INSTALL_LIB_SUFFIX "lib64")
  ENDIF()
ENDIF()

#
# Todo: We don't generate man pages yet, will fix it
#       later (webhelp to man transformation)
#

#
# DEFAULT layout
#

SET(INSTALL_BINDIR_DEFAULT "bin")
SET(INSTALL_LIBDIR_DEFAULT "${INSTALL_LIB_SUFFIX}/mariadb")
SET(INSTALL_PCDIR_DEFAULT "${INSTALL_LIB_SUFFIX}/pkgconfig")
SET(INSTALL_INCLUDEDIR_DEFAULT "include/mariadb")
SET(INSTALL_DOCDIR_DEFAULT "docs")
SET(INSTALL_LICENSEDIR_DEFAULT ${INSTALL_DOCDIR_DEFAULT})
SET(INSTALL_PLUGINDIR_DEFAULT "${INSTALL_LIB_SUFFIX}/mariadb/plugin")
SET(LIBMARIADB_STATIC_DEFAULT "mariadbclient")

#
# RPM layout
#
SET(INSTALL_BINDIR_RPM "bin")
IF((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "s390x") AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  SET(INSTALL_LIBDIR_RPM "lib64/mariadb")
  SET(INSTALL_PCDIR_RPM "lib64/pkgconfig")
  SET(INSTALL_PLUGINDIR_RPM "lib64/mariadb/plugin")
ELSE()
  SET(INSTALL_LIBDIR_RPM "lib/mariadb")
  SET(INSTALL_PCDIR_RPM "lib/pkgconfig")
  SET(INSTALL_PLUGINDIR_RPM "lib/mariadb/plugin")
ENDIF()
SET(INSTALL_INCLUDEDIR_RPM "include/mariadb")
SET(INSTALL_DOCDIR_RPM "share/doc/mariadb-connector-odbc")
SET(INSTALL_LICENSEDIR_RPM ${INSTALL_DOCDIR_RPM})
SET(LIBMARIADB_STATIC_RPM "mariadbclient")

#
# DEB layout
# Only ia-32 and amd64 here. the list is too long to hardcode it
IF(NOT CMAKE_LIBRARY_ARCHITECTURE)
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(CMAKE_LIBRARY_ARCHITECTURE "x86_64-linux-gnu")
  ELSE()
    SET(CMAKE_LIBRARY_ARCHITECTURE "i386-linux-gnu")
  ENDIF()
ENDIF()

SET(INSTALL_BINDIR_DEB "bin")
SET(INSTALL_LIBDIR_DEB "lib/${CMAKE_LIBRARY_ARCHITECTURE}")
SET(INSTALL_PCDIR_DEB "lib/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig")
SET(INSTALL_PLUGINDIR_DEB "${INSTALL_LIBDIR_DEB}/libmariadb${CPACK_PACKAGE_VERSION_MAJOR}/plugin")
SET(INSTALL_INCLUDEDIR_DEB "include/mariadb")
SET(INSTALL_DOCDIR_DEB "share/doc/mariadb-connector-odbc")
SET(INSTALL_LICENSEDIR_DEB "${INSTALL_DOCDIR_DEB}")
SET(LIBMARIADB_STATIC_DEB "mariadb")

#
# Overwrite defaults
#
IF(INSTALL_LIBDIR)
  SET(INSTALL_LIBDIR_${INSTALL_LAYOUT} ${INSTALL_LIBDIR})
ENDIF()

IF(INSTALL_PCDIR)
  SET(INSTALL_PCDIR_${INSTALL_LAYOUT} ${INSTALL_PCDIR})
ENDIF()

IF(INSTALL_PLUGINDIR)
  SET(INSTALL_PLUGINDIR_${INSTALL_LAYOUT} ${INSTALL_PLUGINDIR})
ENDIF()

IF(INSTALL_DOCDIR)
  SET(INSTALL_DOCDIR_${INSTALL_LAYOUT} ${INSTALL_DOCDIR})
ENDIF()

IF(INSTALL_LICENSEDIR)
  SET(INSTALL_LICENSEDIR_${INSTALL_LAYOUT} ${INSTALL_LICENSEDIR})
ENDIF()

# Extra INSTALL_PLUGINDIR_CLIENT that overrides any INSTALL_PLUGINDIR override
IF(INSTALL_PLUGINDIR_CLIENT)
  SET(INSTALL_PLUGINDIR_${INSTALL_LAYOUT} ${INSTALL_PLUGINDIR_CLIENT})
ENDIF()

IF(INSTALL_INCLUDEDIR)
  SET(INSTALL_INCLUDEDIR_${INSTALL_LAYOUT} ${INSTALL_INCLUDEDIR})
ENDIF()

IF(INSTALL_BINDIR)
  SET(INSTALL_BINDIR_${INSTALL_LAYOUT} ${INSTALL_BINDIR})
ENDIF()

IF(NOT INSTALL_PREFIXDIR)
  SET(INSTALL_PREFIXDIR_${INSTALL_LAYOUT} ${libmariadb_prefix})
ELSE()
  SET(INSTALL_PREFIXDIR_${INSTALL_LAYOUT} ${INSTALL_PREFIXDIR})
ENDIF()

IF(DEFINED INSTALL_SUFFIXDIR)
  SET(INSTALL_SUFFIXDIR_${INSTALL_LAYOUT} ${INSTALL_SUFFIXDIR})
ENDIF()

FOREACH(dir "BIN" "LIB" "PC" "INCLUDE" "DOC" "LICENSE" "PLUGIN")
  SET(INSTALL_${dir}DIR ${INSTALL_${dir}DIR_${INSTALL_LAYOUT}})
  MARK_AS_ADVANCED(INSTALL_${dir}DIR)
  MESSAGE(STATUS "MariaDB Connector ODBC: INSTALL_${dir}DIR=${INSTALL_${dir}DIR}")
ENDFOREACH()

SET(INSTALL_PLUGINDIR_CLIENT ${INSTALL_PLUGINDIR})
MESSAGE(STATUS "MariaDB Connector ODBC: INSTALL_PLUGINDIR_CLIENT=${INSTALL_PLUGINDIR_CLIENT}")

MESSAGE(STATUS "Libraries installation dir: ${INSTALL_LIBDIR}")
MESSAGE(STATUS "Authentication Plugins installation dir: ${INSTALL_PLUGINDIR}")

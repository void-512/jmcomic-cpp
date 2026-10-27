include(ExternalProject)
include(CMakePackageConfigHelpers)
include(ProcessorCount)
find_package(Perl REQUIRED)

set(_openssl_source "${CMAKE_CURRENT_SOURCE_DIR}/external/openssl")
set(_openssl_prefix "${CMAKE_CURRENT_BINARY_DIR}/external/openssl")
set(_openssl_install "${_openssl_prefix}/install")
file(STRINGS "${_openssl_source}/VERSION.dat" _openssl_version_parts REGEX "^(MAJOR|MINOR|PATCH)=")
foreach(_part IN LISTS _openssl_version_parts)
  if(_part MATCHES "^(MAJOR|MINOR|PATCH)=([0-9]+)$")
    set(_openssl_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()
set(_openssl_version "${_openssl_MAJOR}.${_openssl_MINOR}.${_openssl_PATCH}")

set(JMCOMIC_OPENSSL_TARGET "" CACHE STRING "OpenSSL Configure target; empty selects the native platform")
set(_openssl_target ${JMCOMIC_OPENSSL_TARGET})
if(MSVC)
  find_program(_openssl_make NAMES nmake REQUIRED)
  set(_openssl_make_args /NOLOGO)
  set(_openssl_crypto libcrypto.lib)
  set(_openssl_ssl libssl.lib)
  if(NOT _openssl_target)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
      set(_openssl_target VC-WIN64-ARM)
      if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(_openssl_target VC-CLANG-WIN64-CLANGASM-ARM)
      endif()
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_openssl_target VC-WIN64A)
    else()
      set(_openssl_target VC-WIN32)
    endif()
  endif()
else()
  find_program(_openssl_make NAMES gmake make REQUIRED)
  ProcessorCount(_openssl_jobs)
  if(NOT _openssl_jobs)
    set(_openssl_jobs 1)
  endif()
  if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL} AND "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" MATCHES "^[1-9][0-9]*$")
    set(_openssl_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
  endif()
  set(_openssl_make_args "-j${_openssl_jobs}")
  set(_openssl_crypto libcrypto.a)
  set(_openssl_ssl libssl.a)
  if(MINGW AND NOT _openssl_target)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_openssl_target mingw64)
    else()
      set(_openssl_target mingw)
    endif()
  endif()
endif()

set(_openssl_cflags "${CMAKE_C_FLAGS}")
foreach(_config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${_config}" _upper)
  string(APPEND _openssl_cflags " $<$<CONFIG:${_config}>:${CMAKE_C_FLAGS_${_upper}}>")
endforeach()
ExternalProject_Add(jmcomic_openssl
  PREFIX "${_openssl_prefix}"
  SOURCE_DIR "${_openssl_source}"
  BINARY_DIR "${_openssl_prefix}/build"
  INSTALL_DIR "${_openssl_install}"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
    "CC=${CMAKE_C_COMPILER}" "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}" "CFLAGS=${_openssl_cflags}"
    ${PERL_EXECUTABLE} "${_openssl_source}/Configure" ${_openssl_target}
    $<IF:$<CONFIG:Debug>,--debug,--release>
    no-shared no-tests no-docs no-apps no-module
    "--prefix=<INSTALL_DIR>" --libdir=lib
  BUILD_COMMAND ${_openssl_make} ${_openssl_make_args} build_libs
  INSTALL_COMMAND ${_openssl_make} install_dev
  BUILD_BYPRODUCTS "${_openssl_install}/lib/${_openssl_crypto}" "${_openssl_install}/lib/${_openssl_ssl}"
  EXCLUDE_FROM_ALL TRUE
  LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE
)
ExternalProject_Add_StepDependencies(jmcomic_openssl configure "${_openssl_source}/Configure" "${_openssl_source}/VERSION.dat")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_openssl_source}/VERSION.dat")

file(MAKE_DIRECTORY "${_openssl_install}/include")
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
  IMPORTED_LOCATION "${_openssl_install}/lib/${_openssl_crypto}"
  INTERFACE_INCLUDE_DIRECTORIES "${_openssl_install}/include"
  INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}"
)
if(WIN32)
  set_property(TARGET OpenSSL::Crypto APPEND PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 gdi32 advapi32 crypt32 user32)
endif()
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
  IMPORTED_LOCATION "${_openssl_install}/lib/${_openssl_ssl}"
  INTERFACE_INCLUDE_DIRECTORIES "${_openssl_install}/include"
  INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)
add_dependencies(OpenSSL::Crypto jmcomic_openssl)
add_dependencies(OpenSSL::SSL jmcomic_openssl)

# Redirect curl's find_package call to these targets, including in build trees
# that previously cached paths to a system OpenSSL installation.
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/openssl-config.cmake"
  "set(OpenSSL_FOUND TRUE)\nset(OPENSSL_FOUND TRUE)\nset(OPENSSL_USE_STATIC_LIBS TRUE)\n"
  "set(OPENSSL_VERSION \"${_openssl_version}\")\nset(OPENSSL_VERSION_MAJOR ${_openssl_MAJOR})\n"
  "set(OPENSSL_INCLUDE_DIR \"${_openssl_install}/include\")\n"
  "set(OPENSSL_SSL_LIBRARY \"${_openssl_install}/lib/${_openssl_ssl}\")\n"
  "set(OPENSSL_CRYPTO_LIBRARY \"${_openssl_install}/lib/${_openssl_crypto}\")\n"
  "set(OPENSSL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)\n"
)
write_basic_package_version_file("${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/openssl-config-version.cmake"
  VERSION "${_openssl_version}" COMPATIBILITY SameMajorVersion)

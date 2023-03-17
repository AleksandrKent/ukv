set(OPENSSL_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/openssl-src)
set(OPENSSL_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/openssl)
set(OPENSSL_INCLUDE_DIR ${OPENSSL_INSTALL_DIR}/include)
set(OPENSSL_CONFIGURE_COMMAND ${OPENSSL_SOURCE_DIR}/config)
ExternalProject_Add(
  openssl-external
  SOURCE_DIR ${OPENSSL_SOURCE_DIR}
  GIT_REPOSITORY https://github.com/openssl/openssl.git
  GIT_TAG openssl-3.1.0
  CONFIGURE_COMMAND
    ${OPENSSL_CONFIGURE_COMMAND}
    --prefix=${OPENSSL_INSTALL_DIR}
    --openssldir=${OPENSSL_INSTALL_DIR}
  BUILD_COMMAND make
  TEST_COMMAND ""
  INSTALL_COMMAND make install
  INSTALL_DIR ${OPENSSL_INSTALL_DIR}

  BUILD_ALWAYS 0
  UPDATE_DISCONNECTED 1
)
file(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})

add_library(openssl::ssl STATIC IMPORTED)
set_property(TARGET openssl::ssl PROPERTY IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib64/libssl.a)
set_property(TARGET openssl::ssl PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
add_dependencies(openssl::ssl openssl-externa)

add_library(openssl::crypto STATIC IMPORTED GLOBAL)
set_property(TARGET openssl::crypto PROPERTY IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib64/libcrypto.a)
set_property(TARGET openssl::crypto PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
add_dependencies(openssl::crypto openssl-externa)
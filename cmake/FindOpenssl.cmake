function(FindOpenssl)
  if(TARGET openssl_internal)
    return()
  endif()

  if(IOS)
    set(IOS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../Build_iOS")

    set(OPENSSL_INCLUDE_DIR "${IOS_SOURCE_DIR}/openssl/include" CACHE INTERNAL "")
    set(OPENSSL_LIBRARIES
      "${IOS_SOURCE_DIR}/openssl/lib/libcrypto.a"
      "${IOS_SOURCE_DIR}/openssl/lib/libssl.a"
      CACHE INTERNAL ""
      )
    set(_SSL_LEAK_SUPPRESS_AVAILABLE ON CACHE INTERNAL "")
  elseif(ANDROID)
    if(ARM)
      set(OPENSSL_INCLUDE_DIR "${CMAKE_BINARY_DIR}/../openssl/armeabi-v7a/include" CACHE INTERNAL "")
      set(OPENSSL_LIBRARIES
        "${CMAKE_BINARY_DIR}/../openssl/armeabi-v7a/lib/libssl.a"
        "${CMAKE_BINARY_DIR}/../openssl/armeabi-v7a/lib/libcrypto.a"
        CACHE INTERNAL ""
        )
    else()
      set(OPENSSL_INCLUDE_DIR "${CMAKE_BINARY_DIR}/../openssl/x86/include" CACHE INTERNAL "")
      set(OPENSSL_LIBRARIES
        "${CMAKE_BINARY_DIR}/../openssl/x86/lib/libssl.a"
        "${CMAKE_BINARY_DIR}/../openssl/x86/lib/libcrypto.a"
        CACHE INTERNAL ""
        )
    endif()
    set(_SSL_LEAK_SUPPRESS_AVAILABLE ON CACHE INTERNAL "")
  else()
    #if(APPLE)
    #  if(NOT DEFINED OPENSSL_ROOT_DIR)
    #    # Prefer a homebrew version of OpenSSL over the one in /usr/lib
    #    file(GLOB OPENSSL_ROOT_DIR /usr/local/Cellar/openssl@1.1/*)
    #    # Prefer the latest (make the latest one first)
    #    list(REVERSE OPENSSL_ROOT_DIR)
    #  endif()
    #  # This should prevent linking against the system provided 0.9.8y
    #  set(_OPENSSL_VERSION "")
    #endif()

    find_package(OpenSSL 1.0.0 REQUIRED)

    INCLUDE(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES "${OPENSSL_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_LIBRARIES "${OPENSSL_LIBRARIES}")
    CHECK_CXX_SOURCE_COMPILES("
      #include <openssl/ssl.h>
      int main()
      {
      ::SSL_COMP_free_compression_methods();
      }
    " _SSL_LEAK_SUPPRESS_AVAILABLE)
  endif()

  add_library(openssl_internal INTERFACE)
  if(TARGET OpenSSL::SSL)
    target_link_libraries(openssl_internal INTERFACE OpenSSL::SSL)
  else()
    target_link_libraries(openssl_internal INTERFACE "$<BUILD_INTERFACE:${OPENSSL_LIBRARIES}>")
    target_include_directories(openssl_internal INTERFACE "$<BUILD_INTERFACE:${OPENSSL_INCLUDE_DIR}>")
  endif()

  if (NOT _SSL_LEAK_SUPPRESS_AVAILABLE)
    # libressl doesn't ship with the cleanup method being used in ws_client_wspp
    target_compile_definitions(openssl_internal INTERFACE -DCPPREST_NO_SSL_LEAK_SUPPRESS)
  endif()
endfunction()

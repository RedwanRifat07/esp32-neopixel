cmake_minimum_required(VERSION 3.16)

# --- .env -> sdkconfig.local (fails the build on a missing/invalid .env) ----
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SIOT_ENV_FILE "${CMAKE_SOURCE_DIR}/.env")
set(SIOT_LOCAL_CFG "${CMAKE_SOURCE_DIR}/sdkconfig.local")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SIOT_ENV_FILE}")

set(_siot_old_hash "")
if(EXISTS "${SIOT_LOCAL_CFG}")
    file(SHA256 "${SIOT_LOCAL_CFG}" _siot_old_hash)
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/env_to_sdkconfig.py"
            --env "${SIOT_ENV_FILE}" --out "${SIOT_LOCAL_CFG}"
    RESULT_VARIABLE _siot_rc
    ERROR_VARIABLE _siot_err)
if(NOT _siot_rc EQUAL 0)
    file(REMOVE "${SIOT_LOCAL_CFG}")
    message(FATAL_ERROR "${_siot_err}")
endif()
file(SHA256 "${SIOT_LOCAL_CFG}" _siot_new_hash)
# A stale sdkconfig would keep old values (defaults files only seed new symbols),
# so regenerate it whenever the .env-derived config changed.
if(NOT _siot_old_hash STREQUAL _siot_new_hash AND EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    message(STATUS ".env changed: discarding generated sdkconfig so new values apply")
    file(REMOVE "${CMAKE_SOURCE_DIR}/sdkconfig")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_ims_neopixel)

include_guard(GLOBAL)

# Detect the target architecture by trying to compile `src/arch_detect.c`.
# Some toolchains (notably MSYS2/MinGW) can leave the detection in an "unknown"
# state even though CMAKE_SYSTEM_PROCESSOR is set correctly, so keep a portable
# fallback based on the configured target processor.
if(CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _arch_lower)
    if(_arch_lower MATCHES "^(x86_64|amd64|x64)$")
        set(ARCH x86_64)
    elseif(_arch_lower MATCHES "^(arm64|aarch64)$")
        set(ARCH arm64)
    else()
        set(ARCH unknown)
    endif()
else()
    try_compile(RESULT_VAR ${CMAKE_BINARY_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/src/arch_detect.c" OUTPUT_VARIABLE ARCH)
    string(REGEX MATCH "ARCH ([a-zA-Z0-9_]+)" ARCH "${ARCH}")
    string(REPLACE "ARCH " "" ARCH "${ARCH}")

    if(NOT ARCH)
        set(ARCH unknown)
    endif()
endif()

if (ARCH STREQUAL "x86_64")
    set(ARCH_X64 1)
elseif (ARCH STREQUAL "arm64")
    set(ARCH_ARM64 1)
endif()

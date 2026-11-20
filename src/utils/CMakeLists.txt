#
# 86Box    A hypervisor and IBM PC system emulator that specializes in
#          running old operating systems and software designed for IBM
#          PC systems and compatibles from 1981 through fairly recent
#          system designs based on the PCI bus.
#
#          This file is part of the 86Box distribution.
#
#          CMake build script.
#
# Authors: David Hrdlička, <hrdlickadavid@outlook.com>
#          Jasmine Iwanek, <jriwanek@gmail.com>
#
#          Copyright 2020-2021 David Hrdlička.
#          Copyright 2024-2026 Jasmine Iwanek.
#

add_library(utils OBJECT
    # Core
    cJSON.c
    civil_time.c
    crc.c
    crc32.c
    fifo.c
    fifo8.c
    fs_name.c
    ini.c
    log.c
    random.c
)

# Names that macOS gives decomposed are composed again through CoreFoundation.
if(APPLE)
    target_link_libraries(86Box "-framework CoreFoundation")
endif()

# Names in Shift-JIS for Japanese machines and languages go through iconv; Windows has its own code pages.
if(NOT WIN32)
    find_package(Iconv)
    if(Iconv_FOUND)
        target_compile_definitions(utils PRIVATE USE_ICONV)
        target_link_libraries(86Box Iconv::Iconv)
    endif()
endif()

add_library(mdsx_dll OBJECT
    # MDSX DLL Core
    mdsx_dll.c
)

# cmake file for Raspberry Pi Pico
set(PICO_BOARD "pimoroni_artmatrix")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR})
set(PICO_PLATFORM "rp2350")
set(PICO_NUM_GPIOS 48)

# Board specific version of the frozen manifest
set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/manifest.py)

add_compile_options(-fno-math-errno)   # hardware VSQRT on the M33 FPU (safe: MP math uses isnan/isinf, not errno)

# Nothing of ours uses the C heap, but libstdc++ mallocs a 1088 byte emergency
# exception pool from a static ctor, before main(). __StackLimit is __bss_end__
# plus this, and nano-malloc extends the break to the next 4k page, so it can
# need 1112 + 4095 bytes depending on where .bss happens to end. At 4096 the
# boot panics with "Out of memory" whenever (__bss_end__ & 0xfff) > 2984, which
# makes any change to a static's size a coin flip. 8192 always covers it.
set(MICROPY_C_HEAP_SIZE 8192)

if(NOT DEFINED MICROPY_HW_FLASH_STORAGE_BYTES)
    set(MICROPY_HW_FLASH_STORAGE_BYTES 13631488)  # 16 * 1024 * 1024 - 1MB for ROMFS and 2MB for LittleFS
endif()

# Enable PSRAM, should pick up CS and size from
# the board header file.
set(MICROPY_HW_ENABLE_PSRAM 1)

# Links micropy_lib_lwip and sets MICROPY_PY_LWIP = 1
# Picked up and expanded upon in mpconfigboard.h
set(MICROPY_PY_LWIP ON)

# Links cyw43-driver and sets:
# MICROPY_PY_NETWORK_CYW43 = 1,
# MICROPY_PY_SOCKET_DEFAULT_TIMEOUT_MS = 30000
set(MICROPY_PY_NETWORK_CYW43 ON)

# Adds mpbthciport.c
# And sets:
# MICROPY_PY_BLUETOOTH = 1,
# MICROPY_PY_BLUETOOTH_USE_SYNC_EVENTS = 1,
# MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE = 1
set(MICROPY_PY_BLUETOOTH ON)

# Links pico_btstack_hci_transport_cyw43
# And sets:
# MICROPY_BLUETOOTH_BTSTACK = 1,
# MICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE =
set(MICROPY_BLUETOOTH_BTSTACK ON)

# Sets:
# CYW43_ENABLE_BLUETOOTH = 1,
# MICROPY_PY_BLUETOOTH_CYW43 = 1
set(MICROPY_PY_BLUETOOTH_CYW43 ON)

set(PIMORONI_LFS_MANIFEST ${CMAKE_CURRENT_LIST_DIR}/manifest.txt)
set(PIMORONI_LFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../examples)
set(PIMORONI_ROMFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../romfs)
include(${CMAKE_CURRENT_LIST_DIR}/filesystem.cmake)

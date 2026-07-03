if(NOT DEFINED PIMORONI_PICO_PATH)
set(PIMORONI_PICO_PATH ${CMAKE_CURRENT_LIST_DIR}/../pimoroni-pico)
endif()
include(${PIMORONI_PICO_PATH}/pimoroni_pico_import.cmake)

include_directories(${CMAKE_CURRENT_LIST_DIR}/../../)
include_directories(${PIMORONI_PICO_PATH}/micropython)

# Drivers, etc
list(APPEND CMAKE_MODULE_PATH "${PIMORONI_PICO_PATH}")
# modules_py/modules_py
list(APPEND CMAKE_MODULE_PATH "${PIMORONI_PICO_PATH}/micropython")
# All regular modules
list(APPEND CMAKE_MODULE_PATH "${PIMORONI_PICO_PATH}/micropython/modules")

# Allows us to find /modules/duo75
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/..")

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

# PicoVector & MicroPython bindings
# Single-core rasterisation on ArtMatrix (PV_DUAL_CORE is off by default).
set(PV_DUAL_CORE OFF)
find_package(PICOVECTOR_MICROPYTHON CONFIG REQUIRED)

# Build picovector for Pico
target_compile_definitions(usermod_picovector INTERFACE TUFTY=1 PICO=1)

# Essential
include(pimoroni_i2c/micropython)
include(pimoroni_bus/micropython)

# Sensors & Breakouts
include(micropython-common-breakouts)

# Utility
include(adcfft/micropython)
include(plasma/micropython)

# LEDs & Matrices
include(modules/c/artmatrix/micropython)

include(qrcode/micropython/micropython)

# Still required for version.py
include(modules_py/modules_py)

# C++ Magic Memory
include(cppmem/micropython)

# Must call `enable_ulab()` to enable
include(micropython-common-ulab)
enable_ulab()

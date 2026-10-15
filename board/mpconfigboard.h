// Board and hardware specific configuration
#define MICROPY_HW_BOARD_NAME                   "ArtMatrix"

// Defined in mpconfigboard.cmake
// #define MICROPY_HW_ROMFS_BYTES                  (1 * 1024 * 1024)

#define MICROPY_OBJ_REPR                        (MICROPY_OBJ_REPR_C)

#define MICROPY_GC_NO_SCAN   (1)   // big PSRAM GC win; NO_CLEAR benefit comes with it
#define MICROPY_HW_VM_IN_RAM (1)   // ~+20% interp, costs ~6 KB SRAM (.data)

// Set up networking.
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT     "ArtMatrix"

// Enable WiFi & PPP
#define MICROPY_PY_NETWORK                      (1)

// Raise the 5 socket limit
#define MEMP_NUM_TCP_PCB                        (50)

// CYW43 driver configuration.
#define CYW43_USE_SPI                           (1)
#define CYW43_LWIP                              (1)
#define CYW43_GPIO                              (1)
#define CYW43_SPI_PIO                           (1)

#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif

#define MICROPY_HW_PIN_EXT_COUNT    CYW43_WL_GPIO_COUNT

int mp_hal_is_pin_reserved(int n);
#define MICROPY_HW_PIN_RESERVED(i) mp_hal_is_pin_reserved(i)

// Skip default pins
#define MICROPY_HW_UART_NO_DEFAULT_PINS         (1)

// Don't use SRAM for MicroPython heap
#define MICROPY_GC_SPLIT_HEAP                   (0)

#define MICROPY_PY_THREAD                       (0)
add_library(usermod_artmatrix INTERFACE)

target_sources(usermod_artmatrix INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/artmatrix_bindings.c
    ${CMAKE_CURRENT_LIST_DIR}/artmatrix_bindings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/driver/artmatrix.cpp
    ${CMAKE_CURRENT_LIST_DIR}/driver/audio.cpp
)

target_include_directories(usermod_artmatrix INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_generate_pio_header(usermod_artmatrix ${CMAKE_CURRENT_LIST_DIR}/driver/artmatrix.pio)

set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/driver/artmatrix.cpp ${CMAKE_CURRENT_LIST_DIR}/driver/audio.cpp PROPERTIES COMPILE_OPTIONS "-O2;-fgcse-after-reload;-floop-interchange;-fpeel-loops;-fpredictive-commoning;-fsplit-paths;-ftree-loop-distribute-patterns;-ftree-loop-distribution;-ftree-vectorize;-ftree-partial-pre;-funswitch-loops")

target_link_libraries(usermod_artmatrix INTERFACE hardware_adc)

target_link_libraries(usermod INTERFACE usermod_artmatrix)
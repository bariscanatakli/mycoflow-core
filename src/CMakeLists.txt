# Kaynak dosyaları belirle
include(CheckIncludeFile)
set(SOURCES
    main.c
    myco_log.c
    myco_config.c
    myco_sense.c
    myco_persona.c
    myco_control.c
    myco_act.c
    myco_ebpf.c
    myco_ubus.c
)

# eBPF object build (requires clang/llvm)
find_program(CLANG_BIN clang)
find_program(LLC_BIN llc)
if(CLANG_BIN AND LLC_BIN)
    set(BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/mycoflow.bpf.o)
    add_custom_command(
        OUTPUT ${BPF_OBJ}
        COMMAND ${CLANG_BIN} -O2 -g -target bpf -c ${CMAKE_CURRENT_SOURCE_DIR}/mycoflow.bpf.c -o ${BPF_OBJ}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mycoflow.bpf.c
        COMMENT "Building eBPF object"
    )
    add_custom_target(mycoflow_bpf ALL DEPENDS ${BPF_OBJ})
    add_custom_command(TARGET mycoflow_bpf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lib/mycoflow
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BPF_OBJ} ${CMAKE_CURRENT_BINARY_DIR}/lib/mycoflow/mycoflow.bpf.o
        COMMENT "Staging eBPF object"
    )
endif()

# Executable oluştur (mycoflowd)
add_executable(mycoflowd ${SOURCES})

# Math library (needed for fabs in sense module)
target_link_libraries(mycoflowd PRIVATE m)

# Optional ubus support (OpenWrt)
check_include_file(libubus.h HAVE_UBUS_H)
if(HAVE_UBUS_H)
    find_library(UBUS_LIB ubus)
    find_library(UBOX_LIB ubox)
    find_library(JSONC_LIB json-c)
    if(UBUS_LIB AND UBOX_LIB)
        find_package(Threads)
        target_compile_definitions(mycoflowd PRIVATE HAVE_UBUS)
        target_link_libraries(mycoflowd PRIVATE ${UBUS_LIB} ${UBOX_LIB} ${JSONC_LIB} Threads::Threads)
    endif()
endif()

# Optional libbpf support (Phase 3 scaffold)
check_include_file(bpf/libbpf.h HAVE_LIBBPF_H)
if(HAVE_LIBBPF_H)
    find_library(LIBBPF_LIB bpf)
    if(LIBBPF_LIB)
        target_compile_definitions(mycoflowd PRIVATE HAVE_LIBBPF)
        target_link_libraries(mycoflowd PRIVATE ${LIBBPF_LIB})
    endif()
endif()

# Binary dosyasının çıkış adını sabitleyelim (opsiyonel)
set_target_properties(mycoflowd PROPERTIES OUTPUT_NAME "mycoflowd")

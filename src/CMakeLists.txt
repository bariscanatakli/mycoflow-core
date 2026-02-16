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
    myco_ewma.c
    myco_ebpf.c
    myco_netlink.c
    myco_ubus.c
)



# Executable oluştur (mycoflowd)
add_executable(mycoflowd ${SOURCES})

# Math library (needed for fabs in sense module)
find_package(Threads REQUIRED)
target_link_libraries(mycoflowd PRIVATE m Threads::Threads)

# Tests
# enable_testing() moved to root

add_executable(test_ewma tests/test_ewma.c myco_ewma.c)
add_test(NAME ewma COMMAND test_ewma)

add_executable(test_control tests/test_control.c myco_control.c myco_log.c myco_config.c myco_act.c myco_persona.c)
target_link_libraries(test_control PRIVATE m Threads::Threads)
add_test(NAME control COMMAND test_control)

add_executable(test_config tests/test_config.c myco_config.c myco_log.c)
add_test(NAME config COMMAND test_config)

add_executable(test_persona tests/test_persona.c myco_persona.c myco_log.c)
add_test(NAME persona COMMAND test_persona)

# Optional ubus support (OpenWrt)
check_include_file(libubus.h HAVE_UBUS_H)
if(HAVE_UBUS_H)
    target_compile_definitions(mycoflowd PRIVATE HAVE_UBUS)
    target_link_libraries(mycoflowd PRIVATE ubus ubox blobmsg_json)
endif()

# eBPF compilation (if clang is available)
find_program(CLANG_EXE clang)
if(CLANG_EXE)
    message(STATUS "Found clang: ${CLANG_EXE}")
    execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH_UNAME OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(ARCH_INCLUDE "/usr/include/${ARCH_UNAME}-linux-gnu")
    
    add_custom_command(OUTPUT mycoflow.bpf.o
        COMMAND ${CLANG_EXE} -O2 -target bpf -I${ARCH_INCLUDE} -c ${CMAKE_CURRENT_SOURCE_DIR}/bpf/mycoflow.bpf.c -o mycoflow.bpf.o
        DEPENDS bpf/mycoflow.bpf.c
    )
    add_custom_target(bpf_target ALL DEPENDS mycoflow.bpf.o)
    add_dependencies(mycoflowd bpf_target)
    
    # Needs libelf and zlib for libbpf
    find_package(ZLIB)
    if(ZLIB_FOUND)
        target_link_libraries(mycoflowd PRIVATE ZLIB::ZLIB)
    endif()
    # LibElf check omitted for brevity in standard cmake, usually implicit or manual
    # OpenWrt handles library linking via package definition
else()
    message(WARNING "clang not found, BPF program will not be compiled")
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

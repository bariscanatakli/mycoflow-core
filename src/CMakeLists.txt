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
    myco_flow.c
    myco_device.c
    myco_hint.c
    myco_dns.c
    myco_service.c
    myco_mark.c
    myco_mangle.c
    myco_rtt.c
    myco_classifier.c
    myco_profile.c
    myco_ubus.c
)



# Executable oluştur (mycoflowd)
add_executable(mycoflowd ${SOURCES})

# Math library (needed for fabs in sense module)
find_package(Threads REQUIRED)
target_link_libraries(mycoflowd PRIVATE m Threads::Threads)

# Static build option — produces a self-contained binary that runs on OpenWrt
# (musl) without glibc dependency. Enable with: cmake -DSTATIC_BUILD=ON ...
option(STATIC_BUILD "Link mycoflowd statically (for OpenWrt VM deployment)" OFF)
if(STATIC_BUILD)
    message(STATUS "Static build enabled — mycoflowd will link statically")
    target_link_options(mycoflowd PRIVATE -static)
endif()

# Tests
# enable_testing() moved to root

add_executable(test_ewma tests/test_ewma.c myco_ewma.c)
add_test(NAME ewma COMMAND test_ewma)

add_executable(test_act tests/test_act.c myco_act.c myco_log.c myco_config.c myco_persona.c)
target_link_libraries(test_act PRIVATE m Threads::Threads)
add_test(NAME act COMMAND test_act)

add_executable(test_control tests/test_control.c myco_control.c myco_log.c myco_config.c myco_act.c myco_persona.c)
target_link_libraries(test_control PRIVATE m Threads::Threads)
add_test(NAME control COMMAND test_control)

add_executable(test_config tests/test_config.c myco_config.c myco_log.c)
add_test(NAME config COMMAND test_config)

add_executable(test_persona tests/test_persona.c myco_persona.c myco_log.c)
add_test(NAME persona COMMAND test_persona)

add_executable(test_hint tests/test_hint.c myco_hint.c myco_service.c)
add_test(NAME hint COMMAND test_hint)

add_executable(test_dns tests/test_dns.c myco_dns.c myco_service.c myco_log.c)
target_link_libraries(test_dns PRIVATE Threads::Threads)
add_test(NAME dns COMMAND test_dns)

add_executable(test_device tests/test_device.c myco_device.c myco_persona.c myco_flow.c myco_hint.c myco_dns.c myco_service.c myco_log.c)
target_link_libraries(test_device PRIVATE Threads::Threads)
add_test(NAME device COMMAND test_device)

add_executable(test_service tests/test_service.c myco_service.c)
add_test(NAME service COMMAND test_service)

add_executable(test_rtt tests/test_rtt.c myco_rtt.c myco_log.c)
add_test(NAME rtt COMMAND test_rtt)

# Optional libnetfilter_conntrack support (real ct mark push).
# Checked before test targets so tests can opt into real impl too.
check_include_file(libnetfilter_conntrack/libnetfilter_conntrack.h HAVE_LIBNFCT_H)
if(HAVE_LIBNFCT_H)
    find_library(LIBNFCT_LIB netfilter_conntrack)
    if(LIBNFCT_LIB)
        message(STATUS "Found libnetfilter_conntrack: ${LIBNFCT_LIB}")
        target_compile_definitions(mycoflowd PRIVATE HAVE_LIBNFCT)
        target_link_libraries(mycoflowd PRIVATE ${LIBNFCT_LIB})
    endif()
endif()

add_executable(test_mangle tests/test_mangle.c myco_mangle.c myco_mark.c myco_log.c)
add_test(NAME mangle COMMAND test_mangle)
if(HAVE_LIBNFCT_H AND LIBNFCT_LIB)
    target_compile_definitions(test_mangle PRIVATE HAVE_LIBNFCT)
    target_link_libraries(test_mangle PRIVATE ${LIBNFCT_LIB})
endif()

add_executable(test_profile tests/test_profile.c myco_profile.c myco_service.c myco_mangle.c myco_log.c)
add_test(NAME profile COMMAND test_profile)

add_executable(test_classifier tests/test_classifier.c
    myco_classifier.c myco_service.c myco_hint.c myco_dns.c
    myco_flow.c myco_mark.c myco_rtt.c myco_log.c)
target_link_libraries(test_classifier PRIVATE Threads::Threads)
add_test(NAME classifier COMMAND test_classifier)
if(HAVE_LIBNFCT_H AND LIBNFCT_LIB)
    target_compile_definitions(test_classifier PRIVATE HAVE_LIBNFCT)
    target_link_libraries(test_classifier PRIVATE ${LIBNFCT_LIB})
endif()

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
    
    # -g is required for BTF generation (libbpf needs BTF to verify/load programs, kernel >= 5.2)
    # For non-Debug builds, strip DWARF debug info (--strip-debug) while preserving BTF sections
    find_program(LLVM_STRIP_EXE llvm-strip)
    set(BPF_STRIP_CMD "")
    if(LLVM_STRIP_EXE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(BPF_STRIP_CMD COMMAND ${LLVM_STRIP_EXE} --strip-debug mycoflow.bpf.o)
        message(STATUS "BPF: DWARF will be stripped for ${CMAKE_BUILD_TYPE} build (BTF preserved)")
    endif()
    add_custom_command(OUTPUT mycoflow.bpf.o
        COMMAND ${CLANG_EXE} -O2 -g -target bpf -I${ARCH_INCLUDE} -c ${CMAKE_CURRENT_SOURCE_DIR}/bpf/mycoflow.bpf.c -o mycoflow.bpf.o
        ${BPF_STRIP_CMD}
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

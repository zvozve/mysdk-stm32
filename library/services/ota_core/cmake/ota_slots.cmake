# ============================================================================
# ota_slots.cmake —— 多槽 OTA 的三分片构建（由 services.ota_core 模块提供）
#
# 由 <MySDK>/CMakeLists.txt 在末尾自动 include —— **工程根 CMakeLists 无需任何改动**
# （想在工程侧手动接管时才自己 include 本文件）。
#
# 产出（三分片。链接地址各自烤死：Cortex-M 绝对寻址，「一个位置 = 一份二进制」）：
#     <proj>      base   基版 .ld 的 ORIGIN/LENGTH —— 不带 BL，可独立烧到默认位置运行
#     <proj>_A    slotA  由基版派生
#     <proj>_B    slotB  由基版派生
#   三份 .ld 一并生成到 ${CMAKE_BINARY_DIR}（= 与 .bin 同目录）；根目录那份基版 .ld 只读不改。
#   地址全部从 .ld 派生，不写死任何偏移（槽几何只在第 3 步给一次，与工程分区表对齐）。
#
# 为什么用 DEFER 把「建 A/B 目标」推迟到工程根目录处理结束时：
#   CubeMX 的 cmake/stm32cubemx/CMakeLists.txt 只认 ${CMAKE_PROJECT_NAME} 一个目标，
#   base 的源分两处加进去（CubeMX 子目录 + 工程根的用户源），而本文件被 include 时
#   用户源还没加 —— 那时 get_target_property(SOURCES) 会漏源、A/B 会缺文件。
#   （相比之下 CMAKE_EXE_LINKER_FLAGS 的修改不受时机限制：实测晚改对已创建的目标同样生效。）
#
# 可覆盖（在 include 之前 set，或用 -D 传）：
#   OTA_MULTISLOT        OFF = 关掉整个多槽逻辑（只留 base 一份产物）
#   OTA_BASE_LD          基版链接脚本路径；默认在 ${CMAKE_SOURCE_DIR} 下自动找唯一的 *.ld
#   OTA_SLOT_A_ORIGIN / OTA_SLOT_A_LENGTH   显式覆盖槽 A 几何（默认按基版容量派生）
#   OTA_SLOT_B_ORIGIN / OTA_SLOT_B_LENGTH   同上
#   OTA_SELF_BASE_MACRO  注入应用层的「我在哪」宏名；默认 OTA_SELF_BASE
# ============================================================================

if(DEFINED OTA_MULTISLOT AND NOT OTA_MULTISLOT)
    message(STATUS "[ota_slots] OTA_MULTISLOT=OFF -> 只生成 base 一份产物")
    return()
endif()

if(CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR "[ota_slots] 需要 CMake >= 3.19（用到 cmake_language(DEFER)）；当前为 ${CMAKE_VERSION}")
endif()


# .ld 里的尺寸写法不统一（`0x8000000` / `1024K` / `0x00070000` 都见过），
# CMake 的 math() 又不认 `1024K` —— 所以统一先解析成十进制整数。
# 支持：0x… / 十进制 / …K / …M（与 tools/fw-flash.py 的 _size_to_int 同规则）。
macro(_otaslot_size_to_int in_var out_var)
    string(STRIP "${${in_var}}" _otaslot_s)
    string(TOUPPER "${_otaslot_s}" _otaslot_S)
    if(_otaslot_S MATCHES "^0X([0-9A-F]+)$")
        math(EXPR ${out_var} "0x${CMAKE_MATCH_1}")
    elseif(_otaslot_S MATCHES "^([0-9]+)K$")
        math(EXPR ${out_var} "${CMAKE_MATCH_1} * 1024")
    elseif(_otaslot_S MATCHES "^([0-9]+)M$")
        math(EXPR ${out_var} "${CMAKE_MATCH_1} * 1024 * 1024")
    elseif(_otaslot_S MATCHES "^[0-9]+$")
        math(EXPR ${out_var} "${_otaslot_S}")
    else()
        message(FATAL_ERROR "[ota_slots] 无法解析链接脚本里的尺寸 '${${in_var}}'（支持 0x… / 十进制 / …K / …M）")
    endif()
endmacro()


# 用 macro（不是 function）是刻意的：macro 不创建变量作用域，因此它内部对
# CMAKE_EXE_LINKER_FLAGS 的修改直接落在**工程根目录作用域**，这正是各目标链接时读取的
# 那一份；换成 function 就得靠 PARENT_SCOPE 层层转，反而容易漏。
macro(ota_enable_multi_slot)

  if(NOT OTA_SLOT_MULTI_DONE)
    set(OTA_SLOT_MULTI_DONE TRUE)

    # ⚠ 默认值必须在这里给：本 macro 在**工程根**作用域执行（DEFER），而在文件顶层
    #   set 只作用于 <MySDK>/CMakeLists.txt 的子目录作用域 —— 根目录读不到，
    #   于是 ${OTA_SELF_BASE_MACRO} 展开成空，命令行会变成非法的 `-D=0x8000000`。
    if(NOT OTA_SELF_BASE_MACRO)
        set(OTA_SELF_BASE_MACRO OTA_SELF_BASE)
    endif()

    # ---- 1) 定位基版 .ld ----
    if(NOT OTA_BASE_LD)
        file(GLOB _otaslot_lds "${CMAKE_SOURCE_DIR}/*.ld")
        list(LENGTH _otaslot_lds _otaslot_n)
        if(_otaslot_n EQUAL 0)
            message(FATAL_ERROR "[ota_slots] 在 ${CMAKE_SOURCE_DIR} 找不到任何 *.ld；"
                                "请 set(OTA_BASE_LD <基版链接脚本>)")
        elseif(_otaslot_n GREATER 1)
            message(FATAL_ERROR "[ota_slots] ${CMAKE_SOURCE_DIR} 下有多个 *.ld，无法判断哪份是基版：\n"
                                "    ${_otaslot_lds}\n"
                                "    请 set(OTA_BASE_LD <基版链接脚本>)")
        endif()
        set(OTA_BASE_LD "${_otaslot_lds}")
    endif()
    if(NOT EXISTS "${OTA_BASE_LD}")
        message(FATAL_ERROR "[ota_slots] OTA_BASE_LD 不存在：${OTA_BASE_LD}")
    endif()

    # ---- 2) 读基版 FLASH 几何（ORIGIN / LENGTH 原样取出，后面按槽改写）----
    file(READ "${OTA_BASE_LD}" _otaslot_txt)
    string(REGEX MATCH
           "FLASH[ \t]*\\([^)]*\\)[ \t]*:[ \t]*ORIGIN[ \t]*=[ \t]*([^,\r\n]+)[ \t\r\n]*,[ \t\r\n]*LENGTH[ \t]*=[ \t\r\n]*([^ \t\r\n]+)"
           _otaslot_flash "${_otaslot_txt}")
    if(NOT _otaslot_flash)
        message(FATAL_ERROR "[ota_slots] ${OTA_BASE_LD} 里找不到 FLASH ORIGIN/LENGTH 行，无法派生槽几何")
    endif()
    set(_otaslot_base_origin "${CMAKE_MATCH_1}")
    set(_otaslot_base_len    "${CMAKE_MATCH_2}")
    _otaslot_size_to_int(_otaslot_base_origin _otaslot_base_origin_val)
    _otaslot_size_to_int(_otaslot_base_len    _otaslot_base_len_val)

    # ---- 3) 槽几何：默认由基版容量派生（1MB → A = +0x10000/448K、B = +0x80000/512K，
    #        与工程分区表 User/Src/ota_areas.c 对齐）；可用 OTA_SLOT_x_* 覆盖 ----
    if(_otaslot_base_len_val EQUAL 1048576)
        # ⚠ 变量名用大写 A/B，与下面 foreach(_s A B) 的 ${_s} 保持一致 ——
        #   大小写不一致会静默取到空串，生成出 `ORIGIN = , LENGTH =` 的坏链接脚本。
        math(EXPR _otaslot_A_origin "${_otaslot_base_origin_val} + 0x10000" OUTPUT_FORMAT HEXADECIMAL)
        math(EXPR _otaslot_A_len    "0x70000"                              OUTPUT_FORMAT HEXADECIMAL)
        math(EXPR _otaslot_B_origin "${_otaslot_base_origin_val} + 0x80000" OUTPUT_FORMAT HEXADECIMAL)
        math(EXPR _otaslot_B_len    "0x80000"                              OUTPUT_FORMAT HEXADECIMAL)
    else()
        message(FATAL_ERROR "[ota_slots] 未知的基版 FLASH 容量 ${_otaslot_base_len}（只认得 1MB），"
                            "无法派生槽几何；请显式给 OTA_SLOT_A_ORIGIN/LENGTH 与 OTA_SLOT_B_ORIGIN/LENGTH")
    endif()
    foreach(_s A B)
        if(DEFINED OTA_SLOT_${_s}_ORIGIN)
            set(_otaslot_${_s}_origin "${OTA_SLOT_${_s}_ORIGIN}")
        endif()
        if(DEFINED OTA_SLOT_${_s}_LENGTH)
            set(_otaslot_${_s}_len "${OTA_SLOT_${_s}_LENGTH}")
        endif()
    endforeach()

    # ---- 4) 生成三份 .ld 到 ${CMAKE_BINARY_DIR}（与 .bin 同目录）----
    #     base 那份是基版的原样副本 —— 根目录的原始 .ld 只读不改。
    file(WRITE "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.ld" "${_otaslot_txt}")
    foreach(_s A B)
        string(REPLACE "${_otaslot_base_origin}" "${_otaslot_${_s}_origin}" _otaslot_line "${_otaslot_flash}")
        string(REPLACE "${_otaslot_base_len}"    "${_otaslot_${_s}_len}"    _otaslot_line "${_otaslot_line}")
        string(REPLACE "${_otaslot_flash}" "${_otaslot_line}" _otaslot_out "${_otaslot_txt}")
        file(WRITE "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}_${_s}.ld" "${_otaslot_out}")
    endforeach()
    message(STATUS "[ota_slots] base ${_otaslot_base_origin}/${_otaslot_base_len}"
                   "  slotA ${_otaslot_A_origin}/${_otaslot_A_len}"
                   "  slotB ${_otaslot_B_origin}/${_otaslot_B_len}")

    # ---- 5) 摘掉 toolchain 里全局的 -T <基版.ld> ----
    #   留着它每个目标都会带两个 -T：ld 会看到两个同名 MEMORY 区域（实测 redeclaration
    #   警告 + 段分配算两遍），所以改由各目标在下一步各指定唯一一份。
    string(REGEX MATCH "-T[ \t\r\n]+\"${OTA_BASE_LD}\"" _otaslot_ldflag "${CMAKE_EXE_LINKER_FLAGS}")
    if(_otaslot_ldflag)
        string(REPLACE "${_otaslot_ldflag}" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    else()
        message(WARNING "[ota_slots] CMAKE_EXE_LINKER_FLAGS 里没找到 -T \"${OTA_BASE_LD}\"；"
                        "若 toolchain 确有全局 -T，会出现重复 MEMORY 声明，请核对 toolchain 文件")
    endif()

    # ---- 6) base 目标：链接脚本 / 独立 map / 基址宏 ----
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        -T "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.ld"
        -Wl,-Map=${CMAKE_PROJECT_NAME}.map)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        ${OTA_SELF_BASE_MACRO}=${_otaslot_base_origin})

    # ---- 7) A/B 目标：复制 base 的源与链接库 ----
    #   用 get_target_property 而不是手抄源表：CubeMX 下次重新生成时新增/删除的文件
    #   会自动跟着走，不会悄悄漏源。
    get_target_property(_otaslot_srcs ${CMAKE_PROJECT_NAME} SOURCES)
    get_target_property(_otaslot_libs ${CMAKE_PROJECT_NAME} LINK_LIBRARIES)
    get_target_property(_otaslot_incs ${CMAKE_PROJECT_NAME} INCLUDE_DIRECTORIES)
    get_target_property(_otaslot_defs ${CMAKE_PROJECT_NAME} COMPILE_DEFINITIONS)

    foreach(_s A B)
        set(_t "${CMAKE_PROJECT_NAME}_${_s}")

        add_executable(${_t} ${_otaslot_srcs})
        if(_otaslot_libs)
            target_link_libraries(${_t} ${_otaslot_libs})
        endif()
        if(_otaslot_incs)
            target_include_directories(${_t} PRIVATE ${_otaslot_incs})
        endif()
        if(_otaslot_defs)
            target_compile_definitions(${_t} PRIVATE ${_otaslot_defs})
        endif()

        # 「我在哪个槽」是链接期事实：应用层 app_ota.c 用它设 VTOR / 反查自身槽号。
        # （该宏只被应用层使用，SDK 不用它 —— 所以三片能共享同一批 SDK/HAL 目标文件，
        #   只有应用层会编三遍。）
        target_compile_definitions(${_t} PRIVATE ${OTA_SELF_BASE_MACRO}=${_otaslot_${_s}_origin})

        target_link_options(${_t} PRIVATE
            -T "${CMAKE_BINARY_DIR}/${_t}.ld"
            -Wl,-Map=${_t}.map)
        set_target_properties(${_t} PROPERTIES ADDITIONAL_CLEAN_FILES ${_t}.map)

        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${_t}> $<TARGET_FILE_DIR:${_t}>/${_t}.hex
            COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${_t}> $<TARGET_FILE_DIR:${_t}>/${_t}.bin
            COMMENT "Generating HEX and BIN firmware (${_t})")
    endforeach()

  endif()
endmacro()


# 注册到**工程根目录**：等根目录（含所有子目录）处理完再执行 —— 那时 base 目标的
# 源与链接库才齐全，复制给 A/B 才不会漏。
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL ota_enable_multi_slot)

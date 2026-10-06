cmake_minimum_required(VERSION 3.16)

# 注意：CMake 的 project(VERSION) 只接受纯数字，带后缀的版本号单独用 NETSCRATCH_VERSION 表示
project(NETScratch VERSION 1.0 LANGUAGES CXX)

set(NETSCRATCH_VERSION "1.0-beta")

# 本项目只保留 Linux 构建（Debian 系 / 红帽系 / Arch 系 / SUSE 系）
if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "NETScratch 仅支持 Linux 构建（Debian/RHEL/Arch/SUSE）")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 源码为 UTF-8，强制 GCC/Clang 以 UTF-8 处理输入与执行字符集
add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)

# 未显式指定 Qt 路径时的常见回落（各发行版 Qt5 安装位置）
if(NOT CMAKE_PREFIX_PATH)
    foreach(_qt_guess "/usr/lib/x86_64-linux-gnu/cmake" "/usr/lib64/cmake" "/usr/lib/cmake")
        if(EXISTS "${_qt_guess}/Qt5")
            list(APPEND CMAKE_PREFIX_PATH "/usr")
            break()
        endif()
    endforeach()
endif()

find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Network)

# ---------------------------------------------------------------- Qt 目录定位
# 用 qmake -query 拿到 Qt 的库目录 / 插件目录，比手工猜路径可靠（各发行版差异很大）
get_target_property(_QT_QMAKE Qt5::qmake IMPORTED_LOCATION)
if(NOT _QT_QMAKE)
    find_program(_QT_QMAKE NAMES qmake-qt5 qmake)
endif()
if(NOT _QT_QMAKE)
    message(FATAL_ERROR "未找到 qmake，无法定位 Qt 安装布局")
endif()

execute_process(COMMAND "${_QT_QMAKE}" -query QT_INSTALL_LIBS
                OUTPUT_VARIABLE QT_LIBS_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${_QT_QMAKE}" -query QT_INSTALL_PLUGINS
                OUTPUT_VARIABLE QT_PLUGINS_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT QT_LIBS_DIR OR NOT QT_PLUGINS_DIR)
    message(FATAL_ERROR "无法通过 qmake -query 获取 Qt 库/插件目录")
endif()
message(STATUS "Qt 库目录:   ${QT_LIBS_DIR}")
message(STATUS "Qt 插件目录: ${QT_PLUGINS_DIR}")

# ---------------------------------------------------------------- 源文件
set(PROJECT_SOURCES
    src/main.cpp

    src/lang.h
    src/lang.cpp

    src/appsettings.h
    src/appsettings.cpp

    src/netcompat.h

    src/netutils.h
    src/netutils.cpp

    src/cameradetector.h
    src/cameradetector.cpp

    src/netdiscovery.h
    src/netdiscovery.cpp

    src/scanner.h
    src/scanner.cpp

    src/nmaprunner.h
    src/nmaprunner.cpp

    src/scanhistory.h
    src/scanhistory.cpp

    src/ipaddressedit.h
    src/ipaddressedit.cpp

    src/ipgridpanel.h
    src/ipgridpanel.cpp

    src/subnetresultpanel.h
    src/subnetresultpanel.cpp

    src/settingspanel.h
    src/settingspanel.cpp

    src/historypanel.h
    src/historypanel.cpp

    src/uistyle.h

    src/toggleswitch.h
    src/toggleswitch.cpp

    src/mainwindow.h
    src/mainwindow.cpp
)

add_executable(NETScratch
    ${PROJECT_SOURCES}
    resources.qrc
)

target_link_libraries(NETScratch PRIVATE
    Qt5::Widgets
    Qt5::Network
)

# ---------------------------------------------------------------- RPATH
# 用 DT_RPATH（--disable-new-dtags）而非 DT_RUNPATH：DT_RPATH 会传递给 dlopen()
# 加载的 Qt 插件，插件才能在同一 release 目录里找到 Qt 运行库。
target_link_options(NETScratch PRIVATE "LINKER:--disable-new-dtags")
set_target_properties(NETScratch PROPERTIES
    # 构建时：指向系统 Qt 库
    BUILD_RPATH "${QT_LIBS_DIR}"
    # 安装/部署后：相对可执行文件自身目录
    INSTALL_RPATH "$ORIGIN"
    INSTALL_RPATH_USE_LINK_PATH OFF
)

# ---------------------------------------------------------------- 部署规则
set(NETSCRATCH_RELEASE_DIR "${CMAKE_SOURCE_DIR}/release" CACHE PATH "release 输出目录")

# qt.conf：告诉 Qt 运行库/插件就在可执行文件所在目录（便携式发布）
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf"
"[Paths]\nPrefix=.\nPlugins=.\n")

install(TARGETS NETScratch RUNTIME DESTINATION . COMPONENT runtime)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" DESTINATION . COMPONENT runtime)

# 插件拷贝 + 递归收集运行库依赖的动作放在 install 脚本里执行
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/deploy_linux.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/deploy_linux.cmake"
    @ONLY
)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/deploy_linux.cmake" COMPONENT runtime)

# 用户要求：只产出包含全部运行库 + 可执行文件的 release 目录（不打包 zip/安装器）
# 只安装 runtime 组件，避免与下面的安装包（bundle）组件相互污染
add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}"
            --prefix "${NETSCRATCH_RELEASE_DIR}" --component runtime
    DEPENDS NETScratch
    COMMENT "部署到 ${NETSCRATCH_RELEASE_DIR}（含 Qt 运行库与插件、系统依赖库）"
    VERBATIM
)

# ============================================================================
# 安装包（deb / rpm）
# ----------------------------------------------------------------------------
# 采用「自包含」打包：把 release/ 里的全部 Qt 运行库、插件与可执行文件一并装入
# /opt/NETScratch，/usr/bin/netscratch 为启动器，并附带 .desktop 桌面项与图标。
# 在 Debian / RHEL / Arch / SUSE 上安装后无需再单独安装 Qt 即可运行。
#   生成命令：cmake --build build --target deploy && cmake --build build --target package
# ============================================================================
file(MAKE_DIRECTORY "${NETSCRATCH_RELEASE_DIR}")

install(DIRECTORY "${NETSCRATCH_RELEASE_DIR}/"
        DESTINATION "opt/NETScratch"
        COMPONENT bundle
        USE_SOURCE_PERMISSIONS)
install(PROGRAMS "${CMAKE_SOURCE_DIR}/packaging/netscratch"
        DESTINATION "usr/bin"
        COMPONENT bundle)
install(FILES "${CMAKE_SOURCE_DIR}/packaging/netscratch.desktop"
        DESTINATION "usr/share/applications"
        COMPONENT bundle)
install(FILES "${CMAKE_SOURCE_DIR}/packaging/netscratch.png"
        DESTINATION "usr/share/icons/hicolor/256x256/apps"
        COMPONENT bundle)

# 只打包 bundle 组件；runtime 组件（release 部署）不进入安装包
set(CPACK_COMPONENTS_ALL bundle)
# 关键：CPackDeb/CPackRPM 默认会执行全部 install() 规则（组件过滤不生效），
# 会把 runtime 组件里 DESTINATION 为 "." 的 NETScratch / qt.conf 泄漏到包根 "/"。
# 显式指定只安装 bundle 组件：<构建目录>;<工程名>;<组件名>;<安装子目录>
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};NETScratch;bundle;/")
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/packages")

set(CPACK_PACKAGE_NAME "netscratch")
set(CPACK_PACKAGE_VENDOR "NETScratch")
set(CPACK_PACKAGE_CONTACT "NETScratch Project")
set(CPACK_PACKAGE_VERSION "${NETSCRATCH_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "多线程局域网 IP 扫描工具")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://example.org/netscratch")
# 运行库由 deploy 脚本统一处理，这里不做二次 strip
set(CPACK_STRIP_FILES OFF)

# ---- Debian 系（.deb）----
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    set(_deb_arch "amd64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_deb_arch "arm64")
else()
    set(_deb_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_DEBIAN_PACKAGE_NAME "netscratch")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_deb_arch}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "NETScratch Project")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# 运行期仍需的外部命令（Qt 已随包自带）
set(CPACK_DEBIAN_PACKAGE_DEPENDS "nmap, iputils-ping, iproute2")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# ---- 红帽 / SUSE 系（.rpm）----
set(CPACK_RPM_PACKAGE_NAME "netscratch")
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Network")
set(CPACK_RPM_PACKAGE_REQUIRES "nmap, iputils, iproute")
set(CPACK_RPM_PACKAGE_SUMMARY "多线程局域网 IP 扫描工具")
set(CPACK_RPM_PACKAGE_DESCRIPTION
    "NETScratch 是一个多线程局域网 IP 扫描工具，支持主机发现、端口与摄像头探测及历史记录。包内自带 Qt 运行库与插件。")
# 关闭自动依赖探测：否则会对随包自带的 Qt 库生成 libQt5*.so 形式的依赖项
set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
set(CPACK_RPM_PACKAGE_AUTOPROV OFF)
set(CPACK_RPM_FILE_NAME "netscratch-${NETSCRATCH_VERSION}-${CMAKE_SYSTEM_PROCESSOR}.rpm")

set(CPACK_GENERATOR "DEB;RPM")
include(CPack)

# 便捷目标：先确保 release 已生成（安装包内容取自 release/），再分别产出 deb / rpm
if(NOT CMAKE_CPACK_COMMAND)
    find_program(CMAKE_CPACK_COMMAND NAMES cpack)
endif()
if(NOT CMAKE_CPACK_COMMAND)
    message(FATAL_ERROR "未找到 cpack，无法生成安装包")
endif()

add_custom_target(package_deb
    COMMAND "${CMAKE_CPACK_COMMAND}" -G DEB
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS deploy
    COMMENT "生成 .deb 安装包到 ${CPACK_PACKAGE_DIRECTORY}"
    VERBATIM
)

add_custom_target(package_rpm
    COMMAND "${CMAKE_CPACK_COMMAND}" -G RPM
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS deploy
    COMMENT "生成 .rpm 安装包到 ${CPACK_PACKAGE_DIRECTORY}"
    VERBATIM
)

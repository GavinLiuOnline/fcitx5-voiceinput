cmake_minimum_required(VERSION 3.10)
project(fcitx5-voiceinput VERSION 1.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(FeatureSummary)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FCITX5CORE REQUIRED IMPORTED_TARGET Fcitx5Core)

# fcitx5 loads addon libraries ONLY from the addon dir compiled into
# libFcitx5Core (FCITX_INSTALL_ADDONDIR, always "<libdir>/fcitx5") or the
# FCITX_ADDON_DIRS env var. It does NOT scan <prefix>/lib/fcitx5, so the
# library must be installed next to the running fcitx5's own addons,
# derived here from pkg-config instead of CMAKE_INSTALL_LIBDIR.
pkg_get_variable(FCITX5CORE_LIBDIR Fcitx5Core libdir)
# Default: install next to the running fcitx5's own addons (absolute path
# from pkg-config, fcitx5 does NOT scan <prefix>/lib/fcitx5). Snap builds
# flip VOICEINPUT_RELOCATABLE=ON so everything stays inside the prefix.
option(VOICEINPUT_RELOCATABLE
       "Install the addon under the CMake prefix (snap packaging)" OFF)
if(VOICEINPUT_RELOCATABLE)
    set(FCITX5_ADDON_DIR "${CMAKE_INSTALL_LIBDIR}/fcitx5")
else()
    set(FCITX5_ADDON_DIR "${FCITX5CORE_LIBDIR}/fcitx5" CACHE PATH
        "fcitx5 addon library directory")
endif()
set(FCITX5_DATA_DIR "${CMAKE_INSTALL_DATADIR}/fcitx5" CACHE PATH
    "fcitx5 data directory")
message(STATUS "fcitx5 addon library dir: ${FCITX5_ADDON_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(fcitx5-voiceinput MODULE src/voiceinput.cpp)
set_target_properties(fcitx5-voiceinput PROPERTIES PREFIX "")
target_link_libraries(fcitx5-voiceinput PRIVATE PkgConfig::FCITX5CORE)
target_compile_definitions(fcitx5-voiceinput PRIVATE
    FCITX_GETTEXT_DOMAIN="fcitx5-voiceinput"
    VOICEINPUT_BACKEND_PATH="${CMAKE_INSTALL_FULL_DATADIR}/fcitx5/voiceinput/voice_backend.py"
    VOICEINPUT_LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}")

install(TARGETS fcitx5-voiceinput DESTINATION "${FCITX5_ADDON_DIR}")
install(FILES src/voiceinput.conf DESTINATION "${FCITX5_DATA_DIR}/addon")
install(PROGRAMS backend/voice_backend.py
        DESTINATION "${FCITX5_DATA_DIR}/voiceinput")
install(PROGRAMS scripts/install-snap-addon.sh
        DESTINATION "${FCITX5_DATA_DIR}/voiceinput")

# Compile and install the addon gettext catalog so fcitx5-configtool can
# show localized option labels (domain "fcitx5-voiceinput").
find_program(MSGFMT_EXECUTABLE msgfmt)
if(MSGFMT_EXECUTABLE)
    set(VOICEINPUT_PO_LANG "zh_CN")
    set(VOICEINPUT_GMO
        "${CMAKE_CURRENT_BINARY_DIR}/${VOICEINPUT_PO_LANG}.gmo")
    add_custom_command(
        OUTPUT "${VOICEINPUT_GMO}"
        COMMAND "${MSGFMT_EXECUTABLE}" -o "${VOICEINPUT_GMO}"
                "${CMAKE_CURRENT_SOURCE_DIR}/po/${VOICEINPUT_PO_LANG}.po"
        DEPENDS "po/${VOICEINPUT_PO_LANG}.po"
        COMMENT "Compiling ${VOICEINPUT_PO_LANG}.po")
    add_custom_target(voiceinput_po ALL DEPENDS "${VOICEINPUT_GMO}")
    install(FILES "${VOICEINPUT_GMO}"
            DESTINATION
                "${CMAKE_INSTALL_LOCALEDIR}/${VOICEINPUT_PO_LANG}/LC_MESSAGES"
            RENAME "fcitx5-voiceinput.mo")
else()
    message(STATUS "msgfmt not found: zh_CN catalog will not be installed")
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

# ---- Packaging: deb via CPack (snap is built by snapcraft.yaml) ----
set(CPACK_PACKAGE_NAME "fcitx5-voiceinput")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "fcitx5-voiceinput contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local voice input addon for fcitx5")
set(CPACK_PACKAGE_DESCRIPTION
    "Press a hotkey, speak, and the recognized text is typed into the
focused application. Speech recognition (SenseVoice / Paraformer /
Whisper / FireRedASR via sherpa-onnx) and endpoint detection (Silero VAD)
run completely locally - no cloud service.

Requires the sherpa-onnx python package: pip3 install --user sherpa-onnx")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER
    "fcitx5-voiceinput contributors <noreply@example.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "fcitx5, python3, python3-pip, python3-numpy, alsa-utils | pipewire-utils")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst")
# GTK/Qt apps are most reliable over their immodules (dbus frontend);
# without these packages they fall back to the ibus link where a commit may
# be dropped (GNOME Wayland). Ubuntu 22.04/Debian 12 split them into
# fcitx5-frontend-* binaries; older layouts use fcitx5-gtk/fcitx5-qt.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
    "fcitx5-frontend-gtk3 | fcitx5-gtk, fcitx5-frontend-gtk4 | fcitx5-gtk, fcitx5-frontend-qt5 | fcitx5-qt")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_BINARY_DIR}/dist")
include(CPack)

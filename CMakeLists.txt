cmake_minimum_required(VERSION 3.21)

project(shine VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHINE_BUILD_UI "Build Qt6 + QML minimalist GUI when Qt6 is available" ON)
option(SHINE_REQUIRE_UI "Fail configure if SHINE_BUILD_UI is ON and Qt6 is not found" OFF)
option(SHINE_BUILD_TESTS "Build Shine test suite" ON)

add_library(shine_warnings INTERFACE)
if(MSVC)
    target_compile_options(shine_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(shine_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

set(SHINE_CORE_SOURCES
    src/core/clip_library.cpp
    src/core/hotkey.cpp
    src/core/mp4_validator.cpp
    src/core/replay_buffer.cpp
    src/core/settings.cpp
    src/ipc/protocol.cpp
)

if(WIN32)
    list(APPEND SHINE_CORE_SOURCES
        src/platform/windows/dxgi_capture.cpp
        src/platform/windows/d3d11_converter.cpp
        src/platform/windows/mf_encoder.cpp
        src/platform/windows/mf_replay_encoder.cpp
        src/platform/windows/win_audio.cpp
        src/platform/windows/win_autostart.cpp
        src/platform/windows/win_hotkey.cpp
        src/platform/windows/win_ipc.cpp
        src/platform/windows/win_recorder.cpp
        src/platform/windows/win_replay_recorder.cpp
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND SHINE_CORE_SOURCES
        src/platform/linux/x11_capture.cpp
        src/platform/linux/vaapi_encoder.cpp
        src/platform/linux/linux_audio.cpp
        src/platform/linux/linux_autostart.cpp
        src/platform/linux/linux_hotkey.cpp
        src/platform/linux/linux_ipc.cpp
        src/platform/linux/linux_recorder.cpp
        src/platform/linux/linux_replay_recorder.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform for shine")
endif()

add_library(shine_core STATIC ${SHINE_CORE_SOURCES})

target_include_directories(shine_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(shine_core PRIVATE shine_warnings)

if(WIN32)
    target_compile_definitions(shine_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_link_libraries(shine_core PUBLIC
        d3d11
        dxgi
        mfplat
        mfreadwrite
        mfuuid
        mf
        wmcodecdspuuid
        ole32
        psapi
        user32
        advapi32
        shell32
    )
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SHINE_LINUX_DEPS REQUIRED IMPORTED_TARGET
        x11
        xext
        xfixes
        xrandr
        libva
        libva-drm
        alsa
        libavcodec
        libavformat
        libavutil
        libswscale
    )
    target_link_libraries(shine_core PUBLIC
        Threads::Threads
        PkgConfig::SHINE_LINUX_DEPS
    )
endif()

if(WIN32)
    add_executable(shine_daemon WIN32
        src/daemon/daemon_app.cpp
        src/daemon/main.cpp
    )
else()
    add_executable(shine_daemon
        src/daemon/daemon_app.cpp
        src/daemon/main.cpp
    )
endif()

target_include_directories(shine_daemon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(shine_daemon PRIVATE shine_core shine_warnings)
install(TARGETS shine_daemon RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(SHINE_BUILD_UI)
    find_package(Qt6 COMPONENTS Core Gui Qml Quick QuickControls2 Widgets QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.5)

        if(WIN32)
            qt_add_executable(shine_app WIN32
                src/ui/main.cpp
                src/ui/recorder_controller.cpp
                src/ui/recorder_controller.hpp
                src/ui/display_model.cpp
                src/ui/display_model.hpp
                src/ui/clip_model.cpp
                src/ui/clip_model.hpp
                packaging/windows/shine.rc
            )
        else()
            qt_add_executable(shine_app
                src/ui/main.cpp
                src/ui/recorder_controller.cpp
                src/ui/recorder_controller.hpp
                src/ui/display_model.cpp
                src/ui/display_model.hpp
                src/ui/clip_model.cpp
                src/ui/clip_model.hpp
            )
        endif()

        set_target_properties(shine_app PROPERTIES OUTPUT_NAME "shine")

        set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
        set_source_files_properties(qml/components/RecordButton.qml PROPERTIES QT_RESOURCE_ALIAS RecordButton.qml)
        set_source_files_properties(qml/components/StatusBadge.qml PROPERTIES QT_RESOURCE_ALIAS StatusBadge.qml)
        set_source_files_properties(qml/components/DisplaySelector.qml PROPERTIES QT_RESOURCE_ALIAS DisplaySelector.qml)
        set_source_files_properties(qml/components/HotkeyRecorder.qml PROPERTIES QT_RESOURCE_ALIAS HotkeyRecorder.qml)
        set_source_files_properties(qml/components/FirstRunWizard.qml PROPERTIES QT_RESOURCE_ALIAS FirstRunWizard.qml)
        set_source_files_properties(qml/components/ClipsPanel.qml PROPERTIES QT_RESOURCE_ALIAS ClipsPanel.qml)

        qt_add_qml_module(shine_app
            URI Shine
            VERSION 1.0
            QML_FILES
                qml/Main.qml
                qml/components/RecordButton.qml
                qml/components/StatusBadge.qml
                qml/components/DisplaySelector.qml
                qml/components/HotkeyRecorder.qml
                qml/components/FirstRunWizard.qml
                qml/components/ClipsPanel.qml
        )

        target_include_directories(shine_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(shine_app PRIVATE
            shine_core
            shine_warnings
            Qt6::Core
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
            Qt6::QuickControls2
            Qt6::Widgets
        )

        install(TARGETS shine_app
            BUNDLE DESTINATION .
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )

        if(WIN32)
            qt_generate_deploy_qml_app_script(
                TARGET shine_app
                OUTPUT_SCRIPT shine_deploy_script
                NO_UNSUPPORTED_PLATFORM_ERROR
                DEPLOY_USER_QML_MODULES_ON_UNSUPPORTED_PLATFORM
            )
            install(SCRIPT "${shine_deploy_script}")
        endif()
    else()
        if(SHINE_REQUIRE_UI)
            message(FATAL_ERROR "Qt6 (Core Gui Qml Quick QuickControls2 Widgets) is required because SHINE_REQUIRE_UI=ON")
        else()
            message(STATUS "Qt6 not found locally; skipping shine_app target (shine_core, shine_daemon, and tests will still be built).")
        endif()
    endif()
endif()

if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
    include(InstallRequiredSystemLibraries)
    install(FILES LICENSE DESTINATION ".")
elseif(UNIX AND NOT APPLE)
    set(SHINE_DAEMON_PATH "${CMAKE_INSTALL_FULL_BINDIR}/shine_daemon")
    configure_file(
        packaging/linux/shine-daemon.service.in
        "${CMAKE_CURRENT_BINARY_DIR}/shine-daemon.service"
        @ONLY
    )
    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/shine-daemon.service"
        DESTINATION "lib/systemd/user"
    )
    install(
        FILES packaging/linux/shine.desktop
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications"
    )
    install(
        FILES packaging/linux/shine.svg
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
    )
    install(
        FILES LICENSE
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/shine"
    )
endif()

if(SHINE_BUILD_TESTS)
    enable_testing()

    add_executable(shine_test_core tests/test_core.cpp)
    target_link_libraries(shine_test_core PRIVATE shine_core shine_warnings)
    add_test(NAME shine_test_core COMMAND shine_test_core)

    add_executable(shine_test_recording tests/test_recording.cpp)
    target_link_libraries(shine_test_recording PRIVATE shine_core shine_warnings)
    add_test(NAME shine_test_recording COMMAND shine_test_recording)

    add_executable(shine_test_replay tests/test_replay.cpp)
    target_link_libraries(shine_test_replay PRIVATE shine_core shine_warnings)
    add_test(NAME shine_test_replay COMMAND shine_test_replay)

    add_executable(shine_test_daemon_ipc tests/test_daemon_ipc.cpp)
    target_link_libraries(shine_test_daemon_ipc PRIVATE shine_core shine_warnings)
    target_compile_definitions(shine_test_daemon_ipc PRIVATE
        SHINE_DAEMON_PATH="$<TARGET_FILE:shine_daemon>"
    )
    add_test(NAME shine_test_daemon_ipc COMMAND shine_test_daemon_ipc)
endif()

set(CPACK_PACKAGE_NAME "Shine")
set(CPACK_PACKAGE_VENDOR "Shine")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Minimalist GPU-accelerated offline screen recorder")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Shine")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_PACKAGE_EXECUTABLES "shine" "Shine")
    set(CPACK_CREATE_DESKTOP_LINKS "shine")
    set(CPACK_NSIS_DISPLAY_NAME "Shine")
    set(CPACK_NSIS_PACKAGE_NAME "Shine")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\shine.exe")

    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/shine.ico" SHINE_ICO_NATIVE)
    string(REPLACE "\\" "\\\\" SHINE_ICO_NSIS "${SHINE_ICO_NATIVE}")
    set(CPACK_NSIS_MUI_ICON "${SHINE_ICO_NSIS}")
    set(CPACK_NSIS_MUI_UNIICON "${SHINE_ICO_NSIS}")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "ExecWait '\\\"$INSTDIR\\\\bin\\\\shine_daemon.exe\\\" --uninstall-autostart'")
elseif(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_NAME "shine")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Shine Contributors <shine@localhost>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-window")
endif()

include(CPack)

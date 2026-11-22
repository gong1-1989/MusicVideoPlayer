cmake_minimum_required(VERSION 3.21)

project(MusicVideoPlayer VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Multimedia
    MultimediaWidgets
    Test
)

qt_standard_project_setup()
# BUILD_TESTING 是 CMake/CTest 标准开关：发布构建可以 -DBUILD_TESTING=OFF
# 直接不生成 mvp_* 测试程序，不需要额外的打包脚本。
include(CTest)

function(mvp_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# ---------------------------------------------------------------- 核心模块
# 播放/播放列表/网络/歌词/下载全部沉到 mvp_core，界面层只做装配。
qt_add_library(mvp_core STATIC
    src/media/MediaItem.h
    src/util/HeaderPolicy.h
    src/util/PlaylistSniffer.h
    src/util/PlaylistSniffer.cpp
    src/util/SecretStore.h
    src/util/SecretStore.cpp
    src/util/TimeFormat.h
    src/util/TimeFormat.cpp
    src/net/HttpClient.h
    src/net/HttpClient.cpp
    src/net/NetLimits.h
    src/net/SourceConfig.h
    src/net/SourceConfig.cpp
    src/net/ConfigMapper.h
    src/net/ConfigMapper.cpp
    src/net/SearchProviderBase.h
    src/net/ConfigSearchProvider.h
    src/net/ConfigSearchProvider.cpp
    src/net/MaccmsWebProvider.h
    src/net/MaccmsWebProvider.cpp
    src/net/SourceProbe.h
    src/net/SourceProbe.cpp
    src/net/ExternalSearchProvider.h
    src/net/ExternalSearchProvider.cpp
    src/net/SourceRegistry.h
    src/net/SourceRegistry.cpp
    src/net/SearchCoordinator.h
    src/net/SearchCoordinator.cpp
    src/lyrics/LrcParser.h
    src/lyrics/LrcParser.cpp
    src/lyrics/LyricsService.h
    src/lyrics/LyricsService.cpp
    src/download/DownloadTask.h
    src/download/DownloadTask.cpp
    src/download/DownloadJob.h
    src/download/DownloadJob.cpp
    src/download/DownloadPaths.h
    src/download/DownloadPaths.cpp
    src/download/DownloadManager.h
    src/download/DownloadManager.cpp
    src/download/MediaRemux.h
    src/download/MediaRemux.cpp
    src/stream/StreamProxy.h
    src/stream/StreamProxy.cpp
    src/stream/StreamCacheStore.h
    src/stream/StreamCacheStore.cpp
    src/stream/StreamRange.h
    src/stream/StreamRange.cpp
    src/stream/HlsPlaylist.h
    src/stream/HlsPlaylist.cpp
    src/stream/StreamTokenRegistry.h
    src/stream/StreamTokenRegistry.cpp
    src/core/PlaylistModel.h
    src/core/PlaylistModel.cpp
    src/core/PlaylistStore.h
    src/core/PlaylistStore.cpp
    src/core/PlaybackController.h
    src/core/PlaybackController.cpp
    src/settings/WindowPreset.h
    src/settings/AppSettings.h
    src/settings/AppSettings.cpp
    src/storage/StoragePaths.h
    src/storage/StoragePaths.cpp
)

target_include_directories(mvp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mvp_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Multimedia
)
if(WIN32)
    # 源配置凭据使用 DPAPI 加密，避免 sources.json 出现明文 Cookie
    target_link_libraries(mvp_core PUBLIC crypt32)
endif()
mvp_enable_warnings(mvp_core)

# ---------------------------------------------------------------- 应用入口
qt_add_executable(MusicVideoPlayer WIN32
    src/app/main.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.ui
    src/ui/SearchPanelController.h
    src/ui/SearchPanelController.cpp
    src/ui/SearchWindow.h
    src/ui/SearchWindow.cpp
    src/ui/SearchWindow.ui
    src/ui/PlaylistController.h
    src/ui/PlaylistController.cpp
    src/ui/FullscreenController.h
    src/ui/FullscreenController.cpp
    src/ui/PanelOverlayController.h
    src/ui/PanelOverlayController.cpp
    src/ui/LyricsController.h
    src/ui/LyricsController.cpp
    src/ui/MarqueeDelegate.h
    src/ui/MarqueeDelegate.cpp
    src/ui/DownloadController.h
    src/ui/DownloadController.cpp
    src/ui/DownloadManagerDialog.h
    src/ui/DownloadManagerDialog.cpp
    src/ui/DownloadManagerDialog.ui
    src/ui/PlaybackUiController.h
    src/ui/PlaybackUiController.cpp
    src/ui/SeekSlider.h
    src/ui/SeekSlider.cpp
    src/ui/ListMetrics.h
    src/ui/ListMetrics.cpp
    src/ui/UiIcons.h
    src/ui/UiIcons.cpp
    src/ui/SettingsController.h
    src/ui/SettingsController.cpp
    src/ui/ThemeManager.h
    src/ui/ThemeManager.cpp
    src/ui/VideoSurface.h
    src/ui/VideoSurface.cpp
    src/ui/MusicVisualWidget.h
    src/ui/MusicVisualWidget.cpp
    src/ui/VideoStage.h
    src/ui/VideoStage.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.ui
    src/ui/SourceManagerDialog.h
    src/ui/SourceManagerDialog.cpp
    src/ui/SourceManagerDialog.ui
    src/ui/SourceProbeDialog.h
    src/ui/SourceProbeDialog.cpp
    src/ui/SourceProbeDialog.ui
    src/ui/WindowChrome.h
    src/ui/WindowChrome.cpp
    )

target_link_libraries(MusicVideoPlayer PRIVATE
    mvp_core
    Qt6::MultimediaWidgets
)
mvp_enable_warnings(MusicVideoPlayer)

# ---------------------------------------------------------------- 回归测试
# 只锁纯逻辑/序列化/解析；涉及网络、GUI、进程的路径放在下面的集成测试。
if(BUILD_TESTING)
add_executable(mvp_tests tests/test_core_logic.cpp)
target_link_libraries(mvp_tests PRIVATE
    mvp_core
    Qt6::Test
)
mvp_enable_warnings(mvp_tests)
add_test(NAME mvp_core_logic COMMAND mvp_tests)

# 跨模块集成：真实 QNetworkAccessManager + 本地 QTcpServer，验证重定向头、
# 下载失败/超限、搜索超时中止、响应上限和日志脱敏。
add_executable(mvp_integration tests/test_integration.cpp)
target_link_libraries(mvp_integration PRIVATE
    mvp_core
    Qt6::Test
)
mvp_enable_warnings(mvp_integration)
add_test(NAME mvp_integration COMMAND mvp_integration)
set_tests_properties(mvp_integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 存储层：真实文件读写、损坏备份、迁移/种子行为，全部隔离在临时 Data 目录
add_executable(mvp_storage_tests tests/test_storage.cpp)
target_link_libraries(mvp_storage_tests PRIVATE
    mvp_core
    Qt6::Test
)
mvp_enable_warnings(mvp_storage_tests)
add_test(NAME mvp_storage COMMAND mvp_storage_tests)

# 歌词控制器：真实 QWidget 环境，验证切歌后旧请求不会再回填歌词面板
add_executable(mvp_ui_tests
    tests/test_ui_logic.cpp
    src/ui/LyricsController.cpp
    src/ui/PlaylistController.cpp
    src/ui/SeekSlider.cpp
    src/ui/ListMetrics.cpp
    src/ui/SettingsController.cpp
    # SettingsController::openDialog() 仍引用设置/源管理对话框；测试只验证音量逻辑，
    # 但链接期必须提供这些实现，否则整个 SettingsController 对象无法落地。
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.ui
    src/ui/SourceManagerDialog.cpp
    src/ui/SourceManagerDialog.ui
    src/ui/SourceProbeDialog.cpp
    src/ui/SourceProbeDialog.ui
    src/ui/ThemeManager.cpp
    src/ui/WindowChrome.cpp
)
target_link_libraries(mvp_ui_tests PRIVATE
    mvp_core
    Qt6::Test
    Qt6::Widgets
)
mvp_enable_warnings(mvp_ui_tests)
add_test(NAME mvp_ui_logic COMMAND mvp_ui_tests)
set_tests_properties(mvp_ui_logic PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif() # BUILD_TESTING

# 主题样式表嵌入资源，ThemeManager 从 :/themes/<id>.qss 加载
qt_add_resources(MusicVideoPlayer "app_themes"
    PREFIX "/themes"
    BASE "resources/themes"
    FILES resources/themes/dark.qss resources/themes/light.qss
)

set_target_properties(MusicVideoPlayer PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

# ---------------------------------------------------------------- 内置 FFmpeg
# HLS 下载完成后无损转成 MP4 需要 ffmpeg.exe。发布包不依赖用户自行安装：
# 源仓库只保存 LGPL shared 压缩包，配置时解压到 build 缓存，构建后复制到程序目录
# ffmpeg/，安装时随发布包一起带入。GitHub 单文件 100MB 限制也由压缩包规避。
if(WIN32)
    set(MVP_FFMPEG_ARCHIVE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip")
    set(MVP_FFMPEG_CACHE "${CMAKE_BINARY_DIR}/ffmpeg_pkg")
    set(MVP_FFMPEG_ROOT
        "${MVP_FFMPEG_CACHE}/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1")

    if(EXISTS "${MVP_FFMPEG_ARCHIVE}")
        if(NOT EXISTS "${MVP_FFMPEG_ROOT}/bin/ffmpeg.exe")
            message(STATUS "解压内置 FFmpeg 到 ${MVP_FFMPEG_CACHE}")
            file(ARCHIVE_EXTRACT
                INPUT "${MVP_FFMPEG_ARCHIVE}"
                DESTINATION "${MVP_FFMPEG_CACHE}")
        endif()

        if(EXISTS "${MVP_FFMPEG_ROOT}/bin/ffmpeg.exe")
            file(GLOB MVP_FFMPEG_RUNTIME
                "${MVP_FFMPEG_ROOT}/bin/ffmpeg.exe"
                "${MVP_FFMPEG_ROOT}/bin/*.dll")
            add_custom_command(TARGET MusicVideoPlayer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:MusicVideoPlayer>/ffmpeg"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${MVP_FFMPEG_RUNTIME}
                        "$<TARGET_FILE_DIR:MusicVideoPlayer>/ffmpeg"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${MVP_FFMPEG_ROOT}/LICENSE.txt"
                        "$<TARGET_FILE_DIR:MusicVideoPlayer>/ffmpeg/LICENSE.txt"
                COMMENT "复制内置 FFmpeg 到程序目录 ffmpeg/")

            # 安装时 ffmpeg 与主程序同目录层级，MediaRemux 按程序目录/ffmpeg 查找。
            install(DIRECTORY "${MVP_FFMPEG_ROOT}/bin/"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/ffmpeg"
                FILES_MATCHING PATTERN "ffmpeg.exe" PATTERN "*.dll")
            install(FILES "${MVP_FFMPEG_ROOT}/LICENSE.txt"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/ffmpeg")
        else()
            message(WARNING "内置 FFmpeg 压缩包存在但解压后未找到 bin/ffmpeg.exe")
        endif()
    else()
        message(WARNING
            "未找到 ${MVP_FFMPEG_ARCHIVE}，HLS 下载将无法自动转 MP4")
    endif()
endif()

# ---------------------------------------------------------------- 安装与部署
# 默认不安装回归测试程序。确实需要在开发机上一起安装时，
# 配置 -DMVP_INSTALL_TESTS=ON；发布构建默认保持 OFF。
option(MVP_INSTALL_TESTS "把 mvp_* 回归测试程序一并安装到发布目录（仅开发调试）" OFF)

include(GNUInstallDirs)
install(TARGETS MusicVideoPlayer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(MVP_INSTALL_TESTS AND BUILD_TESTING)
    install(TARGETS mvp_tests mvp_integration mvp_storage_tests mvp_ui_tests
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

qt_generate_deploy_app_script(
    TARGET MusicVideoPlayer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

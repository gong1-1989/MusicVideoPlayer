cmake_minimum_required(VERSION 3.21)

project(MusicVideoPlayer VERSION 0.2 LANGUAGES CXX)

# Windows 资源脚本负责 exe 图标与文件属性；需要显式启用 RC 语言，
# 否则部分生成器不会把 .rc 编进可执行文件。
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 运行时代码直接使用 Qt 6.8 才提供的 QAudioBufferOutput，最低版本必须与
# 实际依赖一致，否则 Qt 6.5~6.7 会在编译 AudioSpectrumAnalyzer 时失败。
find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Multimedia
    MultimediaWidgets
)

qt_standard_project_setup()
# BUILD_TESTING 是 CMake/CTest 标准开关：发布构建可以 -DBUILD_TESTING=OFF
# 直接不生成 mvp_* 测试程序，不需要额外的打包脚本。
include(CTest)
# Qt Test 只在生成测试目标时才需要；发布构建关闭 BUILD_TESTING 后不再强制
# 安装 Qt Test 组件，避免无谓的构建环境要求。
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
endif()

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
    src/audio/SpectrumMath.h
    src/audio/SpectrumMath.cpp
    src/audio/AudioSpectrumAnalyzer.h
    src/audio/AudioSpectrumAnalyzer.cpp
    src/util/Aes128Cbc.h
    src/util/Aes128Cbc.cpp
    src/util/HeaderPolicy.h
    src/util/NetworkAddressPolicy.h
    src/util/NetworkAddressPolicy.cpp
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
    src/cache/MediaCache.h
    src/cache/MediaCache.cpp
    src/cache/LyricsCache.h
    src/cache/LyricsCache.cpp
    src/cache/HlsCacheBuilder.h
    src/cache/HlsCacheBuilder.cpp
    src/cache/HlsCacheManifest.h
    src/cache/HlsCacheManifest.cpp
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
    src/settings/AppShortcuts.h
    src/settings/AppShortcuts.cpp
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
    # 源配置凭据使用 DPAPI 加密，避免 sources.json 出现明文 Cookie；
    # HLS 的 AES-128 分片解密走系统 CNG，必须显式链接 bcrypt。
    target_link_libraries(mvp_core PUBLIC crypt32 bcrypt)
endif()
mvp_enable_warnings(mvp_core)

# ---------------------------------------------------------------- 应用入口
# 应用图标和版本信息由 .rc 编进 exe；图标路径用绝对路径生成，
# 避免 windres 在 build 目录下解析相对路径失败。
set(MVP_APP_RESOURCE "")
if(WIN32)
    configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    set(MVP_APP_RESOURCE ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()
qt_add_executable(MusicVideoPlayer WIN32
    ${MVP_APP_RESOURCE}
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
    src/ui/ShortcutSettingsDialog.h
    src/ui/ShortcutSettingsDialog.cpp
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

# ---------------------------------------------------------------- 外部解析器
# 原有 SourceProbe 探测规则移到独立进程，主程序只通过 QProcess 消费 JSON；
# 用户也可以把“外部解析器”配置成自己的脚本或 exe。
qt_add_executable(source-probe-cli
    tools/source_probe_cli/main.cpp
)
target_link_libraries(source-probe-cli PRIVATE
    mvp_core
    Qt6::Core
    Qt6::Network
)
mvp_enable_warnings(source-probe-cli)

# ---------------------------------------------------------------- 回归测试
# 只锁纯逻辑/序列化/解析；涉及网络、GUI、进程的路径放在下面的集成测试。
if(BUILD_TESTING)
add_executable(mvp_tests tests/test_core_logic.cpp)
target_link_libraries(mvp_tests PRIVATE
    mvp_core
    Qt6::Test
)
mvp_enable_warnings(mvp_tests)
# 核心测试里的 HLS 预取用例会故意制造不完整分片，Qt Test 达到默认
# 警告上限后会中止后续断言；放宽上限，保证 260 条 LRU 容量断言完整执行。
add_test(NAME mvp_core_logic COMMAND mvp_tests -maxwarnings 100000)

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
    src/ui/ShortcutSettingsDialog.cpp
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

# 运行时窗口图标与启动页共用同一份 PNG，避免各处再画一个近似图标
qt_add_resources(MusicVideoPlayer "app_icons"
    PREFIX "/icons"
    BASE "resources/icons"
    FILES resources/icons/app-icon.png
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
install(TARGETS MusicVideoPlayer source-probe-cli
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

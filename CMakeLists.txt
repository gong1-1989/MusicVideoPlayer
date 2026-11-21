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
    LinguistTools
    Test
)

qt_standard_project_setup()
enable_testing()

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
    src/stream/StreamProxy.h
    src/stream/StreamProxy.cpp
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
    src/ui/PlaybackUiController.h
    src/ui/PlaybackUiController.cpp
    src/ui/SettingsController.h
    src/ui/SettingsController.cpp
    src/ui/ThemeManager.h
    src/ui/ThemeManager.cpp
    src/ui/VideoSurface.h
    src/ui/VideoSurface.cpp
    src/ui/VideoStage.h
    src/ui/VideoStage.cpp
    src/ui/LyricsView.h
    src/ui/LyricsView.cpp
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
# 只锁纯逻辑/序列化/解析；涉及网络、GUI、进程的路径留给后续集成测试。
add_executable(mvp_tests tests/test_core_logic.cpp)
target_link_libraries(mvp_tests PRIVATE
    mvp_core
    Qt6::Test
)
mvp_enable_warnings(mvp_tests)
add_test(NAME mvp_core_logic COMMAND mvp_tests)

# 存储层：真实文件读写、损坏备份、迁移/种子行为，全部隔离在临时 Data 目录
add_executable(mvp_storage_tests tests/test_storage.cpp)
target_link_libraries(mvp_storage_tests PRIVATE
    mvp_core
    Qt6::Test
)
mvp_enable_warnings(mvp_storage_tests)
add_test(NAME mvp_storage COMMAND mvp_storage_tests)

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

# ---------------------------------------------------------------- 翻译
# qt_add_translations 会把 .qm 编译后嵌入资源，main.cpp 的 :/i18n 才能真正加载。
qt_add_translations(MusicVideoPlayer
    TS_FILES translations/MusicVideoPlayer_zh_CN.ts
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
)

# ---------------------------------------------------------------- 安装与部署
include(GNUInstallDirs)
install(TARGETS MusicVideoPlayer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
    TARGET MusicVideoPlayer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

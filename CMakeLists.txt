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
)

qt_standard_project_setup()

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
    src/util/TimeFormat.h
    src/util/TimeFormat.cpp
    src/net/HttpClient.h
    src/net/HttpClient.cpp
    src/net/SourceConfig.h
    src/net/SourceConfig.cpp
    src/net/SearchProviderBase.h
    src/net/ConfigSearchProvider.h
    src/net/ConfigSearchProvider.cpp
    src/net/BilibiliProvider.h
    src/net/BilibiliProvider.cpp
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
)

target_include_directories(mvp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mvp_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
)
mvp_enable_warnings(mvp_core)

# ---------------------------------------------------------------- 应用入口
qt_add_executable(MusicVideoPlayer WIN32
    src/app/main.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.ui
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
    src/ui/SourceManagerDialog.h
    src/ui/SourceManagerDialog.cpp
    )

target_link_libraries(MusicVideoPlayer PRIVATE
    mvp_core
    Qt6::MultimediaWidgets
)
mvp_enable_warnings(MusicVideoPlayer)

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

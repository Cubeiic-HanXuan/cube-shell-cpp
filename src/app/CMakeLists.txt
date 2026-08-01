# Application entry point.

qt_add_executable(cube-shell
    main.cpp
    # 图标资源：cpp 项目自带的 resources/icons/icons.qrc（从 Python 侧拷入，
    # 无 prefix，因此 ":/icons8-xxx.png" 路径与 Python 版一致）。qrc 直接挂在
    # 可执行目标上（AUTORCC），避开静态库里 Q_INIT_RESOURCE 被链接器丢弃的问题。
    ${CMAKE_SOURCE_DIR}/resources/icons/icons.qrc

    # qdarktheme 全局主题资源（由 resources/qdarktheme/export_from_venv.py 从
    # Python 版 venv 的 pyqtdarktheme-fork 导出，与 cube-shell.py::setDarkTheme/
    # setLightTheme 的 load_stylesheet 输出逐字一致，主色 #00A1FF/#E05B00 已在
    # 导出时通过 custom_colors 注入）。qss 内 url 已改写为 :/qdarktheme/svg/*.svg，
    # 与 qrc 的 prefix "/qdarktheme" 严格对应。同样挂在可执行目标而非静态库
    # cube_core。ThemeManager::styleSheet() 在资源缺失时会 qWarning 并回退到内置 QSS。
    ${CMAKE_SOURCE_DIR}/resources/qdarktheme/qdarktheme.qrc
)

target_link_libraries(cube-shell PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    cube_ui
    cube_core
    qtermwidget
)

set_target_properties(cube-shell PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# MSVC：链接时生成 cube-shell.map（与 exe 同目录）。Release 崩溃只有模块偏移可用时，
# 靠这张地址→函数名映射表定位崩溃函数。
if(MSVC)
    target_link_options(cube-shell PRIVATE "/MAP")
endif()

# --- 运行期资源部署（→ bundle Resources / 可执行文件旁 resources/） ---
# 资源来源两处：
#   * cpp/resources/            —— icons/ 与 translations/（非 qtermwidget 资源）
#   * cpp/src/qtermwidget/      —— color-schemes/、kb-layouts/、根级 default.keytab
#     （与 Python 版 qtermwidget/ 包内布局一致）
# 部署后布局保持原样（Resources/color-schemes、Resources/kb-layouts 等），
# 与 src/qtermwidget/tools.cpp、src/ui/LanguageManager.cpp 的运行期探测顺序对应。
set(QTERMWIDGET_RES_SRC ${CMAKE_SOURCE_DIR}/src/qtermwidget)
if(APPLE)
    # 构建后把 resources/ 与 qtermwidget 资源拷进 .app 的 Contents/Resources/。
    add_custom_command(TARGET cube-shell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/resources"
                "$<TARGET_BUNDLE_CONTENT_DIR:cube-shell>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${QTERMWIDGET_RES_SRC}/color-schemes"
                "$<TARGET_BUNDLE_CONTENT_DIR:cube-shell>/Resources/color-schemes"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${QTERMWIDGET_RES_SRC}/kb-layouts"
                "$<TARGET_BUNDLE_CONTENT_DIR:cube-shell>/Resources/kb-layouts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QTERMWIDGET_RES_SRC}/default.keytab"
                "$<TARGET_BUNDLE_CONTENT_DIR:cube-shell>/Resources/default.keytab"
        COMMENT "Deploying cpp/resources + qtermwidget resources into app bundle Resources/"
    )
else()
    # 非 bundle 平台：拷到可执行文件旁的 resources/。
    add_custom_command(TARGET cube-shell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/resources"
                "$<TARGET_FILE_DIR:cube-shell>/resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${QTERMWIDGET_RES_SRC}/color-schemes"
                "$<TARGET_FILE_DIR:cube-shell>/resources/color-schemes"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${QTERMWIDGET_RES_SRC}/kb-layouts"
                "$<TARGET_FILE_DIR:cube-shell>/resources/kb-layouts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QTERMWIDGET_RES_SRC}/default.keytab"
                "$<TARGET_FILE_DIR:cube-shell>/resources/default.keytab"
        COMMENT "Deploying cpp/resources + qtermwidget resources next to the executable"
    )
endif()

cmake_minimum_required(VERSION 3.28)
project(katemarkdownlive VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_INSTALL_RPATH ON)
find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs6)
include(KDECMakeSettings)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Qml Svg Network)
find_package(KF6 REQUIRED COMPONENTS TextEditor CoreAddons XmlGui I18n SyntaxHighlighting)

# Auditable, offline and reproducible: unmodified upstream release with its license.
include(FetchContent)
set(CMARK_TESTS OFF CACHE BOOL "" FORCE)
set(CMARK_SHARED OFF CACHE BOOL "" FORCE)
set(CMARK_STATIC ON CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(cmark
    URL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cmark-gfm-0.29.0.gfm.13.tar.gz"
    URL_HASH SHA256=5abc61798ebd9de5660bc076443c07abad2b8d15dbc11094a3a79644b8ad243a
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(cmark)

add_library(markdown_core STATIC src/markdown.cpp src/mathsyntax.cpp)
target_include_directories(markdown_core PUBLIC src PRIVATE
    ${cmark_SOURCE_DIR}/src ${cmark_BINARY_DIR}/src
    ${cmark_SOURCE_DIR}/extensions ${cmark_BINARY_DIR}/extensions)
target_link_libraries(markdown_core PUBLIC Qt6::Widgets PRIVATE
    libcmark-gfm-extensions_static libcmark-gfm_static)

add_library(markdown_editor STATIC src/liveeditor.cpp src/mathrenderer.cpp src/mediastore.cpp src/renderdocument.cpp)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mathjax/engine.js" mathjax_hash)
if(NOT mathjax_hash STREQUAL "b55101a92b62955da09703a1594080fd00ab48e6799cf6963316a0c7b0d5e474")
    message(FATAL_ERROR "MathJax bundle checksum mismatch; regenerate and review the vendored bundle")
endif()
qt_add_resources(markdown_editor math_assets PREFIX "/markdownlive" FILES third_party/mathjax/engine.js)
target_link_libraries(markdown_editor PUBLIC markdown_core KF6::TextEditor
    KF6::SyntaxHighlighting KF6::XmlGui Qt6::Qml Qt6::Svg Qt6::Network)
target_compile_options(markdown_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(markdown_editor PRIVATE -Wall -Wextra -Wpedantic)

kcoreaddons_add_plugin(katemarkdownlive INSTALL_NAMESPACE "kf6/ktexteditor")
target_sources(katemarkdownlive PRIVATE src/plugin.cpp)
# Recompile the embedding source in the same incremental build as a metadata edit.
set_property(SOURCE src/plugin.cpp APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.json")
target_link_libraries(katemarkdownlive PRIVATE markdown_editor KF6::CoreAddons KF6::I18n)
target_compile_definitions(katemarkdownlive PRIVATE TRANSLATION_DOMAIN="katemarkdownlive")
qt_add_resources(katemarkdownlive ui PREFIX "/markdownlive" FILES ui.rc)
install(FILES LICENSE third_party/COPYING.cmark-gfm
    DESTINATION "${KDE_INSTALL_DATADIR}/katemarkdownlive/licenses")
install(FILES third_party/mathjax/LICENSE
    DESTINATION "${KDE_INSTALL_DATADIR}/katemarkdownlive/licenses/mathjax")

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(markdown_test tests/markdown_test.cpp)
    target_link_libraries(markdown_test PRIVATE markdown_core Qt6::Test)
    add_test(NAME markdown COMMAND markdown_test)
    add_executable(editor_test tests/editor_test.cpp)
    target_link_libraries(editor_test PRIVATE markdown_editor Qt6::Test KF6::CoreAddons)
    target_compile_definitions(editor_test PRIVATE PLUGIN_FILE="$<TARGET_FILE:katemarkdownlive>")
    add_dependencies(editor_test katemarkdownlive)
    add_test(NAME editor COMMAND editor_test)
    add_executable(rendering_test tests/rendering_test.cpp)
    target_link_libraries(rendering_test PRIVATE markdown_editor Qt6::Test)
    add_test(NAME rendering COMMAND rendering_test)
    set_tests_properties(markdown editor rendering PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-state/config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-state/cache;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-state/data")
endif()

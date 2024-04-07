find_package(${QT_PACKAGE_NAME} REQUIRED COMPONENTS Mqtt)
find_package(QCoro${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

add_executable(headless-client MACOSX_BUNDLE
    HeadlessClientApp.h
    LayerRenderer.cpp
    main.cpp
    PaintEngine.cpp
    Palettize.cpp
    SessionController.cpp
    SessionController.h
)

set_property(TARGET headless-client PROPERTY CXX_STANDARD 23)

target_link_libraries(headless-client PRIVATE
    cmake-config
    dpshared
    dpclient
    drawdance
    ${QT_PACKAGE_NAME}::Core
    ${QT_PACKAGE_NAME}::Mqtt
    QCoro${QT_VERSION_MAJOR}::Coro
    QCoro${QT_VERSION_MAJOR}::Core
)

directory_auto_source_groups()

cmake_minimum_required(VERSION 3.16)
project(DeviceManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(devmgmt
    main.cpp
    MainWindow.cpp
    DeviceModel.cpp
    DeviceOps.cpp
    DevicePropertiesDialog.cpp
    DriverDetailsDialog.cpp
    UpdateDriverDialog.cpp
    SysfsScanner.cpp
    DeviceScanner.cpp
)
target_link_libraries(devmgmt PRIVATE Qt6::Widgets)
cmake_minimum_required(VERSION 3.16)
project(DeviceManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(devmgmt
    src/main.cpp
    src/model/DeviceModel.cpp
    src/model/DeviceOps.cpp
    src/scanner/SysfsScanner.cpp
    src/scanner/DeviceScanner.cpp
    src/monitor/UdevMonitor.cpp
    src/ui/MainWindow.cpp
    src/ui/DevicePropertiesDialog.cpp
    src/ui/DriverDetailsDialog.cpp
    src/ui/UpdateDriverDialog.cpp
)

target_include_directories(devmgmt PRIVATE
    src/model
    src/scanner
    src/monitor
    src/ui
)

target_link_libraries(devmgmt PRIVATE Qt6::Widgets)

install(TARGETS devmgmt RUNTIME DESTINATION bin)
install(FILES linux-devmgmt.desktop DESTINATION share/applications)
install(FILES udev/60-linux-devmgmt.rules DESTINATION lib/udev/rules.d)
add_library(spoolease STATIC
    SpoolEaseConfig.cpp
    SpoolEaseConfig.hpp
    SpoolEaseDeviceSlotUi.cpp
    SpoolEaseDeviceSlotUi.hpp
    SpoolEasePrintDialogUi.cpp
    SpoolEasePrintDialogUi.hpp
)

target_include_directories(spoolease
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${Boost_INCLUDE_DIRS}
)

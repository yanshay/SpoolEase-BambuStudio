add_library(spoolease STATIC
    SpoolEaseConfig.cpp
    SpoolEaseConfig.hpp
    SpoolEaseDeviceSlotUi.cpp
    SpoolEaseDeviceSlotUi.hpp
    SpoolEaseInventory.cpp
    SpoolEaseInventory.hpp
    SpoolEasePrintDialogUi.cpp
    SpoolEasePrintDialogUi.hpp
)

target_include_directories(spoolease
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${Boost_INCLUDE_DIRS}
)

target_link_libraries(spoolease
    PRIVATE libcurl OpenSSL::SSL OpenSSL::Crypto
)

if (SLIC3R_STATIC)
    target_compile_definitions(spoolease PRIVATE wxDEBUG_LEVEL=0)
endif()

set(SLIC3R_GUI_SOURCES
    ${SLIC3R_GUI_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseConfigDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseConfigDialog.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseWebPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseWebPage.hpp
    PARENT_SCOPE
)

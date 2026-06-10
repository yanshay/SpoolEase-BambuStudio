add_library(spoolease STATIC
    SpoolEaseConfig.cpp
    SpoolEaseConfig.hpp
    SpoolEaseDeviceSlotUi.cpp
    SpoolEaseDeviceSlotUi.hpp
    SpoolEaseInventory.cpp
    SpoolEaseInventory.hpp
    SpoolEaseLog.cpp
    SpoolEaseLog.hpp
    SpoolEaseStatus.cpp
    SpoolEaseStatus.hpp
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

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseWebPage.cpp
    PROPERTIES COMPILE_DEFINITIONS "SPOOLEASE_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
)

if (WIN32)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources")
elseif (SLIC3R_FHS)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/" DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/BambuStudio")
elseif (CMAKE_MACOSX_BUNDLE)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/" DESTINATION "${CMAKE_INSTALL_PREFIX}/BambuStudio.app/Contents/resources")
else()
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources")
endif()

set(SLIC3R_GUI_SOURCES
    ${SLIC3R_GUI_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseConfigDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseConfigDialog.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseBackup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseBackup.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseCustomFilaments.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseCustomFilaments.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseWebPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SpoolEaseWebPage.hpp
    PARENT_SCOPE
)

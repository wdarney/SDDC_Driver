# SoapySDR Lib

find_package(SoapySDR CONFIG)

if (SoapySDR_FOUND)

    if(APPLE AND SDDC_MACOS_BUNDLE_FRAMEWORKS_DIR)
        # Use Homebrew's headers and CMake helper, but bind the plugin to the
        # single SoapySDR runtime already present in SDR++.app.
        set_target_properties(SoapySDR PROPERTIES
            IMPORTED_LOCATION "${SDDC_MACOS_BUNDLE_FRAMEWORKS_DIR}/libSoapySDR.0.8.dylib"
            IMPORTED_LOCATION_RELEASE "${SDDC_MACOS_BUNDLE_FRAMEWORKS_DIR}/libSoapySDR.0.8.dylib")
    endif()

    include_directories("." "../Core")

    SOAPY_SDR_MODULE_UTIL(
        TARGET SDDCSupport
        SOURCES
        SoapySDDC.hpp
        Registration.cpp
        Settings.cpp
        Streaming.cpp
        LIBRARIES
        SDDC_CORE
    )

    add_dependencies(SDDCSupport SDDC_CORE)

    if(APPLE AND SDDC_MACOS_BUNDLE_FRAMEWORKS_DIR)
        # The installed module lives under Contents/SoapySDR/modules0.8.
        set_target_properties(SDDCSupport PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@loader_path/../../Frameworks")
    endif()

    target_link_libraries(sddc PRIVATE SDDC_CORE)
    set_target_properties(sddc PROPERTIES VERSION ${PROJECT_VERSION})
    set_target_properties(sddc PROPERTIES SOVERSION 1)

endif(SoapySDR_FOUND)

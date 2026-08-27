cmake_minimum_required(VERSION 3.13)

find_package(SoapySDR CONFIG REQUIRED)

add_executable(sddc-soapy-benchmark soapy_rx888_benchmark.cpp)
target_link_libraries(sddc-soapy-benchmark PRIVATE SoapySDR)
target_compile_features(sddc-soapy-benchmark PRIVATE cxx_std_20)

install(TARGETS sddc-soapy-benchmark RUNTIME DESTINATION bin)

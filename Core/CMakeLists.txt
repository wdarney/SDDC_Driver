cmake_minimum_required(VERSION 3.13)

file(GLOB SRC "*.cpp" "radio/*.cpp" "pffft/*.cpp" "fft_mt_r2iq/*.cpp" "dsp/*.cpp")

if(MSVC)
  file(GLOB ARCH_SRC "arch/win32/*.cpp" "arch/win32/CyAPI/*.cpp")
else()
  file(GLOB ARCH_SRC "arch/linux/*.c" "arch/linux/*.cpp")
endif()
list(APPEND SRC ${ARCH_SRC})

if (MSVC)
    # Assume Windows/x86 target ;)
    set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_avx.cpp PROPERTIES COMPILE_FLAGS /arch:AVX)
    set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_avx2.cpp PROPERTIES COMPILE_FLAGS /arch:AVX2)
    set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_avx512.cpp PROPERTIES COMPILE_FLAGS /arch:AVX512)
else()
    # Assume GCC, check target CPU/FPU
  if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    message(STATUS "Compiling for x64")
    set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_avx.cpp PROPERTIES COMPILE_FLAGS -mavx)
    set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_avx2.cpp PROPERTIES COMPILE_FLAGS -mavx2)
    set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_avx512.cpp PROPERTIES COMPILE_FLAGS -mavx512f)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "arm.*")
    # We may have Neon..
    message(STATUS "Compiling for Neon")
    list(FILTER SRC EXCLUDE REGEX "fft_mt_r2iq/fft_mt_r2iq_avx.*")
    list(APPEND SRC fft_mt_r2iq/fft_mt_r2iq_impl_neon.cpp)
    # On Apple Silicon (arm64) clang does not accept '-mfpu='; NEON is enabled by default.
    if(APPLE)
      # Don't pass -mfpu to Apple clang; only enable PFFFT's NEON code path and relax strict-aliasing warnings
      set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_neon.cpp PROPERTIES COMPILE_FLAGS "")
      set_source_files_properties(pffft/pf_mixer.cpp PROPERTIES COMPILE_FLAGS "-D PFFFT_ENABLE_NEON -Wno-strict-aliasing")
    else()
      set_source_files_properties(fft_mt_r2iq/fft_mt_r2iq_impl_neon.cpp PROPERTIES COMPILE_FLAGS -mfpu=neon-vfpv4)
      set_source_files_properties(pffft/pf_mixer.cpp PROPERTIES COMPILE_FLAGS "-D PFFFT_ENABLE_NEON -mfpu=neon-vfpv4 -Wno-strict-aliasing")
    endif()
  else()
    message(FATAL_ERROR "Unable to identify CPU: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif (MSVC)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")
set(CMAKE_CXX_STANDARD 20)

# Generatge firmware
include(bin2h.cmake)
bin2h(SOURCE_FILE "../SDDC_FX3.img" HEADER_FILE "${CMAKE_BINARY_DIR}/firmware.h" VARIABLE_NAME "firmware")
include_directories(${CMAKE_BINARY_DIR})

add_library(SDDC_CORE STATIC ${SRC})
set_target_properties(SDDC_CORE PROPERTIES POSITION_INDEPENDENT_CODE True)
target_compile_definitions(SDDC_CORE PUBLIC _CRT_SECURE_NO_WARNINGS)

if(MSVC AND TARGET LIBFFTW)
  # LIBFFTW supplies both fftw3.h and the import library.  Make the dependency
  # explicit so building only SDDCSupport cannot compile SDDC_CORE before the
  # ExternalProject has downloaded and prepared FFTW.
  add_dependencies(SDDC_CORE LIBFFTW)
endif()

target_include_directories(SDDC_CORE PUBLIC "${LIBFFTW_INCLUDE_DIRS}")
target_link_directories(SDDC_CORE PUBLIC "${LIBFFTW_LIBRARY_DIRS}")
target_link_libraries(SDDC_CORE PUBLIC ${LIBFFTW_LIBRARIES})

if (NOT USE_SIMD_OPTIMIZATIONS)
   target_compile_definitions(SDDC_CORE PRIVATE NO_SIMD_OPTIM)
endif()

if(MSVC)
  # The native Windows backend uses Cypress CyUSB/CyAPI. This preserves the
  # RX888 bootloader/firmware transition without requiring Zadig or WinUSB.
  target_link_libraries(SDDC_CORE PUBLIC Setupapi.lib)
else()
  target_include_directories(SDDC_CORE PUBLIC "${LIBUSB_INCLUDE_DIRS}")
  target_link_directories(SDDC_CORE PUBLIC "${LIBUSB_LIBRARY_DIRS}")
  target_link_libraries(SDDC_CORE PUBLIC ${LIBUSB_LIBRARIES})
endif()

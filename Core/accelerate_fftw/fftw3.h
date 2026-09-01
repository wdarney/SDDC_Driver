#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef float fftwf_complex[2];
typedef struct fftwf_plan_s* fftwf_plan;

#define FFTW_FORWARD (-1)
#define FFTW_BACKWARD (+1)
#define FFTW_MEASURE (0U)
#define FFTW_ESTIMATE (1U << 6)

// Keep this private compatibility layer out of the process-wide FFTW symbol
// namespace. SDR++ loads its bundled FFTW before loading Soapy modules, and
// dyld can otherwise interpose those fftwf_* definitions on calls originating
// in libSDDCSupport.so.
#define fftwf_malloc sddc_accel_fftwf_malloc
#define fftwf_free sddc_accel_fftwf_free
#define fftwf_plan_dft_1d sddc_accel_fftwf_plan_dft_1d
#define fftwf_plan_dft_r2c_1d sddc_accel_fftwf_plan_dft_r2c_1d
#define fftwf_execute sddc_accel_fftwf_execute
#define fftwf_execute_dft sddc_accel_fftwf_execute_dft
#define fftwf_execute_dft_r2c sddc_accel_fftwf_execute_dft_r2c
#define fftwf_destroy_plan sddc_accel_fftwf_destroy_plan
#define fftwf_import_wisdom_from_filename sddc_accel_fftwf_import_wisdom_from_filename
#define fftwf_export_wisdom_to_filename sddc_accel_fftwf_export_wisdom_to_filename
#define fftwf_forget_wisdom sddc_accel_fftwf_forget_wisdom
#define fftwf_cleanup sddc_accel_fftwf_cleanup

void* fftwf_malloc(size_t bytes);
void fftwf_free(void* ptr);

fftwf_plan fftwf_plan_dft_1d(int n, fftwf_complex* in, fftwf_complex* out,
    int sign, unsigned flags);
fftwf_plan fftwf_plan_dft_r2c_1d(int n, float* in, fftwf_complex* out,
    unsigned flags);

void fftwf_execute(const fftwf_plan plan);
void fftwf_execute_dft(const fftwf_plan plan, fftwf_complex* in,
    fftwf_complex* out);
void fftwf_execute_dft_r2c(const fftwf_plan plan, float* in,
    fftwf_complex* out);
void fftwf_destroy_plan(fftwf_plan plan);

int fftwf_import_wisdom_from_filename(const char* filename);
int fftwf_export_wisdom_to_filename(const char* filename);
void fftwf_forget_wisdom(void);
void fftwf_cleanup(void);

#ifdef __cplusplus
}
#endif

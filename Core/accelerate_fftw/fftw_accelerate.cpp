#include "fftw3.h"

#include <Accelerate/Accelerate.h>
#include <cstdlib>

namespace {

enum class TransformKind {
    Complex,
    RealForward
};

}

struct fftwf_plan_s {
    vDSP_DFT_Setup setup = nullptr;
    size_t length = 0;
    TransformKind kind = TransformKind::Complex;
    int sign = FFTW_FORWARD;
    void* plannedInput = nullptr;
    fftwf_complex* plannedOutput = nullptr;
    float* inputReal = nullptr;
    float* inputImag = nullptr;
    float* outputReal = nullptr;
    float* outputImag = nullptr;
};

namespace {

bool allocateScratch(fftwf_plan plan, size_t count)
{
    plan->inputReal = static_cast<float*>(fftwf_malloc(count * sizeof(float)));
    plan->inputImag = static_cast<float*>(fftwf_malloc(count * sizeof(float)));
    plan->outputReal = static_cast<float*>(fftwf_malloc(count * sizeof(float)));
    plan->outputImag = static_cast<float*>(fftwf_malloc(count * sizeof(float)));
    return plan->inputReal && plan->inputImag && plan->outputReal && plan->outputImag;
}

void executeComplex(fftwf_plan plan, fftwf_complex* input, fftwf_complex* output)
{
    const size_t n = plan->length;
    for (size_t i = 0; i < n; ++i)
    {
        plan->inputReal[i] = input[i][0];
        plan->inputImag[i] = input[i][1];
    }

    vDSP_DFT_Execute(plan->setup, plan->inputReal, plan->inputImag,
        plan->outputReal, plan->outputImag);

    for (size_t i = 0; i < n; ++i)
    {
        output[i][0] = plan->outputReal[i];
        output[i][1] = plan->outputImag[i];
    }
}

void executeRealForward(fftwf_plan plan, float* input, fftwf_complex* output)
{
    const size_t n = plan->length;
    const size_t half = n / 2;
    for (size_t i = 0; i < half; ++i)
    {
        plan->inputReal[i] = input[2 * i];
        plan->inputImag[i] = input[2 * i + 1];
    }

    vDSP_DFT_Execute(plan->setup, plan->inputReal, plan->inputImag,
        plan->outputReal, plan->outputImag);

    // vDSP's real-forward transform is scaled by two. FFTW is unscaled and
    // exposes DC and Nyquist as separate complex bins.
    output[0][0] = 0.5f * plan->outputReal[0];
    output[0][1] = 0.0f;
    for (size_t i = 1; i < half; ++i)
    {
        output[i][0] = 0.5f * plan->outputReal[i];
        output[i][1] = 0.5f * plan->outputImag[i];
    }
    output[half][0] = 0.5f * plan->outputImag[0];
    output[half][1] = 0.0f;
}

}

extern "C" {

void* fftwf_malloc(size_t bytes)
{
    void* pointer = nullptr;
    return posix_memalign(&pointer, 16, bytes) == 0 ? pointer : nullptr;
}

void fftwf_free(void* pointer)
{
    free(pointer);
}

fftwf_plan fftwf_plan_dft_1d(int n, fftwf_complex* input,
    fftwf_complex* output, int sign, unsigned)
{
    if (n <= 0 || !input || !output)
        return nullptr;

    fftwf_plan plan = static_cast<fftwf_plan>(calloc(1, sizeof(fftwf_plan_s)));
    if (!plan)
        return nullptr;
    plan->length = static_cast<size_t>(n);
    plan->kind = TransformKind::Complex;
    plan->sign = sign;
    plan->plannedInput = input;
    plan->plannedOutput = output;
    const vDSP_DFT_Direction direction = sign == FFTW_FORWARD ?
        vDSP_DFT_FORWARD : vDSP_DFT_INVERSE;
    plan->setup = vDSP_DFT_zop_CreateSetup(nullptr, n, direction);
    if (!plan->setup || !allocateScratch(plan, plan->length))
    {
        fftwf_destroy_plan(plan);
        return nullptr;
    }
    return plan;
}

fftwf_plan fftwf_plan_dft_r2c_1d(int n, float* input,
    fftwf_complex* output, unsigned)
{
    if (n <= 0 || (n & 1) || !input || !output)
        return nullptr;

    fftwf_plan plan = static_cast<fftwf_plan>(calloc(1, sizeof(fftwf_plan_s)));
    if (!plan)
        return nullptr;
    plan->length = static_cast<size_t>(n);
    plan->kind = TransformKind::RealForward;
    plan->sign = FFTW_FORWARD;
    plan->plannedInput = input;
    plan->plannedOutput = output;
    plan->setup = vDSP_DFT_zrop_CreateSetup(nullptr, n, vDSP_DFT_FORWARD);
    if (!plan->setup || !allocateScratch(plan, plan->length / 2))
    {
        fftwf_destroy_plan(plan);
        return nullptr;
    }
    return plan;
}

void fftwf_execute(const fftwf_plan plan)
{
    if (!plan)
        return;
    if (plan->kind == TransformKind::Complex)
        executeComplex(plan, static_cast<fftwf_complex*>(plan->plannedInput),
            plan->plannedOutput);
    else
        executeRealForward(plan, static_cast<float*>(plan->plannedInput),
            plan->plannedOutput);
}

void fftwf_execute_dft(const fftwf_plan plan, fftwf_complex* input,
    fftwf_complex* output)
{
    if (plan && plan->kind == TransformKind::Complex)
        executeComplex(plan, input, output);
}

void fftwf_execute_dft_r2c(const fftwf_plan plan, float* input,
    fftwf_complex* output)
{
    if (plan && plan->kind == TransformKind::RealForward)
        executeRealForward(plan, input, output);
}

void fftwf_destroy_plan(fftwf_plan plan)
{
    if (!plan)
        return;
    if (plan->setup)
        vDSP_DFT_DestroySetup(plan->setup);
    fftwf_free(plan->inputReal);
    fftwf_free(plan->inputImag);
    fftwf_free(plan->outputReal);
    fftwf_free(plan->outputImag);
    free(plan);
}

int fftwf_import_wisdom_from_filename(const char*) { return 0; }
int fftwf_export_wisdom_to_filename(const char*) { return 1; }
void fftwf_forget_wisdom(void) {}
void fftwf_cleanup(void) {}

}

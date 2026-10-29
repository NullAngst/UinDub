// SPDX-License-Identifier: GPL-3.0-or-later
#include "Simd.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define UD_X86 1
#endif

namespace ud {

namespace {

std::atomic<bool> g_simd{true};

double sinc(double x) {
    if (std::fabs(x) < 1e-12) return 1.0;
    const double px = M_PI * x;
    return std::sin(px) / px;
}

void shiftRowC(const float* src, float* dst, int w, double shift, float fill, bool repeat) {
    const double base = std::floor(shift);
    const int ib = int(base);
    float k[6];
    lanczos3Weights(shift - base, k);
    auto at = [&](int i) -> float {
        if (i < 0) return repeat ? src[0] : fill;
        if (i >= w) return repeat ? src[w - 1] : fill;
        return src[i];
    };
    for (int x = 0; x < w; ++x) {
        const int i = x + ib;
        if (i - 2 >= 0 && i + 3 < w) {
            const float* s = src + i - 2;
            dst[x] = k[0] * s[0] + k[1] * s[1] + k[2] * s[2] + k[3] * s[3] + k[4] * s[4] + k[5] * s[5];
        } else {
            float v = 0;
            for (int t = 0; t < 6; ++t) v += k[t] * at(i - 2 + t);
            dst[x] = v;
        }
    }
}

float dotC(const float* a, const float* b, int n) {
    float s = 0;
    for (int i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

#ifdef UD_X86
__attribute__((target("avx2,fma"))) void shiftRowAvx2(const float* src, float* dst, int w, double shift, float fill, bool repeat) {
    const double base = std::floor(shift);
    const int ib = int(base);
    float k[6];
    lanczos3Weights(shift - base, k);
    // Interior run where all six taps are inside the row.
    const int x0 = std::max(0, 2 - ib);
    const int x1 = std::min(w, w - 3 - ib);  // exclusive
    auto edge = [&](int x) {
        const int i = x + ib;
        float v = 0;
        for (int t = 0; t < 6; ++t) {
            const int j = i - 2 + t;
            const float s = j < 0 ? (repeat ? src[0] : fill) : (j >= w ? (repeat ? src[w - 1] : fill) : src[j]);
            v += k[t] * s;
        }
        dst[x] = v;
    };
    if (x1 <= x0) {
        for (int x = 0; x < w; ++x) edge(x);
        return;
    }
    for (int x = 0; x < x0; ++x) edge(x);
    const __m256 k0 = _mm256_set1_ps(k[0]), k1 = _mm256_set1_ps(k[1]), k2 = _mm256_set1_ps(k[2]);
    const __m256 k3 = _mm256_set1_ps(k[3]), k4 = _mm256_set1_ps(k[4]), k5 = _mm256_set1_ps(k[5]);
    int x = x0;
    for (; x + 8 <= x1; x += 8) {
        const float* s = src + x + ib - 2;
        __m256 v = _mm256_mul_ps(k0, _mm256_loadu_ps(s));
        v = _mm256_fmadd_ps(k1, _mm256_loadu_ps(s + 1), v);
        v = _mm256_fmadd_ps(k2, _mm256_loadu_ps(s + 2), v);
        v = _mm256_fmadd_ps(k3, _mm256_loadu_ps(s + 3), v);
        v = _mm256_fmadd_ps(k4, _mm256_loadu_ps(s + 4), v);
        v = _mm256_fmadd_ps(k5, _mm256_loadu_ps(s + 5), v);
        _mm256_storeu_ps(dst + x, v);
    }
    for (; x < x1; ++x) {
        const float* s = src + x + ib - 2;
        dst[x] = k[0] * s[0] + k[1] * s[1] + k[2] * s[2] + k[3] * s[3] + k[4] * s[4] + k[5] * s[5];
    }
    for (x = std::max(x1, x0); x < w; ++x) edge(x);
}

__attribute__((target("avx2,fma"))) float dotAvx2(const float* a, const float* b, int n) {
    __m256 acc = _mm256_setzero_ps();
    int i = 0;
    for (; i + 8 <= n; i += 8) acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc);
    alignas(32) float t[8];
    _mm256_store_ps(t, acc);
    float s = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
#endif

bool detectAvx2() {
#ifdef UD_X86
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return false;
#endif
}

}  // namespace

bool cpuHasAvx2() {
    static const bool has = detectAvx2();
    return has;
}

void setSimdEnabled(bool on) { g_simd = on; }
bool simdEnabled() { return g_simd && cpuHasAvx2(); }

void lanczos3Weights(double t, float w[6]) {
    double sum = 0, v[6];
    for (int k = 0; k < 6; ++k) {
        const double z = double(k - 2) - t;
        v[k] = std::fabs(z) < 3.0 ? sinc(z) * sinc(z / 3.0) : 0.0;
        sum += v[k];
    }
    for (int k = 0; k < 6; ++k) w[k] = float(v[k] / sum);
}

void shiftRow(const float* src, float* dst, int w, double shift, float fill, bool repeat) {
    if (w <= 0) return;
#ifdef UD_X86
    if (simdEnabled()) return shiftRowAvx2(src, dst, w, shift, fill, repeat);
#endif
    shiftRowC(src, dst, w, shift, fill, repeat);
}

void warpRow(const float* src, float* dst, int w, double off, double slope, double xref, float fill, bool repeat) {
    auto at = [&](int i) -> float {
        if (i < 0) return repeat ? src[0] : fill;
        if (i >= w) return repeat ? src[w - 1] : fill;
        return src[i];
    };
    float k[6];
    for (int x = 0; x < w; ++x) {
        const double u = double(x) + off + (double(x) - xref) * slope;
        const double b = std::floor(u);
        lanczos3Weights(u - b, k);
        const int i = int(b);
        float v = 0;
        for (int t = 0; t < 6; ++t) v += k[t] * at(i - 2 + t);
        dst[x] = v;
    }
}

float dot(const float* a, const float* b, int n) {
#ifdef UD_X86
    if (simdEnabled()) return dotAvx2(a, b, n);
#endif
    return dotC(a, b, n);
}

}  // namespace ud

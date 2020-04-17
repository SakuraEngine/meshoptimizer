// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#include "meshoptimizer.h"

#include <math.h>

// Detect SIMD settings.
#if !defined(MESHOPTIMIZER_NO_SIMD)

#if defined(__SSE2__)
#define SIMD_SSE
#endif

#if !defined(SIMD_SSE) && defined(_MSC_VER) && !defined(__clang__) && (defined(_M_IX86) || defined(_M_X64))
#define SIMD_SSE
#include <intrin.h>
#endif

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#define SIMD_NEON
#endif

#if defined(__wasm_simd128__)
#define SIMD_WASM
#endif

#endif // !MESHOPTIMIZER_NO_SIMD

#ifdef SIMD_SSE
#include <emmintrin.h>
#include <stdint.h>
#endif

#ifdef SIMD_NEON
#if defined(_MSC_VER) && defined(_M_ARM64)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

#ifdef SIMD_WASM
#include <wasm_simd128.h>
#endif

#ifdef SIMD_WASM
#define wasmx_unpacklo_v16x8(a, b) wasm_v16x8_shuffle(a, b, 0, 8, 1, 9, 2, 10, 3, 11)
#define wasmx_unpackhi_v16x8(a, b) wasm_v16x8_shuffle(a, b, 4, 12, 5, 13, 6, 14, 7, 15)
#define wasmx_unziplo_v32x4(a, b) wasm_v32x4_shuffle(a, b, 0, 2, 4, 6)
#define wasmx_unziphi_v32x4(a, b) wasm_v32x4_shuffle(a, b, 1, 3, 5, 7)
#endif

namespace meshopt
{

#if !defined(SIMD_SSE) && !defined(SIMD_NEON) && !defined(SIMD_WASM)
template <typename T>
static void decodeFilterOct(T* data, size_t count)
{
	const float max = float((1 << (sizeof(T) * 8 - 1)) - 1);

	for (size_t i = 0; i < count; ++i)
	{
		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		float x = float(data[i * 4 + 0]);
		float y = float(data[i * 4 + 1]);
		float z = float(data[i * 4 + 2]) - fabsf(x) - fabsf(y);

		// fixup octahedral coordinates for z<0
		float t = (z >= 0.f) ? 0.f : z;

		x += (x >= 0.f) ? t : -t;
		y += (y >= 0.f) ? t : -t;

		// compute normal length & scale
		float l = sqrtf(x * x + y * y + z * z);
		float s = max / l;

		// rounded signed float->int
		int xf = int(x * s + (x >= 0.f ? 0.5f : -0.5f));
		int yf = int(y * s + (y >= 0.f ? 0.5f : -0.5f));
		int zf = int(z * s + (z >= 0.f ? 0.5f : -0.5f));

		data[i * 4 + 0] = T(xf);
		data[i * 4 + 1] = T(yf);
		data[i * 4 + 2] = T(zf);
	}
}

static void decodeFilterQuat(short* data, size_t count)
{
	const float scale = 1.f / sqrtf(2.f);

	for (size_t i = 0; i < count; ++i)
	{
		// recover scale from the high byte of the component
		int sf = data[i * 4 + 3] | 3;
		float ss = scale / float(sf);

		// convert x/y/z to [-1..1] (scaled...)
		float x = float(data[i * 4 + 0]) * ss;
		float y = float(data[i * 4 + 1]) * ss;
		float z = float(data[i * 4 + 2]) * ss;

		// reconstruct w as a square root; we clamp to 0.f to avoid NaN due to precision errors
		float ww = 1.f - x * x - y * y - z * z;
		float w = sqrtf(ww >= 0.f ? ww : 0.f);

		// rounded signed float->int
		int xf = int(x * 32767.f + (x >= 0.f ? 0.5f : -0.5f));
		int yf = int(y * 32767.f + (y >= 0.f ? 0.5f : -0.5f));
		int zf = int(z * 32767.f + (z >= 0.f ? 0.5f : -0.5f));
		int wf = int(w * 32767.f + 0.5f);

		int qc = data[i * 4 + 3] & 3;

		// output order is dictated by input index
		data[i * 4 + ((qc + 1) & 3)] = short(xf);
		data[i * 4 + ((qc + 2) & 3)] = short(yf);
		data[i * 4 + ((qc + 3) & 3)] = short(zf);
		data[i * 4 + ((qc + 0) & 3)] = short(wf);
	}
}

static void decodeFilterExp(unsigned int* data, size_t count)
{
	for (size_t i = 0; i < count; ++i)
	{
		unsigned int v = data[i];

		// decode mantissa and exponent
		int m = int(v << 8) >> 8;
		int e = int(v) >> 24;

		union {
			float f;
			unsigned int ui;
		} u;

		// optimized version of ldexp(float(m), e)
		u.ui = unsigned(e + 127) << 23;
		u.f = u.f * float(m);

		data[i] = u.ui;
	}
}
#endif

#if defined(SIMD_SSE) || defined(SIMD_NEON) || defined(SIMD_WASM)
inline uint64_t rotateleft64(uint64_t v, int x)
{
#if defined(_MSC_VER) && !defined(__clang__)
	return _rotl64(v, x);
#elif defined(__clang__) && __clang_major__ >= 8
	return __builtin_rotateleft64(v, x);
#else
	return (v << (x & 63)) | (v >> ((64 - x) & 63));
#endif
}
#endif

#ifdef SIMD_SSE
static void decodeFilterOctSimd(signed char* data, size_t count)
{
	const __m128 sign = _mm_set1_ps(-0.f);

	for (size_t i = 0; i < count; i += 4)
	{
		__m128i n4 = _mm_loadu_si128(reinterpret_cast<__m128i*>(&data[i * 4]));

		// sign-extends each of x,y in [x y ? ?] with arithmetic shifts
		__m128i xf = _mm_srai_epi32(_mm_slli_epi32(n4, 24), 24);
		__m128i yf = _mm_srai_epi32(_mm_slli_epi32(n4, 16), 24);

		// unpack z; note that z is unsigned so we technically don't need to sign extend it
		__m128i zf = _mm_srai_epi32(_mm_slli_epi32(n4, 8), 24);

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		__m128 x = _mm_cvtepi32_ps(xf);
		__m128 y = _mm_cvtepi32_ps(yf);
		__m128 z = _mm_sub_ps(_mm_cvtepi32_ps(zf), _mm_add_ps(_mm_andnot_ps(sign, x), _mm_andnot_ps(sign, y)));

		// fixup octahedral coordinates for z<0
		__m128 t = _mm_min_ps(z, _mm_setzero_ps());

		x = _mm_add_ps(x, _mm_xor_ps(t, _mm_and_ps(x, sign)));
		y = _mm_add_ps(y, _mm_xor_ps(t, _mm_and_ps(y, sign)));

		// compute normal length & scale
		__m128 ll = _mm_add_ps(_mm_mul_ps(x, x), _mm_add_ps(_mm_mul_ps(y, y), _mm_mul_ps(z, z)));
		__m128 s = _mm_mul_ps(_mm_set1_ps(127.f), _mm_rsqrt_ps(ll));

		// rounded signed float->int
		__m128i xr = _mm_cvtps_epi32(_mm_mul_ps(x, s));
		__m128i yr = _mm_cvtps_epi32(_mm_mul_ps(y, s));
		__m128i zr = _mm_cvtps_epi32(_mm_mul_ps(z, s));

		// combine xr/yr/zr into final value
		__m128i res = _mm_and_si128(n4, _mm_set1_epi32(0xff000000));
		res = _mm_or_si128(res, _mm_and_si128(xr, _mm_set1_epi32(0xff)));
		res = _mm_or_si128(res, _mm_slli_epi32(_mm_and_si128(yr, _mm_set1_epi32(0xff)), 8));
		res = _mm_or_si128(res, _mm_slli_epi32(_mm_and_si128(zr, _mm_set1_epi32(0xff)), 16));

		_mm_storeu_si128(reinterpret_cast<__m128i*>(&data[i * 4]), res);
	}
}

static void decodeFilterOctSimd(short* data, size_t count)
{
	const __m128 sign = _mm_set1_ps(-0.f);

	for (size_t i = 0; i < count; i += 4)
	{
		__m128 n4_0 = _mm_loadu_ps(reinterpret_cast<float*>(&data[(i + 0) * 4]));
		__m128 n4_1 = _mm_loadu_ps(reinterpret_cast<float*>(&data[(i + 2) * 4]));

		// gather both x/y 16-bit pairs in each 32-bit lane
		__m128i n4 = _mm_castps_si128(_mm_shuffle_ps(n4_0, n4_1, _MM_SHUFFLE(2, 0, 2, 0)));

		// sign-extends each of x,y in [x y] with arithmetic shifts
		__m128i xf = _mm_srai_epi32(_mm_slli_epi32(n4, 16), 16);
		__m128i yf = _mm_srai_epi32(n4, 16);

		// unpack z; note that z is unsigned so we don't need to sign extend it
		__m128i z4 = _mm_castps_si128(_mm_shuffle_ps(n4_0, n4_1, _MM_SHUFFLE(3, 1, 3, 1)));
		__m128i zf = _mm_and_si128(z4, _mm_set1_epi32(0x7fff));

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		__m128 x = _mm_cvtepi32_ps(xf);
		__m128 y = _mm_cvtepi32_ps(yf);
		__m128 z = _mm_sub_ps(_mm_cvtepi32_ps(zf), _mm_add_ps(_mm_andnot_ps(sign, x), _mm_andnot_ps(sign, y)));

		// fixup octahedral coordinates for z<0
		__m128 t = _mm_min_ps(z, _mm_setzero_ps());

		x = _mm_add_ps(x, _mm_xor_ps(t, _mm_and_ps(x, sign)));
		y = _mm_add_ps(y, _mm_xor_ps(t, _mm_and_ps(y, sign)));

		// compute normal length & scale
		__m128 ll = _mm_add_ps(_mm_mul_ps(x, x), _mm_add_ps(_mm_mul_ps(y, y), _mm_mul_ps(z, z)));
		__m128 s = _mm_div_ps(_mm_set1_ps(32767.f), _mm_sqrt_ps(ll));

		// rounded signed float->int
		__m128i xr = _mm_cvtps_epi32(_mm_mul_ps(x, s));
		__m128i yr = _mm_cvtps_epi32(_mm_mul_ps(y, s));
		__m128i zr = _mm_cvtps_epi32(_mm_mul_ps(z, s));

		// mix x/z and y/0 to make 16-bit unpack easier
		__m128i xzr = _mm_or_si128(_mm_and_si128(xr, _mm_set1_epi32(0xffff)), _mm_slli_epi32(zr, 16));
		__m128i y0r = _mm_and_si128(yr, _mm_set1_epi32(0xffff));

		// pack x/y/z using 16-bit unpacks; note that this has 0 where we should have .w
		__m128i res_0 = _mm_unpacklo_epi16(xzr, y0r);
		__m128i res_1 = _mm_unpackhi_epi16(xzr, y0r);

		// patch in .w
		res_0 = _mm_or_si128(res_0, _mm_and_si128(_mm_castps_si128(n4_0), _mm_set1_epi64x(0xffff000000000000)));
		res_1 = _mm_or_si128(res_1, _mm_and_si128(_mm_castps_si128(n4_1), _mm_set1_epi64x(0xffff000000000000)));

		_mm_storeu_si128(reinterpret_cast<__m128i*>(&data[(i + 0) * 4]), res_0);
		_mm_storeu_si128(reinterpret_cast<__m128i*>(&data[(i + 2) * 4]), res_1);
	}
}

static void decodeFilterQuatSimd(short* data, size_t count)
{
	const float scale = 1.f / sqrtf(2.f);

	for (size_t i = 0; i < count; i += 4)
	{
		__m128 q4_0 = _mm_loadu_ps(reinterpret_cast<float*>(&data[(i + 0) * 4]));
		__m128 q4_1 = _mm_loadu_ps(reinterpret_cast<float*>(&data[(i + 2) * 4]));

		// gather both x/y 16-bit pairs in each 32-bit lane
		__m128i q4_xy = _mm_castps_si128(_mm_shuffle_ps(q4_0, q4_1, _MM_SHUFFLE(2, 0, 2, 0)));
		__m128i q4_zc = _mm_castps_si128(_mm_shuffle_ps(q4_0, q4_1, _MM_SHUFFLE(3, 1, 3, 1)));

		// sign-extends each of x,y in [x y] with arithmetic shifts
		__m128i xf = _mm_srai_epi32(_mm_slli_epi32(q4_xy, 16), 16);
		__m128i yf = _mm_srai_epi32(q4_xy, 16);
		__m128i zf = _mm_srai_epi32(_mm_slli_epi32(q4_zc, 16), 16);
		__m128i cf = _mm_srai_epi32(q4_zc, 16);

		// get a floating-point scaler using zc with bottom 2 bits set to 1 (which represents 1.f)
		__m128i sf = _mm_or_si128(cf, _mm_set1_epi32(3));
		__m128 ss = _mm_div_ps(_mm_set1_ps(scale), _mm_cvtepi32_ps(sf));

		// convert x/y/z to [-1..1] (scaled...)
		__m128 x = _mm_mul_ps(_mm_cvtepi32_ps(xf), ss);
		__m128 y = _mm_mul_ps(_mm_cvtepi32_ps(yf), ss);
		__m128 z = _mm_mul_ps(_mm_cvtepi32_ps(zf), ss);

		// reconstruct w as a square root; we clamp to 0.f to avoid NaN due to precision errors
		__m128 ww = _mm_sub_ps(_mm_set1_ps(1.f), _mm_add_ps(_mm_mul_ps(x, x), _mm_add_ps(_mm_mul_ps(y, y), _mm_mul_ps(z, z))));
		__m128 w = _mm_sqrt_ps(_mm_max_ps(ww, _mm_setzero_ps()));

		__m128 s = _mm_set1_ps(32767.f);

		// rounded signed float->int
		__m128i xr = _mm_cvtps_epi32(_mm_mul_ps(x, s));
		__m128i yr = _mm_cvtps_epi32(_mm_mul_ps(y, s));
		__m128i zr = _mm_cvtps_epi32(_mm_mul_ps(z, s));
		__m128i wr = _mm_cvtps_epi32(_mm_mul_ps(w, s));

		// mix x/z and w/y to make 16-bit unpack easier
		__m128i xzr = _mm_or_si128(_mm_and_si128(xr, _mm_set1_epi32(0xffff)), _mm_slli_epi32(zr, 16));
		__m128i wyr = _mm_or_si128(_mm_and_si128(wr, _mm_set1_epi32(0xffff)), _mm_slli_epi32(yr, 16));

		// pack x/y/z/w using 16-bit unpacks; we pack wxyz by default (for qc=0)
		__m128i res_0 = _mm_unpacklo_epi16(wyr, xzr);
		__m128i res_1 = _mm_unpackhi_epi16(wyr, xzr);

		// store results to stack so that we can rotate using scalar instructions
		uint64_t res[4];
		_mm_storeu_si128(reinterpret_cast<__m128i*>(&res[0]), res_0);
		_mm_storeu_si128(reinterpret_cast<__m128i*>(&res[2]), res_1);

		// rotate and store
		uint64_t* out = reinterpret_cast<uint64_t*>(&data[i * 4]);

		out[0] = rotateleft64(res[0], data[(i + 0) * 4 + 3] << 4);
		out[1] = rotateleft64(res[1], data[(i + 1) * 4 + 3] << 4);
		out[2] = rotateleft64(res[2], data[(i + 2) * 4 + 3] << 4);
		out[3] = rotateleft64(res[3], data[(i + 3) * 4 + 3] << 4);
	}
}

static void decodeFilterExpSimd(unsigned int* data, size_t count)
{
	for (size_t i = 0; i < count; i += 4)
	{
		__m128i v = _mm_loadu_si128(reinterpret_cast<__m128i*>(&data[i]));

		// decode exponent into 2^x directly
		__m128i ef = _mm_srai_epi32(v, 24);
		__m128i es = _mm_slli_epi32(_mm_add_epi32(ef, _mm_set1_epi32(127)), 23);

		// decode 24-bit mantissa into floating-point value
		__m128i mf = _mm_srai_epi32(_mm_slli_epi32(v, 8), 8);
		__m128 m = _mm_cvtepi32_ps(mf);

		__m128 r = _mm_mul_ps(_mm_castsi128_ps(es), m);

		_mm_storeu_ps(reinterpret_cast<float*>(&data[i]), r);
	}
}
#endif

#if defined(SIMD_NEON) && !defined(__aarch64__)
inline float32x4_t vsqrtq_f32(float32x4_t x)
{
	float32x4_t r = vrsqrteq_f32(x);
	r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(r, x), r)); // refine rsqrt estimate
	return vmulq_f32(r, x);
}

inline float32x4_t vdivq_f32(float32x4_t x, float32x4_t y)
{
	float32x4_t r = vrecpeq_f32(y);
	r = vmulq_f32(r, vrecpsq_f32(y, r)); // refine rcp estimate
	return vmulq_f32(x, r);
}
#endif

#ifdef SIMD_NEON
static void decodeFilterOctSimd(signed char* data, size_t count)
{
	const int32x4_t sign = vdupq_n_s32(0x80000000);

	for (size_t i = 0; i < count; i += 4)
	{
		int32x4_t n4 = vld1q_s32(reinterpret_cast<int32_t*>(&data[i * 4]));

		// sign-extends each of x,y in [x y ? ?] with arithmetic shifts
		int32x4_t xf = vshrq_n_s32(vshlq_n_s32(n4, 24), 24);
		int32x4_t yf = vshrq_n_s32(vshlq_n_s32(n4, 16), 24);

		// unpack z; note that z is unsigned so we technically don't need to sign extend it
		int32x4_t zf = vshrq_n_s32(vshlq_n_s32(n4, 8), 24);

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		float32x4_t x = vcvtq_f32_s32(xf);
		float32x4_t y = vcvtq_f32_s32(yf);
		float32x4_t z = vsubq_f32(vcvtq_f32_s32(zf), vaddq_f32(vabsq_f32(x), vabsq_f32(y)));

		// fixup octahedral coordinates for z<0
		float32x4_t t = vminq_f32(z, vdupq_n_f32(0.f));

		x = vaddq_f32(x, vreinterpretq_f32_s32(veorq_s32(vreinterpretq_s32_f32(t), vandq_s32(vreinterpretq_s32_f32(x), sign))));
		y = vaddq_f32(y, vreinterpretq_f32_s32(veorq_s32(vreinterpretq_s32_f32(t), vandq_s32(vreinterpretq_s32_f32(y), sign))));

		// compute normal length & scale
		float32x4_t ll = vaddq_f32(vmulq_f32(x, x), vaddq_f32(vmulq_f32(y, y), vmulq_f32(z, z)));
		float32x4_t rl = vrsqrteq_f32(ll);
		float32x4_t s = vmulq_f32(vdupq_n_f32(127.f), rl);

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 16 bits so we can omit the subtraction
		const float32x4_t fsnap = vdupq_n_f32(3 << 22);

		int32x4_t xr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(x, s), fsnap));
		int32x4_t yr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(y, s), fsnap));
		int32x4_t zr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(z, s), fsnap));

		// combine xr/yr/zr into final value
		int32x4_t res = vandq_s32(n4, vdupq_n_s32(0xff000000));
		res = vorrq_s32(res, vandq_s32(xr, vdupq_n_s32(0xff)));
		res = vorrq_s32(res, vshlq_n_s32(vandq_s32(yr, vdupq_n_s32(0xff)), 8));
		res = vorrq_s32(res, vshlq_n_s32(vandq_s32(zr, vdupq_n_s32(0xff)), 16));

		vst1q_s32(reinterpret_cast<int32_t*>(&data[i * 4]), res);
	}
}

static void decodeFilterOctSimd(short* data, size_t count)
{
	const int32x4_t sign = vdupq_n_s32(0x80000000);

	for (size_t i = 0; i < count; i += 4)
	{
		int32x4_t n4_0 = vld1q_s32(reinterpret_cast<int32_t*>(&data[(i + 0) * 4]));
		int32x4_t n4_1 = vld1q_s32(reinterpret_cast<int32_t*>(&data[(i + 2) * 4]));

		// gather both x/y 16-bit pairs in each 32-bit lane
		int32x4_t n4 = vuzpq_s32(n4_0, n4_1).val[0];

		// sign-extends each of x,y in [x y] with arithmetic shifts
		int32x4_t xf = vshrq_n_s32(vshlq_n_s32(n4, 16), 16);
		int32x4_t yf = vshrq_n_s32(n4, 16);

		// unpack z; note that z is unsigned so we don't need to sign extend it
		int32x4_t z4 = vuzpq_s32(n4_0, n4_1).val[1];
		int32x4_t zf = vandq_s32(z4, vdupq_n_s32(0x7fff));

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		float32x4_t x = vcvtq_f32_s32(xf);
		float32x4_t y = vcvtq_f32_s32(yf);
		float32x4_t z = vsubq_f32(vcvtq_f32_s32(zf), vaddq_f32(vabsq_f32(x), vabsq_f32(y)));

		// fixup octahedral coordinates for z<0
		float32x4_t t = vminq_f32(z, vdupq_n_f32(0.f));

		x = vaddq_f32(x, vreinterpretq_f32_s32(veorq_s32(vreinterpretq_s32_f32(t), vandq_s32(vreinterpretq_s32_f32(x), sign))));
		y = vaddq_f32(y, vreinterpretq_f32_s32(veorq_s32(vreinterpretq_s32_f32(t), vandq_s32(vreinterpretq_s32_f32(y), sign))));

		// compute normal length & scale
		float32x4_t ll = vaddq_f32(vmulq_f32(x, x), vaddq_f32(vmulq_f32(y, y), vmulq_f32(z, z)));
		float32x4_t rl = vrsqrteq_f32(ll);
		rl = vmulq_f32(rl, vrsqrtsq_f32(vmulq_f32(rl, ll), rl)); // refine rsqrt estimate
		float32x4_t s = vmulq_f32(vdupq_n_f32(32767.f), rl);

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 16 bits so we can omit the subtraction
		const float32x4_t fsnap = vdupq_n_f32(3 << 22);

		int32x4_t xr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(x, s), fsnap));
		int32x4_t yr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(y, s), fsnap));
		int32x4_t zr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(z, s), fsnap));

		// mix x/z and y/0 to make 16-bit unpack easier
		int32x4_t xzr = vorrq_s32(vandq_s32(xr, vdupq_n_s32(0xffff)), vshlq_n_s32(zr, 16));
		int32x4_t y0r = vandq_s32(yr, vdupq_n_s32(0xffff));

		// pack x/y/z using 16-bit unpacks; note that this has 0 where we should have .w
		int32x4_t res_0 = vreinterpretq_s32_s16(vzipq_s16(vreinterpretq_s16_s32(xzr), vreinterpretq_s16_s32(y0r)).val[0]);
		int32x4_t res_1 = vreinterpretq_s32_s16(vzipq_s16(vreinterpretq_s16_s32(xzr), vreinterpretq_s16_s32(y0r)).val[1]);

		// patch in .w
		res_0 = vbslq_s32(vreinterpretq_u32_u64(vdupq_n_u64(0xffff000000000000)), n4_0, res_0);
		res_1 = vbslq_s32(vreinterpretq_u32_u64(vdupq_n_u64(0xffff000000000000)), n4_1, res_1);

		vst1q_s32(reinterpret_cast<int32_t*>(&data[(i + 0) * 4]), res_0);
		vst1q_s32(reinterpret_cast<int32_t*>(&data[(i + 2) * 4]), res_1);
	}
}

static void decodeFilterQuatSimd(short* data, size_t count)
{
	const float scale = 1.f / sqrtf(2.f);

	for (size_t i = 0; i < count; i += 4)
	{
		int32x4_t q4_0 = vld1q_s32(reinterpret_cast<int32_t*>(&data[(i + 0) * 4]));
		int32x4_t q4_1 = vld1q_s32(reinterpret_cast<int32_t*>(&data[(i + 2) * 4]));

		// gather both x/y 16-bit pairs in each 32-bit lane
		int32x4_t q4_xy = vuzpq_s32(q4_0, q4_1).val[0];
		int32x4_t q4_zc = vuzpq_s32(q4_0, q4_1).val[1];

		// sign-extends each of x,y in [x y] with arithmetic shifts
		int32x4_t xf = vshrq_n_s32(vshlq_n_s32(q4_xy, 16), 16);
		int32x4_t yf = vshrq_n_s32(q4_xy, 16);
		int32x4_t zf = vshrq_n_s32(vshlq_n_s32(q4_zc, 16), 16);
		int32x4_t cf = vshrq_n_s32(q4_zc, 16);

		// get a floating-point scaler using zc with bottom 2 bits set to 1 (which represents 1.f)
		int32x4_t sf = vorrq_s32(cf, vdupq_n_s32(3));
		float32x4_t ss = vdivq_f32(vdupq_n_f32(scale), vcvtq_f32_s32(sf));

		// convert x/y/z to [-1..1] (scaled...)
		float32x4_t x = vmulq_f32(vcvtq_f32_s32(xf), ss);
		float32x4_t y = vmulq_f32(vcvtq_f32_s32(yf), ss);
		float32x4_t z = vmulq_f32(vcvtq_f32_s32(zf), ss);

		// reconstruct w as a square root; we clamp to 0.f to avoid NaN due to precision errors
		float32x4_t ww = vsubq_f32(vdupq_n_f32(1.f), vaddq_f32(vmulq_f32(x, x), vaddq_f32(vmulq_f32(y, y), vmulq_f32(z, z))));
		float32x4_t w = vsqrtq_f32(vmaxq_f32(ww, vdupq_n_f32(0.f)));

		float32x4_t s = vdupq_n_f32(32767.f);

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 16 bits so we can omit the subtraction
		const float32x4_t fsnap = vdupq_n_f32(3 << 22);

		int32x4_t xr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(x, s), fsnap));
		int32x4_t yr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(y, s), fsnap));
		int32x4_t zr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(z, s), fsnap));
		int32x4_t wr = vreinterpretq_s32_f32(vaddq_f32(vmulq_f32(w, s), fsnap));

		// mix x/z and w/y to make 16-bit unpack easier
		int32x4_t xzr = vorrq_s32(vandq_s32(xr, vdupq_n_s32(0xffff)), vshlq_n_s32(zr, 16));
		int32x4_t wyr = vorrq_s32(vandq_s32(wr, vdupq_n_s32(0xffff)), vshlq_n_s32(yr, 16));

		// pack x/y/z/w using 16-bit unpacks; we pack wxyz by default (for qc=0)
		int32x4_t res_0 = vreinterpretq_s32_s16(vzipq_s16(vreinterpretq_s16_s32(wyr), vreinterpretq_s16_s32(xzr)).val[0]);
		int32x4_t res_1 = vreinterpretq_s32_s16(vzipq_s16(vreinterpretq_s16_s32(wyr), vreinterpretq_s16_s32(xzr)).val[1]);

		// rotate and store
		uint64_t* out = (uint64_t*)&data[i * 4];

		out[0] = rotateleft64(vgetq_lane_u64(vreinterpretq_u64_s32(res_0), 0), vgetq_lane_s32(cf, 0) << 4);
		out[1] = rotateleft64(vgetq_lane_u64(vreinterpretq_u64_s32(res_0), 1), vgetq_lane_s32(cf, 1) << 4);
		out[2] = rotateleft64(vgetq_lane_u64(vreinterpretq_u64_s32(res_1), 0), vgetq_lane_s32(cf, 2) << 4);
		out[3] = rotateleft64(vgetq_lane_u64(vreinterpretq_u64_s32(res_1), 1), vgetq_lane_s32(cf, 3) << 4);
	}
}

static void decodeFilterExpSimd(unsigned int* data, size_t count)
{
	for (size_t i = 0; i < count; i += 4)
	{
		int32x4_t v = vld1q_s32(reinterpret_cast<int32_t*>(&data[i]));

		// decode exponent into 2^x directly
		int32x4_t ef = vshrq_n_s32(v, 24);
		int32x4_t es = vshlq_n_s32(vaddq_s32(ef, vdupq_n_s32(127)), 23);

		// decode 24-bit mantissa into floating-point value
		int32x4_t mf = vshrq_n_s32(vshlq_n_s32(v, 8), 8);
		float32x4_t m = vcvtq_f32_s32(mf);

		float32x4_t r = vmulq_f32(vreinterpretq_f32_s32(es), m);

		vst1q_f32(reinterpret_cast<float*>(&data[i]), r);
	}
}
#endif

#ifdef SIMD_WASM
static void decodeFilterOctSimd(signed char* data, size_t count)
{
	const v128_t sign = wasm_f32x4_splat(-0.f);

	for (size_t i = 0; i < count; i += 4)
	{
		v128_t n4 = wasm_v128_load(&data[i * 4]);

		// sign-extends each of x,y in [x y ? ?] with arithmetic shifts
		v128_t xf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 24), 24);
		v128_t yf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 16), 24);

		// unpack z; note that z is unsigned so we technically don't need to sign extend it
		v128_t zf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 8), 24);

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		v128_t x = wasm_f32x4_convert_i32x4(xf);
		v128_t y = wasm_f32x4_convert_i32x4(yf);
		v128_t z = wasm_f32x4_sub(wasm_f32x4_convert_i32x4(zf), wasm_f32x4_add(wasm_f32x4_abs(x), wasm_f32x4_abs(y)));

		// fixup octahedral coordinates for z<0
		// note: i32x4_min with 0 is equvalent to f32x4_min
		v128_t t = wasm_i32x4_min(z, wasm_i32x4_splat(0));

		x = wasm_f32x4_add(x, wasm_v128_xor(t, wasm_v128_and(x, sign)));
		y = wasm_f32x4_add(y, wasm_v128_xor(t, wasm_v128_and(y, sign)));

		// compute normal length & scale
		v128_t ll = wasm_f32x4_add(wasm_f32x4_mul(x, x), wasm_f32x4_add(wasm_f32x4_mul(y, y), wasm_f32x4_mul(z, z)));
		v128_t s = wasm_f32x4_div(wasm_f32x4_splat(127.f), wasm_f32x4_sqrt(ll));

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 8 bits so we can omit the subtraction
		const v128_t fsnap = wasm_f32x4_splat(3 << 22);

		v128_t xr = wasm_f32x4_add(wasm_f32x4_mul(x, s), fsnap);
		v128_t yr = wasm_f32x4_add(wasm_f32x4_mul(y, s), fsnap);
		v128_t zr = wasm_f32x4_add(wasm_f32x4_mul(z, s), fsnap);

		// combine xr/yr/zr into final value
		v128_t res = wasm_v128_and(n4, wasm_i32x4_splat(0xff000000));
		res = wasm_v128_or(res, wasm_v128_and(xr, wasm_i32x4_splat(0xff)));
		res = wasm_v128_or(res, wasm_i32x4_shl(wasm_v128_and(yr, wasm_i32x4_splat(0xff)), 8));
		res = wasm_v128_or(res, wasm_i32x4_shl(wasm_v128_and(zr, wasm_i32x4_splat(0xff)), 16));

		wasm_v128_store(&data[i * 4], res);
	}
}

static void decodeFilterOctSimd(short* data, size_t count)
{
	const v128_t sign = wasm_f32x4_splat(-0.f);
	volatile v128_t zmask = wasm_i32x4_splat(0x7fff); // TODO: volatile works around LLVM shuffle "optimizations"

	for (size_t i = 0; i < count; i += 4)
	{
		v128_t n4_0 = wasm_v128_load(&data[(i + 0) * 4]);
		v128_t n4_1 = wasm_v128_load(&data[(i + 2) * 4]);

		// gather both x/y 16-bit pairs in each 32-bit lane
		v128_t n4 = wasmx_unziplo_v32x4(n4_0, n4_1);

		// sign-extends each of x,y in [x y] with arithmetic shifts
		v128_t xf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 16), 16);
		v128_t yf = wasm_i32x4_shr(n4, 16);

		// unpack z; note that z is unsigned so we don't need to sign extend it
		v128_t z4 = wasmx_unziphi_v32x4(n4_0, n4_1);
		v128_t zf = wasm_v128_and(z4, zmask);

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		v128_t x = wasm_f32x4_convert_i32x4(xf);
		v128_t y = wasm_f32x4_convert_i32x4(yf);
		v128_t z = wasm_f32x4_sub(wasm_f32x4_convert_i32x4(zf), wasm_f32x4_add(wasm_f32x4_abs(x), wasm_f32x4_abs(y)));

		// fixup octahedral coordinates for z<0
		// note: i32x4_min with 0 is equvalent to f32x4_min
		v128_t t = wasm_i32x4_min(z, wasm_i32x4_splat(0));

		x = wasm_f32x4_add(x, wasm_v128_xor(t, wasm_v128_and(x, sign)));
		y = wasm_f32x4_add(y, wasm_v128_xor(t, wasm_v128_and(y, sign)));

		// compute normal length & scale
		v128_t ll = wasm_f32x4_add(wasm_f32x4_mul(x, x), wasm_f32x4_add(wasm_f32x4_mul(y, y), wasm_f32x4_mul(z, z)));
		v128_t s = wasm_f32x4_div(wasm_f32x4_splat(32767.f), wasm_f32x4_sqrt(ll));

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 16 bits so we can omit the subtraction
		const v128_t fsnap = wasm_f32x4_splat(3 << 22);

		v128_t xr = wasm_f32x4_add(wasm_f32x4_mul(x, s), fsnap);
		v128_t yr = wasm_f32x4_add(wasm_f32x4_mul(y, s), fsnap);
		v128_t zr = wasm_f32x4_add(wasm_f32x4_mul(z, s), fsnap);

		// mix x/z and y/0 to make 16-bit unpack easier
		v128_t xzr = wasm_v128_or(wasm_v128_and(xr, wasm_i32x4_splat(0xffff)), wasm_i32x4_shl(zr, 16));
		v128_t y0r = wasm_v128_and(yr, wasm_i32x4_splat(0xffff));

		// pack x/y/z using 16-bit unpacks; note that this has 0 where we should have .w
		v128_t res_0 = wasmx_unpacklo_v16x8(xzr, y0r);
		v128_t res_1 = wasmx_unpackhi_v16x8(xzr, y0r);

		// patch in .w
		res_0 = wasm_v128_or(res_0, wasm_v128_and(n4_0, wasm_i64x2_splat(0xffff000000000000)));
		res_1 = wasm_v128_or(res_1, wasm_v128_and(n4_1, wasm_i64x2_splat(0xffff000000000000)));

		wasm_v128_store(&data[(i + 0) * 4], res_0);
		wasm_v128_store(&data[(i + 2) * 4], res_1);
	}
}

static void decodeFilterQuatSimd(short* data, size_t count)
{
	const float scale = 1.f / sqrtf(2.f);

	for (size_t i = 0; i < count; i += 4)
	{
		v128_t q4_0 = wasm_v128_load(&data[(i + 0) * 4]);
		v128_t q4_1 = wasm_v128_load(&data[(i + 2) * 4]);

		// gather both x/y 16-bit pairs in each 32-bit lane
		v128_t q4_xy = wasmx_unziplo_v32x4(q4_0, q4_1);
		v128_t q4_zc = wasmx_unziphi_v32x4(q4_0, q4_1);

		// sign-extends each of x,y in [x y] with arithmetic shifts
		v128_t xf = wasm_i32x4_shr(wasm_i32x4_shl(q4_xy, 16), 16);
		v128_t yf = wasm_i32x4_shr(q4_xy, 16);
		v128_t zf = wasm_i32x4_shr(wasm_i32x4_shl(q4_zc, 16), 16);
		v128_t cf = wasm_i32x4_shr(q4_zc, 16);

		// get a floating-point scaler using zc with bottom 2 bits set to 1 (which represents 1.f)
		v128_t sf = wasm_v128_or(cf, wasm_i32x4_splat(3));
		v128_t ss = wasm_f32x4_div(wasm_f32x4_splat(scale), wasm_f32x4_convert_i32x4(sf));

		// convert x/y/z to [-1..1] (scaled...)
		v128_t x = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(xf), ss);
		v128_t y = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(yf), ss);
		v128_t z = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(zf), ss);

		// reconstruct w as a square root; we clamp to 0.f to avoid NaN due to precision errors
		// note: i32x4_max with 0 is equivalent to f32x4_max
		v128_t ww = wasm_f32x4_sub(wasm_f32x4_splat(1.f), wasm_f32x4_add(wasm_f32x4_mul(x, x), wasm_f32x4_add(wasm_f32x4_mul(y, y), wasm_f32x4_mul(z, z))));
		v128_t w = wasm_f32x4_sqrt(wasm_i32x4_max(ww, wasm_i32x4_splat(0)));

		v128_t s = wasm_f32x4_splat(32767.f);

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 16 bits so we can omit the subtraction
		const v128_t fsnap = wasm_f32x4_splat(3 << 22);

		v128_t xr = wasm_f32x4_add(wasm_f32x4_mul(x, s), fsnap);
		v128_t yr = wasm_f32x4_add(wasm_f32x4_mul(y, s), fsnap);
		v128_t zr = wasm_f32x4_add(wasm_f32x4_mul(z, s), fsnap);
		v128_t wr = wasm_f32x4_add(wasm_f32x4_mul(w, s), fsnap);

		// mix x/z and w/y to make 16-bit unpack easier
		v128_t xzr = wasm_v128_or(wasm_v128_and(xr, wasm_i32x4_splat(0xffff)), wasm_i32x4_shl(zr, 16));
		v128_t wyr = wasm_v128_or(wasm_v128_and(wr, wasm_i32x4_splat(0xffff)), wasm_i32x4_shl(yr, 16));

		// pack x/y/z/w using 16-bit unpacks; we pack wxyz by default (for qc=0)
		v128_t res_0 = wasmx_unpacklo_v16x8(wyr, xzr);
		v128_t res_1 = wasmx_unpackhi_v16x8(wyr, xzr);

		// compute component index shifted left by 4 (and moved into i32x4 slot)
		v128_t cm = wasm_i32x4_shl(cf, 4);

		// rotate and store
		uint64_t* out = reinterpret_cast<uint64_t*>(&data[i * 4]);

		out[0] = rotateleft64(wasm_i64x2_extract_lane(res_0, 0), wasm_i32x4_extract_lane(cm, 0));
		out[1] = rotateleft64(wasm_i64x2_extract_lane(res_0, 1), wasm_i32x4_extract_lane(cm, 1));
		out[2] = rotateleft64(wasm_i64x2_extract_lane(res_1, 0), wasm_i32x4_extract_lane(cm, 2));
		out[3] = rotateleft64(wasm_i64x2_extract_lane(res_1, 1), wasm_i32x4_extract_lane(cm, 3));
	}
}

static void decodeFilterExpSimd(unsigned int* data, size_t count)
{
	for (size_t i = 0; i < count; i += 4)
	{
		v128_t v = wasm_v128_load(&data[i]);

		// decode exponent into 2^x directly
		v128_t ef = wasm_i32x4_shr(v, 24);
		v128_t es = wasm_i32x4_shl(wasm_i32x4_add(ef, wasm_i32x4_splat(127)), 23);

		// decode 24-bit mantissa into floating-point value
		v128_t mf = wasm_i32x4_shr(wasm_i32x4_shl(v, 8), 8);
		v128_t m = wasm_f32x4_convert_i32x4(mf);

		v128_t r = wasm_f32x4_mul(es, m);

		wasm_v128_store(&data[i], r);
	}
}
#endif

} // namespace meshopt

void meshopt_decodeFilterOct(void* buffer, size_t vertex_count, size_t vertex_size)
{
	using namespace meshopt;

	assert(vertex_count % 4 == 0);
	assert(vertex_size == 4 || vertex_size == 8);

#if defined(SIMD_SSE) || defined(SIMD_NEON) || defined(SIMD_WASM)
	if (vertex_size == 4)
		decodeFilterOctSimd(static_cast<signed char*>(buffer), vertex_count);
	else
		decodeFilterOctSimd(static_cast<short*>(buffer), vertex_count);
#else
	if (vertex_size == 4)
		decodeFilterOct(static_cast<signed char*>(buffer), vertex_count);
	else
		decodeFilterOct(static_cast<short*>(buffer), vertex_count);
#endif
}

void meshopt_decodeFilterQuat(void* buffer, size_t vertex_count, size_t vertex_size)
{
	using namespace meshopt;

	assert(vertex_count % 4 == 0);
	assert(vertex_size == 8);
	(void)vertex_size;

#if defined(SIMD_SSE) || defined(SIMD_NEON) || defined(SIMD_WASM)
	decodeFilterQuatSimd(static_cast<short*>(buffer), vertex_count);
#else
	decodeFilterQuat(static_cast<short*>(buffer), vertex_count);
#endif
}

void meshopt_decodeFilterExp(void* buffer, size_t vertex_count, size_t vertex_size)
{
	using namespace meshopt;

	assert(vertex_count % 4 == 0);
	assert(vertex_size % 4 == 0);

#if defined(SIMD_SSE) || defined(SIMD_NEON) || defined(SIMD_WASM)
	decodeFilterExpSimd(static_cast<unsigned int*>(buffer), vertex_count * (vertex_size / 4));
#else
	decodeFilterExp(static_cast<unsigned int*>(buffer), vertex_count * (vertex_size / 4));
#endif
}

#undef SIMD_SSE
#undef SIMD_NEON
#undef SIMD_WASM

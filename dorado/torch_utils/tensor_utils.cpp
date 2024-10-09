#include "tensor_utils.h"

#include "utils/simd.h"

#include <torch/csrc/jit/serialization/pickle.h>
#include <torch/torch.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

#if !ENABLE_NEON_IMPL  // We only need the SIMD implementation when we have Neon support.
#if ENABLE_AVX2_IMPL
__attribute__((target("default")))
#endif
void convert_f32_to_f16_impl(c10::Half* const dest, const float* const src, std::size_t count) {
    auto src_tensor_f32 = at::from_blob(const_cast<float*>(src), {static_cast<int64_t>(count)});
    auto src_tensor_f16 = src_tensor_f32.to(at::ScalarType::Half);
    std::memcpy(dest, src_tensor_f16.data_ptr(), count * sizeof(c10::Half));
}
#endif  // ENABLE_NEON_IMPL

#if ENABLE_AVX2_IMPL || ENABLE_NEON_IMPL
#if ENABLE_AVX2_IMPL
// We have to specify f16c to have _mm256_cvtps_ph available, as strictly speaking it's a separate
// feature from AVX2.  All relevant CPUs have it.
__attribute__((target("avx2,f16c")))
#endif
void convert_f32_to_f16_impl(c10::Half* const dest, const float* const src, std::size_t count) {
    if (!count) {
        return;
    }

#if ENABLE_AVX2_IMPL
    // AVX registers have 8 floats.
    static constexpr size_t kFloatsPerRegister = 8;

    // There seems to be no improvement by unrolling this (tested on pipelinedev).
    static constexpr size_t kUnrollFactor = 1;

    // Matches torch behaviour.
    const int kRoundNearestEven = 0;

    using FloatRegister = __m256;
    using HalfRegister = __m128i;

#define simd_load(ptr) _mm256_loadu_ps(ptr)
#define simd_load1(ptr) _mm256_broadcast_ss(ptr)
#define simd_convert(reg) _mm256_cvtps_ph(reg, kRoundNearestEven)
#define simd_store(ptr, reg) _mm_storeu_si128(reinterpret_cast<HalfRegister*>(ptr), reg)
#define simd_store1(ptr, reg)                      \
    *(reinterpret_cast<std::int16_t*>(dest_ptr)) = \
            static_cast<std::int16_t>(_mm_extract_epi16(elem_f16, 0))

#else
    // Neon registers have 4 floats.
    static constexpr size_t kFloatsPerRegister = 4;

    // An unroll factor of 2 gives ~30% improvement on Apple Silicon.
    // Any higher unrolling shows no difference.
    static constexpr size_t kUnrollFactor = 2;

    using FloatRegister = float32x4_t;
    using HalfRegister = float16x4_t;

#define simd_load(ptr) vld1q_f32(ptr)
#define simd_load1(ptr) vdupq_n_f32(*ptr)
#define simd_convert(reg) vcvt_f16_f32(reg)
#define simd_store(ptr, reg) vst1_f16(reinterpret_cast<float16_t*>(ptr), reg)
#define simd_store1(ptr, reg) *ptr = vget_lane_f16(reg, 0)

#endif

    // Outer unroll.
    static constexpr size_t kUnroll = kFloatsPerRegister * kUnrollFactor;

    // Main vectorised loop.
    const auto* src_ptr = src;
    auto* dest_ptr = dest;
    for (size_t chunk_i = 0; chunk_i < count / kUnroll; ++chunk_i) {
        for (size_t unroll_i = 0; unroll_i < kUnrollFactor; ++unroll_i) {
            const FloatRegister elems_f32 = simd_load(src_ptr);
            const HalfRegister elems_f16 = simd_convert(elems_f32);
            simd_store(dest_ptr, elems_f16);
            src_ptr += kFloatsPerRegister;
            dest_ptr += kFloatsPerRegister;
        }
    }

    // Loop for final floats.
    // TODO -- probably nicer to use masked loads/stores.
    const size_t remaining_count = count % kUnroll;
    for (size_t i = 0; i < remaining_count; ++i) {
        const FloatRegister elem_f32 = simd_load1(src_ptr);
        const HalfRegister elem_f16 = simd_convert(elem_f32);
        simd_store1(dest_ptr, elem_f16);
        ++src_ptr;
        ++dest_ptr;
    }
}
#endif  // ENABLE_AVX2_IMPL || ENABLE_NEON_IMPL

}  // namespace

namespace dorado::utils {

void serialise_tensor(const at::Tensor& t, const std::string& path) {
    auto bytes = torch::jit::pickle_save(t);
    std::ofstream fout(path);
    fout.write(bytes.data(), bytes.size());
    fout.close();
}

std::vector<at::Tensor> load_tensors(const std::filesystem::path& dir,
                                     const std::vector<std::string>& tensors) {
    auto weights = std::vector<at::Tensor>();
    for (const auto& tensor : tensors) {
        auto path = dir / tensor;
        torch::load(weights, path.string());
    }

    return weights;
}

at::Tensor quantile(const at::Tensor& t, const at::Tensor& q) {
    assert(q.dtype() == at::ScalarType::Float);

    auto tmp = t.clone();
    auto [qval, qidx] = q.sort();
    auto res = at::empty_like(q);

    auto start = tmp.data_ptr<float>();
    auto end = tmp.data_ptr<float>() + tmp.size(0);

    for (int i = 0; i < q.size(0); i++) {
        auto m = tmp.data_ptr<float>() +
                 static_cast<size_t>((tmp.size(0) - 1) * qval[i].item<float>());
        std::nth_element(start, m, end);
        res[qidx[i]] = *m;
        start = m;
    }

    return res;
}

at::Tensor quantile_counting(const at::Tensor& t, const at::Tensor& q) {
    assert(q.dtype() == at::ScalarType::Float);

    auto p = t.data_ptr<int16_t>();
    auto range_min = t.min().item<int16_t>();
    auto range_max = t.max().item<int16_t>();

    size_t size = t.size(0);

    std::vector<int> counts(range_max - range_min + 1, 0);
    for (size_t i = 0; i < size; ++i) {
        counts[p[i] - range_min]++;
    }
    std::partial_sum(counts.begin(), counts.end(), counts.begin());

    auto res = at::empty_like(q);

    for (size_t idx = 0; idx < size_t(q.numel()); idx++) {
        int threshold = int(q[idx].item<float>() * (size - 1));
        for (int i = 0; i < int(counts.size()); ++i) {
            if (counts[i] > threshold) {
                res[idx] = i + range_min;
                break;
            }
        }
    }

    return res;
}

// Multiversioned function dispatch doesn't work across the dorado_lib linking
// boundary.  Without this wrapper, AVX machines still only execute the default
// version.
void convert_f32_to_f16(c10::Half* const dest, const float* const src, std::size_t count) {
    return convert_f32_to_f16_impl(dest, src, count);
}

void copy_tensor_elems(at::Tensor& dest_tensor,
                       std::size_t dest_offset,
                       const at::Tensor& src_tensor,
                       std::size_t src_offset,
                       std::size_t count) {
    assert(dest_tensor.is_contiguous());
    assert(src_tensor.is_contiguous());
    assert(dest_offset + count <= size_t(dest_tensor.numel()));
    assert(src_offset + count <= size_t(src_tensor.numel()));

    if (dest_tensor.dtype() == src_tensor.dtype()) {
        // No conversion.
        auto* const dest_ptr = reinterpret_cast<std::byte*>(dest_tensor.data_ptr());
        const auto* const src_ptr = reinterpret_cast<std::byte*>(src_tensor.data_ptr());
        const size_t elem_size = dest_tensor.element_size();
        std::memcpy(&dest_ptr[dest_offset * elem_size], &src_ptr[src_offset * elem_size],
                    count * elem_size);
    } else if (dest_tensor.dtype() == at::ScalarType::Half &&
               src_tensor.dtype() == at::ScalarType::Float) {
        // float32 -> float16 conversion.
        auto* const dest_ptr = dest_tensor.data_ptr<c10::Half>();
        const auto* const src_ptr = src_tensor.data_ptr<float>();
        convert_f32_to_f16_impl(&dest_ptr[dest_offset], &src_ptr[src_offset], count);
    } else {
        // Slow fallback path for other conversions.
        using at::indexing::Slice;
        dest_tensor.flatten().index_put_(
                {Slice(dest_offset, dest_offset + count)},
                src_tensor.flatten().index({Slice(src_offset, src_offset + count)}));
    }
}

ScaledTensor quantize_tensor(const at::Tensor& t, int dim) {
    auto fp_range = t.abs().amax(dim);
    constexpr int levels = 256;
    auto quant_scale = ((levels / 2) / fp_range);
    auto quant_max = (levels / 2) - 1;
    auto t_quant = (t * quant_scale.unsqueeze(dim)).round().clip(-quant_max, quant_max);
    return ScaledTensor{t_quant.to(at::ScalarType::Char), quant_scale.to(at::ScalarType::Float)};
}

std::string print_size(const at::Tensor& t, const std::string& name) {
    std::string size = "";
    std::stringstream ss;
    ss << name << " tensor size ";
    for (auto s : t.sizes()) {
        ss << s << ",";
    }
    ss << " dtype " << t.dtype();
    return ss.str();
}

}  // namespace dorado::utils

#pragma once

#include "basecall/CRFModelConfig.h"
#include "basecall/nn/CRFModel.h"
#include "torch_utils/gpu_profiling.h"
#include "torch_utils/module_utils.h"
#include "torch_utils/tensor_utils.h"

#include <ATen/core/TensorBody.h>
#include <c10/core/Device.h>
#include <c10/core/TensorOptions.h>
#include <torch/nn.h>

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dorado::basecall {

namespace nn {

torch::Tensor scaled_dot_product_attention_naive(const torch::Tensor &q,
                                                 const torch::Tensor &k,
                                                 const torch::Tensor &v,
                                                 const torch::Tensor &mask);

struct RMSNormImpl : torch::nn::Module {
    RMSNormImpl(int hidden_size_);
    at::Tensor forward(at::Tensor x);

    at::Tensor weight;
    const int hidden_size;
    const float eps{1e-5f};
};

TORCH_MODULE(RMSNorm);

struct GatedMLPImpl : torch::nn::Module {
    GatedMLPImpl(int in_features, int hidden_features);

    at::Tensor forward(const at::Tensor &x);

    bool features_interleaved = false;
    int in_features;
    int hidden_features;
    torch::nn::Linear fc1{nullptr}, fc2{nullptr};
};

TORCH_MODULE(GatedMLP);

struct RotaryEmbeddingImpl : torch::nn::Module {
    RotaryEmbeddingImpl(int dim_,
                        float theta_,
                        int max_seq_len_,
                        const at::TensorOptions &options_);

    at::Tensor forward(at::Tensor &qkv);
    void assert_forward_dims(const at::Tensor &qkv) const;

    at::Tensor get_inv_freqs() const;

    const int64_t dim, max_seq_len;
    const float theta;
    const at::TensorOptions options;
};

TORCH_MODULE(RotaryEmbedding);

using MaskKey = std::pair<int64_t, torch::Device>;

// Hash function for std::pair<int64_t, int>
struct MaskKeyHash {
    std::size_t operator()(const MaskKey &key) const {
        auto hash1 = std::hash<int64_t>{}(key.first);
        auto hash2 = std::hash<torch::Device>{}(key.second);
        return hash1 ^ (hash2 << 1);
    }
};

struct MultiHeadAttentionImpl : torch::nn::Module {
    MultiHeadAttentionImpl(int d_model_,
                           int nhead_,
                           bool qkv_bias_,
                           bool out_bias_,
                           const std::pair<int, int> &attn_window_,
                           const at::TensorOptions &options_);

    at::Tensor forward(at::Tensor x);

    at::Tensor get_attn_window_mask(const int64_t size);
    at::Tensor build_attn_window_mask(const int64_t size) const;

    const int d_model, nhead, head_dim, num_splits;
    const std::pair<int, int> attn_window;
    const at::TensorOptions options;
    bool wqkv_transposed = false;

    std::unordered_map<MaskKey, at::Tensor, MaskKeyHash> mask_cache{};

    torch::nn::Linear wqkv{nullptr}, out_proj{nullptr};
    RotaryEmbedding rotary_emb{nullptr};
};

TORCH_MODULE(MultiHeadAttention);

struct TxEncoderImpl : torch::nn::Module {
    TxEncoderImpl(const tx::TxEncoderParams &params, const at::TensorOptions &options);

    at::Tensor forward(at::Tensor x);

    void koi_forward(utils::ScaledTensor &scaled_tensor, at::Tensor &x_f16);

    tx::TxEncoderParams params;

    // Rearranged weights for Koi tiled codepath
    utils::ScaledTensor wqkv_weights_i8, wqkv_weights_f16, t_fc1_wts_i8, t_fc1_wts_f16;
    at::Tensor sincos_bfr, proj_weight, proj_bias, t_res_weights, t_res2_weights, t_fc2_wts;

    MultiHeadAttention self_attn{nullptr};
    GatedMLP ff{nullptr};
    RMSNorm norm1{nullptr}, norm2{nullptr};
};

TORCH_MODULE(TxEncoder);

struct TxEncoderStackImpl : torch::nn::Module {
    TxEncoderStackImpl(const tx::TxEncoderParams &params, const at::TensorOptions &options);

    at::Tensor forward(const at::Tensor &x);

    bool use_koi_tiled{false};
    bool use_i8{false};
    torch::nn::Sequential stack{nullptr};
    std::vector<TxEncoder> layer_vec;
};

TORCH_MODULE(TxEncoderStack);

struct LinearUpsampleImpl : torch::nn::Module {
    LinearUpsampleImpl(const tx::EncoderUpsampleParams &params);

    at::Tensor forward(const at::Tensor &x);

    const int scale_factor;
    torch::nn::Linear linear{nullptr};
};

TORCH_MODULE(LinearUpsample);

struct LinearScaledCRFImpl : torch::nn::Module {
    LinearScaledCRFImpl(const tx::CRFEncoderParams &params);

    at::Tensor forward(const at::Tensor &x);

    bool scale_applied = false;
    torch::nn::Linear linear{nullptr};
    tx::CRFEncoderParams m_params;
};

TORCH_MODULE(LinearScaledCRF);

struct TxModelImpl : torch::nn::Module {
    explicit TxModelImpl(const basecall::CRFModelConfig &config, const at::TensorOptions &options);

    void load_state_dict(const std::vector<at::Tensor> &weights) {
        utils::load_state_dict(*this, weights);
    }

    at::Tensor forward(const at::Tensor &chunk_NCT);

    basecall::nn::ConvStack convs{nullptr};
    TxEncoderStack tx_encoder{nullptr};
    LinearUpsample tx_decoder{nullptr};
    LinearScaledCRF crf{nullptr};

    const at::TensorOptions m_options;
};

TORCH_MODULE(TxModel);

}  // namespace nn

}  // namespace dorado::basecall

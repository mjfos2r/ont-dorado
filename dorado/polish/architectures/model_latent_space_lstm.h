#pragma once

#include "model_torch_base.h"
#include "polish/polish_utils.h"

#include <torch/torch.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace dorado::polisher {

// Function to create 1D convolutional layers
inline torch::nn::Sequential make_1d_conv_layers(const std::vector<int32_t>& kernel_sizes,
                                                 int32_t num_in_features,
                                                 const std::vector<int32_t>& channels,
                                                 const bool use_batch_norm,
                                                 const std::string& activation = "ReLU") {
    if (std::size(kernel_sizes) != std::size(channels)) {
        throw std::invalid_argument("channels and kernel_sizes must have the same size");
    }

    for (const int32_t k : kernel_sizes) {
        if ((k % 2) == 0) {
            throw std::invalid_argument(
                    "Kernel sizes must be odd for equal and symmetric padding. Given: k = " +
                    std::to_string(k));
        }
    }

    torch::nn::Sequential layers;
    for (size_t i = 0; i < std::size(kernel_sizes); ++i) {
        const int32_t k = kernel_sizes[i];
        const int32_t c = channels[i];

        layers->push_back(torch::nn::Conv1d(
                torch::nn::Conv1dOptions(num_in_features, c, k).padding((k - 1) / 2)));

        if (activation == "ReLU") {
            layers->push_back(torch::nn::ReLU());
        } else {
            throw std::invalid_argument("Activation " + activation + " not implemented");
        }

        if (use_batch_norm) {
            layers->push_back(torch::nn::BatchNorm1d(c));
        }

        num_in_features = c;
    }

    return layers;
}

// Class for ReadLevelConv
class ReadLevelConvImpl : public torch::nn::Module {
public:
    ReadLevelConvImpl(const int32_t num_in_features,             // 5
                      const int32_t out_dim,                     // 128
                      const std::vector<int32_t>& kernel_sizes,  // [1, 17]
                      const std::vector<int32_t>& channel_dims,
                      bool use_batch_norm  // True
                      )
            : m_convs{make_1d_conv_layers(kernel_sizes,
                                          num_in_features,
                                          channel_dims,
                                          use_batch_norm)},
              m_expansion_layer{torch::nn::Linear(channel_dims.back(), out_dim)}

    {
        if (std::size(kernel_sizes) != std::size(channel_dims)) {
            throw std::runtime_error(
                    "Wrong number of values in kernel_sizes or channel_dims. Got: "
                    "kernel_sizes.size() = " +
                    std::to_string(std::size(kernel_sizes)) +
                    ", channel_dims.size() = " + std::to_string(std::size(channel_dims)));
        }

        register_module("convs", m_convs);
        register_module("expansion_layer", m_expansion_layer);
    }

    torch::Tensor forward(torch::Tensor x) { return m_convs->forward(x); }

private:
    torch::nn::Sequential m_convs;
    torch::nn::Linear m_expansion_layer;
};
TORCH_MODULE(ReadLevelConv);

class MeanPoolerImpl : public torch::nn::Module {
public:
    torch::Tensor forward(torch::Tensor x, torch::Tensor non_empty_position_mask) {
        // const auto read_depths = non_empty_position_mask.sum(-1, true);
        const auto read_depths = non_empty_position_mask.sum(-1).unsqueeze(-1).unsqueeze(-1);

        const auto mask = non_empty_position_mask.unsqueeze(-1).unsqueeze(-1);
        // std::cerr << "[IS] MeanPoolerImpl: x.shape = " << tensor_shape_as_string(x) << ", read_depths.shape = " << tensor_shape_as_string(read_depths)
        //     << ", mask.shape = " << tensor_shape_as_string(mask) << ", read_depths.shape = " << tensor_shape_as_string(read_depths) << "\n";
        x = x * mask;
        // std::cerr << "[IS] After: x = x * mask, x.shape = " << tensor_shape_as_string(x) << "\n";
        x = x.sum(1);
        // std::cerr << "[IS] After: x = x.sum(1), x.shape = " << tensor_shape_as_string(x) << "\n";
        x = x / read_depths;  // TODO: Does the read_depths need to be unsqueezed?
        // std::cerr << "[IS] After: x = x / read_depths, x.shape = " << tensor_shape_as_string(x) << "\n";
        return x;
    }
};
TORCH_MODULE(MeanPooler);

class ReversibleLSTM : public torch::nn::Module {
public:
    ReversibleLSTM(const int32_t input_size,
                   const int32_t hidden_size,
                   const bool batch_first,
                   const bool reverse)
            : m_lstm(torch::nn::LSTMOptions(input_size, hidden_size).batch_first(batch_first)),
              m_batch_first{batch_first},
              m_reverse(reverse) {
        register_module("lstm", m_lstm);
    }

    torch::Tensor forward(torch::Tensor x) {
        const int32_t flip_dim = m_batch_first ? 1 : 0;
        if (m_reverse) {
            x = x.flip(flip_dim);  // Flip along the sequence dimension
        }
        auto output = std::get<0>(m_lstm->forward(x));  // .output;
        if (m_reverse) {
            output = output.flip(flip_dim);
        }
        return output;
    }

private:
    torch::nn::LSTM m_lstm;
    bool m_batch_first = false;
    bool m_reverse = false;
};

class ModelLatentSpaceLSTM : public ModelTorchBase {
public:
    ModelLatentSpaceLSTM(const int32_t num_classes = 5,
                         const int32_t lstm_size = 128,
                         const int32_t cnn_size = 128,
                         const std::vector<int32_t> kernel_sizes = {1, 17},
                         const std::string& pooler_type = "mean",
                         const bool use_dwells = false,
                         const int32_t bases_alphabet_size = 6,
                         const int32_t bases_embedding_size = 6,
                         const bool bidirectional = true)
            : m_base_embedder(
                      torch::nn::EmbeddingOptions(bases_alphabet_size, bases_embedding_size)),
              strand_embedder(torch::nn::EmbeddingOptions(3, bases_embedding_size)),
              m_read_level_conv(bases_embedding_size + (use_dwells ? 2 : 1),
                                lstm_size,
                                kernel_sizes,
                                std::vector<int32_t>(std::size(kernel_sizes), cnn_size),
                                true),
              m_pre_pool_expansion_layer(cnn_size, lstm_size),
              m_pooler(MeanPooler()),  // register_module("pooler", MeanPooler())),
                                       //   m_lstm(),
              m_lstm_bidir(torch::nn::LSTMOptions(lstm_size, lstm_size)
                                   .num_layers(2)
                                   .batch_first(true)
                                   .bidirectional(bidirectional)),
              m_lstm_unidir(),
              m_linear((1 + bidirectional) * lstm_size, num_classes),
              m_normalise(true),
              m_lstm_size(lstm_size),
              m_use_dwells(use_dwells),
              m_bidirectional(bidirectional) {
        if (bidirectional) {
            m_lstm_bidir = torch::nn::LSTM(torch::nn::LSTMOptions(lstm_size, lstm_size)
                                                   .num_layers(2)
                                                   .batch_first(true)
                                                   .bidirectional(bidirectional));
            // m_lstm->push_back(torch::nn::LSTM(torch::nn::LSTMOptions(lstm_size, lstm_size)
            //                                  .num_layers(2)
            //                                  .batch_first(true)
            //                                  .bidirectional(bidirectional)));
        } else {
            for (int32_t i = 0; i < 4; ++i) {
                m_lstm_unidir->push_back(ReversibleLSTM(lstm_size, lstm_size, true, !(i % 2)));
            }
        }

        if (pooler_type != "mean") {
            throw std::runtime_error("Pooler " + pooler_type + " not implemented yet.");
        }

        register_module("base_embedder", m_base_embedder);
        register_module("strand_embedder", strand_embedder);
        register_module("read_level_conv", m_read_level_conv);
        register_module("pre_pool_expansion_layer", m_pre_pool_expansion_layer);
        register_module("pooler", m_pooler);
        if (bidirectional) {
            register_module("lstm", m_lstm_bidir);
        } else {
            register_module("lstm", m_lstm_unidir);
        }
        register_module("linear", m_linear);
    }

    torch::Tensor forward(torch::Tensor x) {
        const auto non_empty_position_mask = (x.sum({1, -1}) != 0);

        auto bases_embedding = m_base_embedder->forward(
                x.index({torch::indexing::Slice(), torch::indexing::Slice(),
                         torch::indexing::Slice(), 0})
                        .to(torch::kLong));
        auto strand_embedding = strand_embedder->forward(
                x.index({torch::indexing::Slice(), torch::indexing::Slice(),
                         torch::indexing::Slice(), 2})
                        .to(torch::kLong) +
                1);
        auto scaled_q_scores = (x.index({torch::indexing::Slice(), torch::indexing::Slice(),
                                         torch::indexing::Slice(), 1}) /
                                        25 -
                                1)
                                       .unsqueeze(-1);

        if (m_use_dwells) {
            if (x.sizes().back() != 5) {
                throw std::runtime_error(
                        "if using dwells, x must have 5 features/read/position. Shape of x: " +
                        tensor_shape_as_string(x));
            }
            auto dwells = x.index({torch::indexing::Slice(), torch::indexing::Slice(),
                                   torch::indexing::Slice(), 4})
                                  .unsqueeze(-1);
            x = torch::cat({std::move(bases_embedding) + std::move(strand_embedding),
                            std::move(scaled_q_scores), std::move(dwells)},
                           -1);
        } else {
            x = torch::cat({std::move(bases_embedding) + std::move(strand_embedding),
                            std::move(scaled_q_scores)},
                           -1);
        }

        x = x.permute({0, 2, 3, 1});

        // The sizes() returns a torch::IntArrayRef
        const auto b = x.sizes()[0];
        const auto d = x.sizes()[1];
        // const auto f = x.sizes()[2];
        const auto p = x.sizes()[3];

        x = x.flatten(0, 1);
        x = m_read_level_conv->forward(x);
        x = x.permute({0, 2, 1});
        x = m_pre_pool_expansion_layer->forward(x);
        x = x.view({b, d, p, m_lstm_size});
        // std::cerr << "[IS] x.shape = " << tensor_shape_as_string(x) << ", non_empty_position_mask.shape = " << tensor_shape_as_string(non_empty_position_mask) << "\n";
        x = m_pooler->forward(x, non_empty_position_mask);
        x = m_bidirectional ? std::get<0>(m_lstm_bidir->forward(x)) : m_lstm_unidir->forward(x);
        x = m_linear->forward(x);

        if (m_normalise) {
            x = torch::softmax(x, -1);
        }

        return x;
    }

private:
    torch::nn::Embedding m_base_embedder;
    torch::nn::Embedding strand_embedder;
    ReadLevelConv m_read_level_conv;
    torch::nn::Linear m_pre_pool_expansion_layer;
    MeanPooler m_pooler{nullptr};
    torch::nn::LSTM m_lstm_bidir;
    torch::nn::Sequential m_lstm_unidir{nullptr};
    torch::nn::Linear m_linear;
    bool m_normalise = false;
    int32_t m_lstm_size = 0;
    bool m_use_dwells = false;
    bool m_bidirectional = true;
};

}  // namespace dorado::polisher

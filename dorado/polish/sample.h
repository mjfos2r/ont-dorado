#pragma once

#include "polish_utils.h"

#include <ATen/ATen.h>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <tuple>
#include <vector>

namespace dorado::polisher {

struct Sample {
    at::Tensor features;
    std::vector<int64_t> positions_major;
    std::vector<int64_t> positions_minor;
    at::Tensor depth;
    int32_t seq_id = -1;
    int32_t region_id = -1;
    std::vector<std::string> read_ids_left;
    std::vector<std::string> read_ids_right;

    Sample() = default;

    Sample(at::Tensor features_,
           std::vector<int64_t> positions_major_,
           std::vector<int64_t> positions_minor_,
           at::Tensor depth_,
           const int32_t seq_id_,
           const int32_t region_id_,
           std::vector<std::string> read_ids_left_,
           std::vector<std::string> read_ids_right_)
            : features{std::move(features_)},
              positions_major{std::move(positions_major_)},
              positions_minor{std::move(positions_minor_)},
              depth{std::move(depth_)},
              seq_id{seq_id_},
              region_id{region_id_},
              read_ids_left{std::move(read_ids_left_)},
              read_ids_right{std::move(read_ids_right_)} {}

    int64_t start() const { return (std::empty(positions_major) ? -1 : (positions_major.front())); }

    int64_t end() const {
        return (std::empty(positions_major) ? -1 : (positions_major.back() + 1));
    }

    std::pair<int64_t, int64_t> get_position(const int64_t idx) const {
        if ((idx < 0) || (idx >= static_cast<int64_t>(std::size(positions_major)))) {
            return {-1, -1};
        }
        return {positions_major[idx], positions_minor[idx]};
    }

    std::pair<int64_t, int64_t> get_last_position() const {
        return get_position(static_cast<int64_t>(std::size(positions_major)) - 1);
    }
};

Sample slice_sample(const Sample& sample, const int64_t idx_start, const int64_t idx_end);

void debug_print_sample(std::ostream& os,
                        const Sample& sample,
                        int64_t start /*= 0*/,
                        int64_t end /*= -1 */,
                        bool debug /*= false */);

std::ostream& operator<<(std::ostream& os, const Sample& sample);

}  // namespace dorado::polisher

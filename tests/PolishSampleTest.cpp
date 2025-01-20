#include "polish/sample.h"

#include <catch2/catch.hpp>
#include <torch/torch.h>

#include <cstdint>
#include <vector>

namespace dorado::polisher::sample::tests {

#define TEST_GROUP "[PolishSample]"

TEST_CASE("slice_sample: Basic slicing", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.features = torch::tensor({{1, 2, 3, 4, 5},
                                     {6, 7, 8, 9, 10},
                                     {11, 12, 13, 14, 15},
                                     {16, 17, 18, 19, 20},
                                     {21, 22, 23, 24, 25},
                                     {26, 27, 28, 29, 30},
                                     {31, 32, 33, 34, 35},
                                     {36, 37, 38, 39, 40},
                                     {41, 42, 43, 44, 45},
                                     {46, 47, 48, 49, 50}},
                                    torch::kInt32);
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    sample.depth =
            torch::tensor({1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8, 9.9, 10.1}, torch::kFloat32);

    SECTION("Slice middle range") {
        // Run unit under test.
        const int64_t idx_start = 2;
        const int64_t idx_end = 7;
        const Sample sliced_sample = slice_sample(sample, idx_start, idx_end);

        // Expected results.
        const auto expected_features = torch::tensor({{11, 12, 13, 14, 15},
                                                      {16, 17, 18, 19, 20},
                                                      {21, 22, 23, 24, 25},
                                                      {26, 27, 28, 29, 30},
                                                      {31, 32, 33, 34, 35}},
                                                     torch::kInt32);
        const auto expected_depth = torch::tensor({3.3, 4.4, 5.5, 6.6, 7.7}, torch::kFloat32);
        const std::vector<int64_t> expected_positions_major{2, 3, 4, 5, 6};
        const std::vector<int64_t> expected_positions_minor{12, 13, 14, 15, 16};

        // Evaluate.
        CHECK(sliced_sample.seq_id == sample.seq_id);
        CHECK(sliced_sample.features.equal(expected_features));
        CHECK(sliced_sample.depth.equal(expected_depth));
        CHECK(sliced_sample.positions_major == expected_positions_major);
        CHECK(sliced_sample.positions_minor == expected_positions_minor);
        CHECK(std::empty(sliced_sample.read_ids_left));
        CHECK(std::empty(sliced_sample.read_ids_right));
    }

    SECTION("Slice entire range") {
        // Run unit under test.
        const int64_t idx_start = 0;
        const int64_t idx_end = 10;
        const Sample sliced_sample = slice_sample(sample, idx_start, idx_end);

        // Evaluate.
        CHECK(sliced_sample.seq_id == sample.seq_id);
        CHECK(sliced_sample.features.equal(sample.features));
        CHECK(sliced_sample.depth.equal(sample.depth));
        CHECK(sliced_sample.positions_major == sample.positions_major);
        CHECK(sliced_sample.positions_minor == sample.positions_minor);
        CHECK(std::empty(sliced_sample.read_ids_left));
        CHECK(std::empty(sliced_sample.read_ids_right));
    }

    SECTION("Slice single row") {
        // Run unit under test.
        const int64_t idx_start = 4;
        const int64_t idx_end = 5;
        const Sample sliced_sample = slice_sample(sample, idx_start, idx_end);

        // Expected results.
        const auto expected_features = torch::tensor({{21, 22, 23, 24, 25}}, torch::kInt32);
        const auto expected_depth = torch::tensor({5.5}, torch::kFloat32);
        const std::vector<int64_t> expected_positions_major{4};
        const std::vector<int64_t> expected_positions_minor{14};

        // Evaluate.
        CHECK(sliced_sample.seq_id == sample.seq_id);
        CHECK(sliced_sample.features.equal(expected_features));
        CHECK(sliced_sample.depth.equal(expected_depth));
        CHECK(sliced_sample.positions_major == expected_positions_major);
        CHECK(sliced_sample.positions_minor == expected_positions_minor);
        CHECK(std::empty(sliced_sample.read_ids_left));
        CHECK(std::empty(sliced_sample.read_ids_right));
    }
}

TEST_CASE("slice_sample: Error conditions", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.features = torch::rand({10, 5});
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    sample.depth = torch::rand({10});

    SECTION("Invalid range: idx_start >= idx_end") {
        CHECK_THROWS_AS(slice_sample(sample, 5, 5), std::out_of_range);
        CHECK_THROWS_AS(slice_sample(sample, 6, 5), std::out_of_range);
    }

    SECTION("Invalid range: idx_start or idx_end out of bounds") {
        CHECK_THROWS_AS(slice_sample(sample, -1, 5), std::out_of_range);
        CHECK_THROWS_AS(slice_sample(sample, 0, 11), std::out_of_range);
        CHECK_THROWS_AS(slice_sample(sample, 10, 11), std::out_of_range);
    }
}

TEST_CASE("slice_sample: features tensor in sample not defined", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    sample.depth = torch::rand({10});

    CHECK_THROWS_AS(slice_sample(sample, 0, 5), std::runtime_error);
}

TEST_CASE("slice_sample: depth tensor in sample not defined", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.features = torch::rand({10, 5});
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};

    CHECK_THROWS_AS(slice_sample(sample, 0, 5), std::runtime_error);
}

TEST_CASE("slice_sample: error, wrong length of the features tensor.", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.features = torch::rand({20, 5});
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    sample.depth = torch::rand({10});

    CHECK_THROWS_AS(slice_sample(sample, 0, 5), std::runtime_error);
}

TEST_CASE("slice_sample: error, wrong length of the depth tensor.", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.features = torch::rand({10, 5});
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    sample.depth = torch::rand({20});

    CHECK_THROWS_AS(slice_sample(sample, 0, 5), std::runtime_error);
}

TEST_CASE("slice_sample: error, wrong length of the positions_major vector.", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.features = torch::rand({10, 5});
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    sample.depth = torch::rand({10});

    CHECK_THROWS_AS(slice_sample(sample, 0, 5), std::runtime_error);
}

TEST_CASE("slice_sample: error, wrong length of the positions_minor vector.", TEST_GROUP) {
    // Create a mock sample.
    Sample sample;
    sample.seq_id = 1;
    sample.features = torch::rand({10, 5});
    sample.positions_major = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    sample.positions_minor = {10, 11, 12, 13, 14, 15, 16, 17, 18};
    sample.depth = torch::rand({10});

    CHECK_THROWS_AS(slice_sample(sample, 0, 5), std::runtime_error);
}

}  // namespace dorado::polisher::sample::tests
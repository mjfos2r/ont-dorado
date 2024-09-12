#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dorado::poly_tail {

class PolyTailCalculator;

class PolyTailCalculatorSelector {
public:
    PolyTailCalculatorSelector(const std::filesystem::path& config,
                               bool is_rna,
                               bool is_rna_adapter);
    PolyTailCalculatorSelector(std::istream& config_stream, bool is_rna, bool is_rna_adapter);

    std::shared_ptr<const PolyTailCalculator> get_calculator(const std::string& name) const;

private:
    void init(std::istream& config_stream, bool is_rna, bool is_rna_adapter);

    mutable std::mutex m_lut_mutex;
    std::unordered_map<std::string, std::shared_ptr<const PolyTailCalculator>> m_lut;
    std::shared_ptr<const PolyTailCalculator> m_default;
};

}  // namespace dorado::poly_tail

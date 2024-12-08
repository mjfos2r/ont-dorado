#pragma once

#include <htslib/sam.h>

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <tuple>
#include <vector>

struct HeaderLineData {
    std::string header_type;
    std::vector<std::pair<std::string, std::string>> tags;
};

class BamFile {
public:
    BamFile(const std::filesystem::path& in_fn);

    // Getters.
    htsFile* fp() const { return m_fp.get(); }
    hts_idx_t* idx() const { return m_idx.get(); }
    sam_hdr_t* hdr() const { return m_hdr.get(); }

    htsFile* fp() { return m_fp.get(); }
    hts_idx_t* idx() { return m_idx.get(); }
    sam_hdr_t* hdr() { return m_hdr.get(); }

    std::vector<HeaderLineData> parse_header() const;

private:
    std::unique_ptr<htsFile, decltype(&hts_close)> m_fp;
    std::unique_ptr<hts_idx_t, decltype(&hts_idx_destroy)> m_idx;
    std::unique_ptr<sam_hdr_t, decltype(&sam_hdr_destroy)> m_hdr;
};

void header_to_stream(std::ostream& os, const std::vector<HeaderLineData>& header);

std::string header_to_string(const std::vector<HeaderLineData>& header);

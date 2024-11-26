#include "gzip_reader.h"

#include <zlib.h>

namespace dorado::utils {

void ZstreamDestructor::operator()(z_stream* zlib_stream) {
    if (zlib_stream) {
        inflateEnd(zlib_stream);
    }
}

GzipReader::GzipReader(std::string gzip_file, std::size_t buffer_size)
        : m_gzip_file(std::move(gzip_file)),
          m_buffer_size(buffer_size),
          m_compressed_buffer(buffer_size),
          m_decompressed_buffer(buffer_size) {
    m_compressed_stream.open(m_gzip_file, std::ios::binary);
    if (!m_compressed_stream.is_open()) {
        set_failure("Cannot open file for gzip reading.");
        return;
    }
    m_zlib_stream = create_zlib_stream();
}

bool GzipReader::is_valid() const { return m_is_valid; }

const std::string& GzipReader::error_message() const { return m_error_message; }

std::size_t GzipReader::num_bytes_read() const { return m_num_bytes_read; }

bool GzipReader::read_next() {
    if (!try_prepare_for_next_decompress()) {
        return false;
    }

    auto inflate_result = inflate(m_zlib_stream.get(), Z_NO_FLUSH);
    if (inflate_result != Z_OK && inflate_result != Z_STREAM_END) {
        set_failure("Could not inflate input stream. Error: " + std::to_string(inflate_result));
        return false;
    }

    m_num_bytes_read = m_buffer_size - m_zlib_stream->avail_out;
    if (inflate_result == Z_STREAM_END) {
        m_zlib_stream = create_next_zlib_stream(*m_zlib_stream);
    } else if (m_num_bytes_read == 0 && m_zlib_stream->avail_in == 0) {
        if (!fetch_next_compressed_chunk()) {
            return false;
        }
    }
    return true;
}

std::vector<char>& GzipReader::decompressed_buffer() { return m_decompressed_buffer; }

void GzipReader::set_failure(std::string error_message) {
    m_is_valid = false;
    m_error_message = std::move(error_message) + " [" + m_gzip_file + "]";
}

bool GzipReader::try_initialise_zlib_stream(z_stream& zlib_stream) {
    zlib_stream.zalloc = Z_NULL;  // B
    zlib_stream.zfree = Z_NULL;
    zlib_stream.opaque = Z_NULL;
    zlib_stream.avail_in = 0;
    zlib_stream.next_in = Z_NULL;
    auto init_result = inflateInit2(&zlib_stream, MAX_WBITS + 16);  // +16 for GZip
    if (init_result != Z_OK) {
        set_failure("Could not initialize stream for inflating. Error: " +
                    std::to_string(init_result));
        return false;
    }
    return true;
}

ZstreamPtr GzipReader::create_zlib_stream() {
    ZstreamPtr zlib_stream(new z_stream());
    if (!try_initialise_zlib_stream(*zlib_stream)) {
        return {};
    }
    return zlib_stream;
}

ZstreamPtr GzipReader::create_next_zlib_stream(const z_stream& last_zlib_stream) {
    auto next_zlib_stream = create_zlib_stream();
    next_zlib_stream->avail_in = last_zlib_stream.avail_in;
    next_zlib_stream->next_in = last_zlib_stream.next_in;
    return next_zlib_stream;
}

bool GzipReader::fetch_next_compressed_chunk() {
    m_num_bytes_read = 0;
    if (!m_compressed_stream.is_open()) {
        set_failure("Error, attempting to read file which is not open.");
    }
    if (m_compressed_stream.eof()) {
        return false;
    }
    if (!m_compressed_stream.good()) {
        set_failure("Error reading from compressed input.");
    }
    m_compressed_stream.read(m_compressed_buffer.data(), m_compressed_buffer.size());
    m_zlib_stream->avail_in = static_cast<uint32_t>(m_compressed_stream.gcount());
    m_zlib_stream->next_in = reinterpret_cast<unsigned char*>(m_compressed_buffer.data());
    return true;
}

bool GzipReader::try_prepare_for_next_decompress() {
    if (!is_valid()) {
        return false;
    }
    if (m_zlib_stream->avail_in == 0 && !fetch_next_compressed_chunk()) {
        return false;
    }
    m_zlib_stream->avail_out = static_cast<uint32_t>(m_buffer_size);
    m_zlib_stream->next_out = reinterpret_cast<unsigned char*>(m_decompressed_buffer.data());
    return true;
}

}  // namespace dorado::utils
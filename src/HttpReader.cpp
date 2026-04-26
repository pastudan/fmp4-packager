#include "HttpReader.h"
#include <stdexcept>
#include <cstring>
#include <iostream>

struct CurlWriteData {
    uint8_t* buffer;
    int buffer_size;
    int bytes_written;
};

static size_t CurlWriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* data = static_cast<CurlWriteData*>(userdata);
    size_t total = size * nmemb;
    size_t to_copy = std::min(total, static_cast<size_t>(data->buffer_size - data->bytes_written));
    
    if (to_copy > 0) {
        memcpy(data->buffer + data->bytes_written, ptr, to_copy);
        data->bytes_written += to_copy;
    }
    
    return total;
}

HttpReader::HttpReader(const std::string& url) : url_(url) {
    curl_ = curl_easy_init();
    if (!curl_)
        throw std::runtime_error("Failed to initialize curl");

    FetchFileSize();

    avio_buffer_ = static_cast<uint8_t*>(av_malloc(kBufferSize));
    if (!avio_buffer_)
        throw std::runtime_error("Failed to allocate AVIO buffer");

    avio_ctx_ = avio_alloc_context(
        avio_buffer_,
        kBufferSize,
        0,  // write_flag = 0 (read-only)
        this,
        ReadPacket,
        nullptr,  // no write
        Seek
    );

    if (!avio_ctx_) {
        av_free(avio_buffer_);
        throw std::runtime_error("Failed to allocate AVIOContext");
    }
}

HttpReader::~HttpReader() {
    if (avio_ctx_) {
        av_freep(&avio_ctx_->buffer);
        avio_context_free(&avio_ctx_);
    }
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

void HttpReader::FetchFileSize() {
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl_);
    if (res == CURLE_OK) {
        curl_off_t cl;
        if (curl_easy_getinfo(curl_, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK && cl > 0) {
            file_size_ = cl;
        }
    }
}

AVIOContext* HttpReader::GetAvioContext() {
    return avio_ctx_;
}

int HttpReader::ReadPacket(void* opaque, uint8_t* buf, int buf_size) {
    auto* reader = static_cast<HttpReader*>(opaque);
    return reader->DoRead(buf, buf_size);
}

int64_t HttpReader::Seek(void* opaque, int64_t offset, int whence) {
    auto* reader = static_cast<HttpReader*>(opaque);
    return reader->DoSeek(offset, whence);
}

int HttpReader::DoRead(uint8_t* buf, int buf_size) {
    if (file_size_ > 0 && position_ >= file_size_) {
        return AVERROR_EOF;
    }

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, CurlWriteCallback);

    CurlWriteData write_data{buf, buf_size, 0};
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &write_data);

    int64_t end = position_ + buf_size - 1;
    if (file_size_ > 0 && end >= file_size_) {
        end = file_size_ - 1;
    }

    char range[64];
    snprintf(range, sizeof(range), "%lld-%lld", (long long)position_, (long long)end);
    curl_easy_setopt(curl_, CURLOPT_RANGE, range);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        std::cerr << "[HttpReader] curl error: " << curl_easy_strerror(res) << std::endl;
        return AVERROR(EIO);
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200 && http_code != 206) {
        std::cerr << "[HttpReader] HTTP " << http_code << " for range " << range << std::endl;
        return AVERROR(EIO);
    }

    if (write_data.bytes_written == 0) {
        return AVERROR_EOF;
    }

    position_ += write_data.bytes_written;
    return write_data.bytes_written;
}

int64_t HttpReader::DoSeek(int64_t offset, int whence) {
    int64_t new_pos;

    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = position_ + offset;
            break;
        case SEEK_END:
            if (file_size_ < 0) return AVERROR(EINVAL);
            new_pos = file_size_ + offset;
            break;
        case AVSEEK_SIZE:
            return file_size_;
        default:
            return AVERROR(EINVAL);
    }

    if (new_pos < 0) return AVERROR(EINVAL);
    if (file_size_ > 0 && new_pos > file_size_) new_pos = file_size_;

    position_ = new_pos;
    return position_;
}

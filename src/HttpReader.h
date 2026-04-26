#pragma once

#include <string>
#include <memory>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <curl/curl.h>
}

class HttpReader {
public:
    HttpReader(const std::string& url);
    ~HttpReader();

    AVIOContext* GetAvioContext();
    int64_t GetFileSize() const { return file_size_; }

    static int ReadPacket(void* opaque, uint8_t* buf, int buf_size);
    static int64_t Seek(void* opaque, int64_t offset, int whence);

private:
    int DoRead(uint8_t* buf, int buf_size);
    int64_t DoSeek(int64_t offset, int whence);
    void FetchFileSize();

    std::string url_;
    CURL* curl_ = nullptr;
    int64_t position_ = 0;
    int64_t file_size_ = -1;

    AVIOContext* avio_ctx_ = nullptr;
    uint8_t* avio_buffer_ = nullptr;
    static constexpr int kBufferSize = 1024 * 1024;  // 1MB AVIO buffer
};

#include "wav_file.h"

#include <limits>
#include <sstream>
#include <cstring>

#include <android/log.h>

// 日志宏仅在 .cpp 内定义（头文件不定义，避免与 player/recorder 的 LOG_TAG 冲突）
#define LOG_TAG "AAudioWavFile"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

WavFile::WavFile() : is_open_(false), data_size_(0) {}

WavFile::~WavFile() noexcept {
    close();
}

bool WavFile::openRead(const std::string& filePath) {
    close();

    file_path_ = filePath;
    in_.open(filePath, std::ios::binary);
    if (!in_.is_open()) {
        LOGE("Failed to open file: %s", filePath.c_str());
        return false;
    }

    if (!readHeader()) {
        LOGE("Failed to read WAV header from: %s", filePath.c_str());
        close();
        return false;
    }

    if (!isValidFormat()) {
        LOGE("Invalid WAV format in file: %s", filePath.c_str());
        close();
        return false;
    }

    remaining_data_ = header_.subchunk2_size;
    read_error_ = false;
    is_open_ = true;
    LOGI("WAV file opened: %s, %s", filePath.c_str(), getFormatInfo().c_str());
    return true;
}

size_t WavFile::readAudioData(void* buffer, size_t bufferSize) {
    if (!is_open_ || !buffer || bufferSize == 0) {
        return 0;
    }
    // 只读 data 区，不越过 data chunk 尾部（尾随元数据 chunk 否则会混入音频）。
    bufferSize = std::min(bufferSize, remaining_data_);
    if (bufferSize == 0) {
        return 0;
    }
    constexpr auto kMaxStreamSize = static_cast<size_t>(std::numeric_limits<std::streamsize>::max());
    size_t actual_read_size = std::min(bufferSize, kMaxStreamSize);
    auto read_size = static_cast<std::streamsize>(actual_read_size);
    in_.read(static_cast<char*>(buffer), read_size);
    const size_t n = static_cast<size_t>(in_.gcount());
    if (n == 0 && remaining_data_ > 0) {
        // 声明的 data 区未读完即止：I/O 失败或文件被截断（区别于正常读尽）
        read_error_ = true;
    }
    remaining_data_ -= n;
    return n;
}

bool WavFile::openWrite(const std::string& filePath, int32_t sampleRate,
                        int32_t channelCount, aaudio_format_t format) {
    close();

    file_path_ = filePath;
    header_ = {};
    write_format_ = format;
    header_.sample_rate = static_cast<uint32_t>(sampleRate);
    header_.num_channels = static_cast<uint16_t>(channelCount);
    header_.bits_per_sample = static_cast<uint16_t>(getBytesPerSample(format) * 8);
    data_size_ = 0;

    out_.open(filePath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        LOGE("Failed to open WAV file for writing: %s", filePath.c_str());
        return false;
    }

    writeHeader(0);
    is_open_ = true;
    LOGI("WAV file opened for writing: %s, %dHz, %dch, %dbit", filePath.c_str(),
         sampleRate, channelCount, header_.bits_per_sample);
    return true;
}

bool WavFile::writeData(const void* data, size_t size) {
    if (!is_open_ || !data || size == 0) {
        return false;
    }
    // 4GB 是 WAV 32 位 size 字段上限；须写前检查——写后再查则末块已入盘且 data_size_ 已回绕
    if (static_cast<uint64_t>(data_size_) + size > std::numeric_limits<uint32_t>::max()) {
        LOGE("Data size exceeds 4GB WAV limit, refusing write (file finalized at limit)");
        return false;
    }
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (out_.fail()) {
        LOGE("Failed to write data to WAV file");
        return false;
    }
    data_size_ += static_cast<uint32_t>(size);
    return true;
}

bool WavFile::close() {
    bool ok = true;
    if (out_.is_open()) {
        writeHeader(data_size_);
        ok = !out_.fail();
        out_.close();
        ok = ok && !out_.fail();
        if (ok) {
            LOGI("WAV file closed: %s, final size: %u bytes", file_path_.c_str(), data_size_);
        } else {
            LOGE("Failed to finalize WAV file: %s (header backfill or close failed)", file_path_.c_str());
        }
    }
    if (in_.is_open()) {
        in_.close();
    }
    is_open_ = false;
    header_ = {};
    data_size_ = 0;
    remaining_data_ = 0;
    write_format_ = AAUDIO_FORMAT_PCM_I16;
    return ok;
}

bool WavFile::isOpen() const {
    return is_open_;
}

int32_t WavFile::getSampleRate() const {
    return static_cast<int32_t>(header_.sample_rate);
}

int32_t WavFile::getChannelCount() const {
    return static_cast<int32_t>(header_.num_channels);
}

aaudio_format_t WavFile::getAAudioFormat() const {
    switch (header_.bits_per_sample) {
        case 16:
            return AAUDIO_FORMAT_PCM_I16;
        case 24:
            return AAUDIO_FORMAT_PCM_I24_PACKED;
        case 32:
            return AAUDIO_FORMAT_PCM_I32;
        default:
            return AAUDIO_FORMAT_PCM_I16;
    }
}

std::string WavFile::getFormatInfo() const {
    std::ostringstream oss;
    oss << static_cast<int32_t>(header_.sample_rate) << "Hz, "
        << static_cast<int32_t>(header_.num_channels) << " channels, "
        << static_cast<int32_t>(header_.bits_per_sample) << " bits, PCM";
    return oss.str();
}

bool WavFile::isValidFormat() const {
    // Only PCM (format 1) is accepted here, so a float WAV (format 3, written by writeHeader for
    // PCM_FLOAT) could not be read back. In practice float is unreachable: the Kotlin layer maps
    // 16/24/32-bit to I16/I24_PACKED/I32 only.
    return (header_.audio_format == 1 && header_.num_channels > 0 && header_.num_channels <= 16 &&
            header_.sample_rate > 0 && header_.sample_rate <= 192000 &&
            (header_.bits_per_sample == 16 || header_.bits_per_sample == 24 ||
             header_.bits_per_sample == 32) &&
            header_.subchunk2_size > 0);
}

int32_t WavFile::getBytesPerSample(aaudio_format_t format) {
    switch (format) {
        case AAUDIO_FORMAT_PCM_I16:
            return 2;
        case AAUDIO_FORMAT_PCM_I24_PACKED:
            return 3;
        case AAUDIO_FORMAT_PCM_I32:
        case AAUDIO_FORMAT_PCM_FLOAT:
            return 4;
        default:
            return 2;
    }
}

bool WavFile::readHeader() {
    in_.seekg(0, std::ios::beg);
    if (!in_.good()) {
        LOGE("Failed to seek to beginning of file");
        return false;
    }
    return validateRiffHeader() && readFmtChunk() && findDataChunk();
}

bool WavFile::validateRiffHeader() {
    in_.read(header_.chunk_id, 4);
    if (in_.gcount() != 4 || strncmp(header_.chunk_id, "RIFF", 4) != 0) {
        LOGE("Invalid RIFF header");
        return false;
    }
    in_.read(reinterpret_cast<char*>(&header_.chunk_size), 4);
    if (in_.gcount() != 4) {
        LOGE("Failed to read RIFF size");
        return false;
    }
    in_.read(header_.format, 4);
    if (in_.gcount() != 4 || strncmp(header_.format, "WAVE", 4) != 0) {
        LOGE("Invalid WAVE header");
        return false;
    }
    return true;
}

bool WavFile::readFmtChunk() {
    char chunk_id[4];
    uint32_t chunk_size;

    while (in_.good()) {
        in_.read(chunk_id, 4);
        if (in_.gcount() != 4) {
            LOGE("Failed to read chunk ID");
            return false;
        }
        in_.read(reinterpret_cast<char*>(&chunk_size), 4);
        if (in_.gcount() != 4) {
            LOGE("Failed to read chunk size");
            return false;
        }

        if (strncmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                // 不足 16 字节时字段读取会越过 chunk 边界，参数全为垃圾值
                LOGE("Invalid fmt chunk size: %u", chunk_size);
                return false;
            }
            strncpy(header_.subchunk1_id, chunk_id, 4);
            in_.read(reinterpret_cast<char*>(&header_.audio_format), 2);
            in_.read(reinterpret_cast<char*>(&header_.num_channels), 2);
            in_.read(reinterpret_cast<char*>(&header_.sample_rate), 4);
            in_.read(reinterpret_cast<char*>(&header_.byte_rate), 4);
            in_.read(reinterpret_cast<char*>(&header_.block_align), 2);
            in_.read(reinterpret_cast<char*>(&header_.bits_per_sample), 2);
            if (chunk_size > 16) {
                skipChunk(chunk_size - 16);
            }
            return true;
        } else {
            skipChunk(chunk_size);
        }
    }
    LOGE("fmt chunk not found");
    return false;
}

bool WavFile::findDataChunk() {
    char chunk_id[4];
    uint32_t chunk_size;

    while (in_.good()) {
        in_.read(chunk_id, 4);
        if (in_.gcount() != 4) {
            LOGE("Failed to read chunk ID while looking for data");
            return false;
        }
        in_.read(reinterpret_cast<char*>(&chunk_size), 4);
        if (in_.gcount() != 4) {
            LOGE("Failed to read chunk size while looking for data");
            return false;
        }

        if (strncmp(chunk_id, "data", 4) == 0) {
            strncpy(header_.subchunk2_id, chunk_id, 4);
            header_.subchunk2_size = chunk_size;
            return true;
        } else {
            skipChunk(chunk_size);
        }
    }
    LOGE("data chunk not found");
    return false;
}

void WavFile::skipChunk(uint32_t chunk_size) {
    constexpr auto kMaxStreamOff = static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (static_cast<uint64_t>(chunk_size) > kMaxStreamOff) {
        LOGE("Chunk size too large: %u", chunk_size);
        return;
    }
    in_.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
    if (!in_.good()) {
        LOGE("Failed to skip chunk of size %u", chunk_size);
        return;
    }
    if (chunk_size % 2 == 1) {
        in_.seekg(1, std::ios::cur);
    }
}

void WavFile::writeHeader(uint32_t data_size) {
    if (!out_.is_open()) {
        return;
    }
    WavHeader header = {};
    memcpy(header.chunk_id, "RIFF", 4);
    header.chunk_size = 36 + data_size;
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1_id, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = (write_format_ == AAUDIO_FORMAT_PCM_FLOAT) ? 3 : 1;
    header.num_channels = header_.num_channels;
    header.sample_rate = header_.sample_rate;
    header.bits_per_sample = header_.bits_per_sample;
    header.block_align = static_cast<uint16_t>(header_.num_channels * (header_.bits_per_sample / 8));
    header.byte_rate = header.sample_rate * header.block_align;
    memcpy(header.subchunk2_id, "data", 4);
    header.subchunk2_size = data_size;

    out_.seekp(0, std::ios::beg);
    out_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out_.flush();
}

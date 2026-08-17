#ifndef AAUDIOTESTER_WAV_FILE_H_
#define AAUDIOTESTER_WAV_FILE_H_

#include <cstdint>
#include <fstream>
#include <string>

#include <aaudio/AAudio.h>

// 注意：本头文件不定义 LOG_TAG / LOGx 宏（player.cpp / recorder.cpp 各自定义了
// LOG_TAG，若本头再定义会在同一编译单元内触发宏重定义错误）。日志宏只在 wav_file.cpp 内定义。

// 合并后的 WAV 文件类：player 读 + recorder 写。
class WavFile {
public:
    WavFile();
    ~WavFile() noexcept;
    WavFile(const WavFile&) = delete;
    WavFile& operator=(const WavFile&) = delete;
    WavFile(WavFile&&) = delete;
    WavFile& operator=(WavFile&&) = delete;

    // 读模式（player）
    bool openRead(const std::string& filePath);
    size_t readAudioData(void* buffer, size_t bufferSize);
    int32_t getSampleRate() const;
    int32_t getChannelCount() const;
    aaudio_format_t getAAudioFormat() const;
    std::string getFormatInfo() const;
    bool isValidFormat() const;

    // 写模式（recorder）
    bool openWrite(const std::string& filePath, int32_t sampleRate,
                   int32_t channelCount, aaudio_format_t format);
    bool writeData(const void* data, size_t size);
    static int32_t getBytesPerSample(aaudio_format_t format);

    // 公共
    bool isOpen() const;
    void close();

private:
#pragma pack(push, 1)
    struct WavHeader {
        char chunk_id[4];            // "RIFF"
        uint32_t chunk_size;         // 36 + data_size
        char format[4];              // "WAVE"
        char subchunk1_id[4];        // "fmt "
        uint32_t subchunk1_size;     // 16 for PCM
        uint16_t audio_format;       // 1 PCM, 3 float
        uint16_t num_channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char subchunk2_id[4];        // "data"
        uint32_t subchunk2_size;
    };
#pragma pack(pop)

    std::ifstream in_;
    std::ofstream out_;
    std::string file_path_;
    WavHeader header_{};
    bool is_open_ = false;
    uint32_t data_size_ = 0;
    size_t remaining_data_ = 0;
    aaudio_format_t write_format_ = AAUDIO_FORMAT_PCM_I16;

    // 读辅助
    bool readHeader();
    bool validateRiffHeader();
    bool readFmtChunk();
    bool findDataChunk();
    void skipChunk(uint32_t chunk_size);
    // 写辅助
    void writeHeader(uint32_t data_size);
};

#endif  // AAUDIOTESTER_WAV_FILE_H_

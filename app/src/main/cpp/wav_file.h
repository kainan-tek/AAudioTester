#ifndef AAUDIOTESTER_WAV_FILE_H_
#define AAUDIOTESTER_WAV_FILE_H_

#include <cstdint>
#include <fstream>
#include <string>

#include <aaudio/AAudio.h>

// NOTE: This header does not define LOG_TAG / LOGx macros (player.cpp / recorder.cpp each define
// their own LOG_TAG; defining one here would trigger a macro redefinition error in the same
// translation unit). Logging macros are defined only in wav_file.cpp.

// Unified WAV file class: read by player + write by recorder.
class WavFile {
public:
    WavFile();
    ~WavFile() noexcept;
    WavFile(const WavFile&) = delete;
    WavFile& operator=(const WavFile&) = delete;
    WavFile(WavFile&&) = delete;
    WavFile& operator=(WavFile&&) = delete;

    // Read mode (player)
    bool openRead(const std::string& filePath);
    size_t readAudioData(void* buffer, size_t bufferSize);
    // Declared data section could not be fully read (I/O failure or truncated file)
    [[nodiscard]] bool hasReadError() const { return read_error_; }
    int32_t getSampleRate() const;
    int32_t getChannelCount() const;
    aaudio_format_t getAAudioFormat() const;
    std::string getFormatInfo() const;
    bool isValidFormat() const;

    // Write mode (recorder)
    bool openWrite(const std::string& filePath, int32_t sampleRate,
                   int32_t channelCount, aaudio_format_t format);
    bool writeData(const void* data, size_t size);

    // Common
    bool isOpen() const;
    // Write side: whether header backfill + close succeeded (failure = file unusable, report as error); read side: always true
    bool close();

private:
#pragma pack(push, 1)
    struct WavHeader {
        char chunk_id[4];            // "RIFF"
        uint32_t chunk_size;         // 36 + data_size
        char format[4];              // "WAVE"
        char subchunk1_id[4];        // "fmt "
        [[maybe_unused]] uint32_t subchunk1_size;     // 16 for PCM
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
    bool read_error_ = false;
    aaudio_format_t write_format_ = AAUDIO_FORMAT_PCM_I16;

    // Read helpers
    bool readHeader();
    bool validateRiffHeader();
    bool readFmtChunk();
    bool findDataChunk();
    void skipChunk(uint32_t chunk_size);
    // Write helpers
    void writeHeader(uint32_t data_size);
};

#endif  // AAUDIOTESTER_WAV_FILE_H_

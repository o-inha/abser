#include "audio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "../third_party/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"

namespace abser {
namespace {
constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint64_t kBufferFrames = 16384;
constexpr uint64_t kDecodeFrames = 1024;

std::wstring extension(const std::wstring& path) {
    auto ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext;
}

ma_encoding_format encoding(const std::wstring& path) {
    const auto ext = extension(path);
    if (ext == L".wav" || ext == L".wave") return ma_encoding_format_wav;
    if (ext == L".flac") return ma_encoding_format_flac;
    if (ext == L".mp3") return ma_encoding_format_mp3;
    if (ext == L".ogg") return ma_encoding_format_vorbis;
    return ma_encoding_format_unknown;
}

bool isAiff(const std::wstring& path);
ma_decoding_backend_vtable** aiffBackends();
#ifdef _WIN32
ma_vfs* sharedReadVfs();
#endif

ma_result openDecoder(const std::wstring& path, ma_decoder_config config, ma_decoder* decoder) {
    config.encodingFormat = encoding(path);
    if (isAiff(path)) { config.ppCustomBackendVTables = aiffBackends(); config.customBackendCount = 1; }
#ifdef _WIN32
    if (isAiff(path)) return ma_decoder_init_file_w(path.c_str(), &config, decoder);
    // All stock decoders share Unicode Windows I/O with FILE_SHARE_DELETE.
    // This also supplies the wide-path support absent from stb_vorbis itself.
    return ma_decoder_init_vfs_w(sharedReadVfs(), path.c_str(), &config, decoder);
#else
    return ma_decoder_init_file(std::filesystem::path(path).string().c_str(), &config, decoder);
#endif
}

FILE* openFile(const std::wstring& path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor < 0) { CloseHandle(handle); return nullptr; }
    FILE* file = _fdopen(descriptor, "rb");
    if (!file) _close(descriptor);
    return file;
#else
    return std::fopen(std::filesystem::path(path).string().c_str(), "rb");
#endif
}

#ifdef _WIN32
// Built-in decoder handles must permit Explorer/DAW moves while a file plays.
// A separate handle is still opened for each decoder and metadata operation.
ma_result sharedOpenW(ma_vfs*, const wchar_t* path, ma_uint32 mode, ma_vfs_file* file) {
    if (!file || !(mode & MA_OPEN_MODE_READ) || (mode & MA_OPEN_MODE_WRITE)) return MA_INVALID_ARGS;
    FILE* stream = openFile(path);
    if (!stream) return MA_DOES_NOT_EXIST;
    *file = stream;
    return MA_SUCCESS;
}
ma_result sharedOpen(ma_vfs* vfs, const char* path, ma_uint32 mode, ma_vfs_file* file) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (count <= 0) return MA_INVALID_ARGS;
    std::vector<wchar_t> wide(static_cast<size_t>(count));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), count)) return MA_INVALID_ARGS;
    return sharedOpenW(vfs, wide.data(), mode, file);
}
ma_result sharedClose(ma_vfs*, ma_vfs_file file) { return std::fclose(static_cast<FILE*>(file)) == 0 ? MA_SUCCESS : MA_IO_ERROR; }
ma_result sharedRead(ma_vfs*, ma_vfs_file file, void* output, size_t bytes, size_t* received) {
    FILE* stream = static_cast<FILE*>(file);
    const size_t count = std::fread(output, 1, bytes, stream);
    if (received) *received = count;
    if (std::ferror(stream)) return MA_IO_ERROR;
    // Vorbis's push decoder consumes partial reads only when they succeed.
    // Report EOF on the following zero-byte read, like miniaudio's default VFS.
    return count || !bytes ? MA_SUCCESS : MA_AT_END;
}
ma_result sharedSeek(ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
    const int whence = origin == ma_seek_origin_start ? SEEK_SET : origin == ma_seek_origin_current ? SEEK_CUR : SEEK_END;
    return _fseeki64(static_cast<FILE*>(file), offset, whence) == 0 ? MA_SUCCESS : MA_BAD_SEEK;
}
ma_result sharedTell(ma_vfs*, ma_vfs_file file, ma_int64* cursor) {
    if (!cursor) return MA_INVALID_ARGS;
    const auto position = _ftelli64(static_cast<FILE*>(file));
    if (position < 0) return MA_IO_ERROR;
    *cursor = position;
    return MA_SUCCESS;
}
ma_result sharedInfo(ma_vfs*, ma_vfs_file file, ma_file_info* info) {
    if (!info) return MA_INVALID_ARGS;
    struct _stat64 status{};
    if (_fstat64(_fileno(static_cast<FILE*>(file)), &status) != 0) return MA_IO_ERROR;
    info->sizeInBytes = static_cast<ma_uint64>(status.st_size);
    return MA_SUCCESS;
}
ma_vfs* sharedReadVfs() {
    static ma_vfs_callbacks callbacks{sharedOpen, sharedOpenW, sharedClose, sharedRead, nullptr, sharedSeek, sharedTell, sharedInfo};
    return &callbacks;
}
#endif

// AIFF's uncompressed PCM and AIFC's PCM/IEEE float forms. Chunks are skipped
// with seeks; opening a multi-gigabyte file does not read its sample data.
bool isAiff(const std::wstring& path) {
    const auto ext = extension(path);
    return ext == L".aif" || ext == L".aiff" || ext == L".aifc";
}

uint64_t bigEndian(const unsigned char* bytes, unsigned count) {
    uint64_t result = 0;
    for (unsigned i = 0; i < count; ++i) result = (result << 8) | bytes[i];
    return result;
}

bool seekFile(FILE* file, uint64_t offset) {
    if (offset > static_cast<uint64_t>(INT64_MAX)) return false;
#ifdef _WIN32
    return _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

uint64_t fileSize(FILE* file) {
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) return 0;
    const auto result = _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END) != 0) return 0;
    const auto result = ftello(file);
#endif
    return result > 0 ? static_cast<uint64_t>(result) : 0;
}

struct AiffDecoder {
    ma_data_source_base base{};
    FILE* file = nullptr;
    uint32_t channels = 0;
    uint32_t rate = 0;
    unsigned bits = 0;
    bool littleEndian = false;
    bool floatingPoint = false;
    uint64_t dataOffset = 0;
    uint64_t frames = 0;
    uint64_t cursor = 0;
    std::vector<unsigned char> buffer;
    ~AiffDecoder() { if (file) std::fclose(file); }

    bool parse() {
        const uint64_t size = fileSize(file);
        unsigned char header[12];
        if (size < 12 || !seekFile(file, 0) || std::fread(header, 1, 12, file) != 12 || std::memcmp(header, "FORM", 4) != 0) return false;
        const bool aifc = std::memcmp(header + 8, "AIFC", 4) == 0;
        if (!aifc && std::memcmp(header + 8, "AIFF", 4) != 0) return false;
        const uint64_t end = std::min<uint64_t>(size, 8 + bigEndian(header + 4, 4));
        bool gotCommon = false, gotSound = false;
        uint64_t soundBytes = 0;
        for (uint64_t offset = 12; offset <= end && end - offset >= 8;) {
            unsigned char chunk[8];
            if (!seekFile(file, offset) || std::fread(chunk, 1, 8, file) != 8) return false;
            const uint64_t bytes = bigEndian(chunk + 4, 4);
            const uint64_t start = offset + 8;
            if (bytes > end - start) return false;
            if (std::memcmp(chunk, "COMM", 4) == 0) {
                unsigned char common[22]{};
                const size_t need = aifc ? 22 : 18;
                if (bytes < need || std::fread(common, 1, need, file) != need) return false;
                channels = static_cast<uint32_t>(bigEndian(common, 2));
                frames = bigEndian(common + 2, 4);
                bits = static_cast<unsigned>(bigEndian(common + 6, 2));
                const int exponent = static_cast<int>(bigEndian(common + 8, 2));
                const uint64_t mantissa = bigEndian(common + 10, 8);
                if (exponent & 0x8000) return false;
                const long double frequency = std::ldexp(static_cast<long double>(mantissa), exponent - 16383 - 63);
                if (!std::isfinite(frequency) || frequency < 1 || frequency > 768000 || channels < 1 || channels > 32) return false;
                rate = static_cast<uint32_t>(std::llround(frequency));
                if (aifc) {
                    const auto* code = common + 18;
                    littleEndian = std::memcmp(code, "sowt", 4) == 0;
                    floatingPoint = std::memcmp(code, "fl32", 4) == 0 || std::memcmp(code, "FL32", 4) == 0 || std::memcmp(code, "fl64", 4) == 0 || std::memcmp(code, "FL64", 4) == 0;
                    if (!littleEndian && !floatingPoint && std::memcmp(code, "NONE", 4) != 0 && std::memcmp(code, "twos", 4) != 0) return false;
                    if (floatingPoint && bits != (code[2] == '3' ? 32u : 64u)) return false;
                }
                if (floatingPoint ? (bits != 32 && bits != 64) : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return false;
                gotCommon = true;
            } else if (std::memcmp(chunk, "SSND", 4) == 0) {
                unsigned char sound[8];
                if (bytes < 8 || std::fread(sound, 1, 8, file) != 8) return false;
                const uint64_t skip = bigEndian(sound, 4);
                if (skip > bytes - 8) return false;
                dataOffset = start + 8 + skip;
                soundBytes = bytes - 8 - skip;
                gotSound = true;
            }
            if (gotCommon && gotSound) break;
            offset = start + bytes + (bytes & 1);
        }
        if (!gotCommon || !gotSound) return false;
        const uint64_t frameBytes = channels * (bits / 8);
        frames = std::min(frames, soundBytes / frameBytes);
        if (!frames || !seekFile(file, dataOffset)) return false;
        buffer.resize(static_cast<size_t>(512 * frameBytes));
        return true;
    }
};

ma_result aiffRead(ma_data_source* source, void* output, ma_uint64 count, ma_uint64* readCount) {
    auto& self = *static_cast<AiffDecoder*>(source);
    if (readCount) *readCount = 0;
    auto* out = static_cast<float*>(output);
    const uint64_t wanted = std::min<uint64_t>(count, self.frames - self.cursor);
    const unsigned sampleBytes = self.bits / 8;
    const size_t frameBytes = static_cast<size_t>(sampleBytes) * self.channels;
    uint64_t done = 0;
    while (done < wanted) {
        const size_t batch = static_cast<size_t>(std::min<uint64_t>(512, wanted - done));
        const size_t received = std::fread(self.buffer.data(), frameBytes, batch, self.file);
        if (!received) break;
        if (out) for (size_t sample = 0; sample < received * self.channels; ++sample) {
            const auto* bytes = self.buffer.data() + sample * sampleBytes;
            uint64_t value = 0;
            if (self.littleEndian) for (unsigned byte = 0; byte < sampleBytes; ++byte) value |= static_cast<uint64_t>(bytes[byte]) << (8 * byte);
            else value = bigEndian(bytes, sampleBytes);
            float decoded;
            if (self.floatingPoint) {
                if (self.bits == 32) { const uint32_t word = static_cast<uint32_t>(value); std::memcpy(&decoded, &word, sizeof(decoded)); }
                else { double number; std::memcpy(&number, &value, sizeof(number)); decoded = static_cast<float>(number); }
                if (!std::isfinite(decoded)) decoded = 0;
            } else {
                const int64_t signedValue = (value & (uint64_t{1} << (self.bits - 1))) ? static_cast<int64_t>(value) - (int64_t{1} << self.bits) : static_cast<int64_t>(value);
                decoded = static_cast<float>(static_cast<double>(signedValue) / std::ldexp(1.0, self.bits - 1));
            }
            out[done * self.channels + sample] = decoded;
        }
        done += received;
        if (received < batch) break;
    }
    self.cursor += done;
    if (readCount) *readCount = done;
    if (done != wanted && std::ferror(self.file)) return MA_IO_ERROR;
    return done < count ? MA_AT_END : MA_SUCCESS;
}
ma_result aiffSeek(ma_data_source* source, ma_uint64 frame) {
    auto& self = *static_cast<AiffDecoder*>(source);
    if (frame > self.frames || !seekFile(self.file, self.dataOffset + frame * self.channels * (self.bits / 8))) return MA_BAD_SEEK;
    self.cursor = frame;
    return MA_SUCCESS;
}
ma_result aiffFormat(ma_data_source* source, ma_format* format, ma_uint32* channels, ma_uint32* rate, ma_channel* map, size_t mapCapacity) {
    const auto& self = *static_cast<AiffDecoder*>(source);
    if (format) *format = ma_format_f32;
    if (channels) *channels = self.channels;
    if (rate) *rate = self.rate;
    if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCapacity, self.channels);
    return MA_SUCCESS;
}
ma_result aiffCursor(ma_data_source* source, ma_uint64* cursor) { if (!cursor) return MA_INVALID_ARGS; *cursor = static_cast<AiffDecoder*>(source)->cursor; return MA_SUCCESS; }
ma_result aiffLength(ma_data_source* source, ma_uint64* length) { if (!length) return MA_INVALID_ARGS; *length = static_cast<AiffDecoder*>(source)->frames; return MA_SUCCESS; }
const ma_data_source_vtable kAiffSourceVtable{aiffRead, aiffSeek, aiffFormat, aiffCursor, aiffLength, nullptr, 0};

ma_result initAiff(FILE* file, ma_data_source** backend) {
    if (!file) return MA_DOES_NOT_EXIST;
    auto decoder = std::make_unique<AiffDecoder>();
    decoder->file = file;
    if (!decoder->parse()) return MA_INVALID_FILE;
    auto config = ma_data_source_config_init();
    config.vtable = &kAiffSourceVtable;
    const auto result = ma_data_source_init(&config, &decoder->base);
    if (result != MA_SUCCESS) return result;
    *backend = decoder.release();
    return MA_SUCCESS;
}
ma_result aiffInitFile(void*, const char* path, const ma_decoding_backend_config*, const ma_allocation_callbacks*, ma_data_source** backend) {
#ifdef _WIN32
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (count <= 0) return MA_INVALID_ARGS;
    std::vector<wchar_t> wide(static_cast<size_t>(count));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), count)) return MA_INVALID_ARGS;
    return initAiff(openFile(wide.data()), backend);
#else
    return initAiff(std::fopen(path, "rb"), backend);
#endif
}
ma_result aiffInitFileW(void*, const wchar_t* path, const ma_decoding_backend_config*, const ma_allocation_callbacks*, ma_data_source** backend) { return initAiff(openFile(path), backend); }
void aiffUninit(void*, ma_data_source* backend, const ma_allocation_callbacks*) { auto* decoder = static_cast<AiffDecoder*>(backend); ma_data_source_uninit(&decoder->base); delete decoder; }
ma_decoding_backend_vtable kAiffBackend{nullptr, aiffInitFile, aiffInitFileW, nullptr, aiffUninit};
ma_decoding_backend_vtable** aiffBackends() { static ma_decoding_backend_vtable* table[]{&kAiffBackend}; return table; }

// miniaudio's MP3 length query may otherwise scan every frame. Only allow its
// constant-time path when Xing/VBRI metadata has already supplied the count.
double cheapDecoderDuration(ma_decoder& decoder, ma_encoding_format format) {
    if (format == ma_encoding_format_mp3) {
        const auto* mp3 = reinterpret_cast<const ma_mp3*>(decoder.pBackend);
        if (!mp3 || mp3->dr.totalPCMFrameCount == MA_UINT64_MAX) return -1.0;
    }
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS || frames == 0 || !decoder.outputSampleRate) return -1.0;
    return static_cast<double>(frames) / decoder.outputSampleRate;
}

constexpr size_t kWaveformBins = 512;
constexpr size_t kWaveformWorkBins = 2048;
constexpr ma_uint64 kWaveformDecodeFrames = 4096;

struct DecoderScope {
    ma_decoder decoder{};
    bool open = false;
    ~DecoderScope() { if (open) ma_decoder_uninit(&decoder); }
};

// Equal-width peak bins compact pairwise as a stream grows. This bounds memory
// even when the file has no duration metadata, without dropping brief impulses.
struct WaveformPeaks {
    std::array<float, kWaveformWorkBins> bins{};
    size_t count = 0;
    uint64_t width = 1;
    uint64_t partialFrames = 0;
    uint64_t frames = 0;
    float partialPeak = 0;

    void add(float peak) {
        if (count == bins.size()) {
            for (size_t bin = 0; bin < bins.size() / 2; ++bin) bins[bin] = std::max(bins[bin * 2], bins[bin * 2 + 1]);
            count /= 2;
            width *= 2;
        }
        partialPeak = std::max(partialPeak, peak);
        ++frames;
        if (++partialFrames == width) {
            bins[count++] = partialPeak;
            partialFrames = 0;
            partialPeak = 0;
        }
    }

    std::vector<float> finish() const {
        if (!frames) return {};
        std::vector<float> result(kWaveformBins, 0);
        const size_t available = count + (partialFrames ? 1 : 0);
        for (size_t bin = 0; bin < available; ++bin) {
            const uint64_t start = static_cast<uint64_t>(bin) * width;
            const uint64_t end = std::min<uint64_t>(start + width, frames);
            const size_t first = std::min<size_t>(kWaveformBins - 1, static_cast<size_t>(static_cast<long double>(start) * kWaveformBins / frames));
            const size_t last = std::min<size_t>(kWaveformBins - 1, static_cast<size_t>(std::ceil(static_cast<long double>(end) * kWaveformBins / frames) - 1));
            const float peak = bin < count ? bins[bin] : partialPeak;
            for (size_t target = first; target <= last; ++target) result[target] = std::max(result[target], peak);
        }
        return result;
    }
};

struct WaveformFileStamp {
    std::wstring path;
    uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    bool valid = false;
    bool operator==(const WaveformFileStamp& other) const {
        return valid && other.valid && path == other.path && size == other.size && modified == other.modified;
    }
};

WaveformFileStamp waveformStamp(const std::wstring& path) {
    WaveformFileStamp result;
    result.path = path;
    std::error_code error;
    result.size = std::filesystem::file_size(std::filesystem::path(path), error);
    if (error) return result;
    result.modified = std::filesystem::last_write_time(std::filesystem::path(path), error);
    result.valid = !error;
    return result;
}

template<class IsCancelled>
WaveformSnapshot scanWaveform(const std::wstring& path, IsCancelled isCancelled) {
    WaveformSnapshot result;
    result.path = path;
    if (isCancelled()) return result;
    DecoderScope source;
    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (openDecoder(path, config, &source.decoder) != MA_SUCCESS) return result;
    source.open = true;
    if (isCancelled() || !source.decoder.outputChannels || !source.decoder.outputSampleRate) return result;
    const auto channels = source.decoder.outputChannels;
    // Keep the original sample rate and every channel. Downmixing or resampling
    // before measuring peaks would hide opposite-phase audio and short hits.
    std::vector<float> decoded(static_cast<size_t>(kWaveformDecodeFrames) * channels);
    WaveformPeaks peaks;
    while (!isCancelled()) {
        ma_uint64 count = 0;
        const auto readResult = ma_decoder_read_pcm_frames(&source.decoder, decoded.data(), kWaveformDecodeFrames, &count);
        if (isCancelled()) return result;
        for (ma_uint64 frame = 0; frame < count; ++frame) {
            float peak = 0;
            for (ma_uint32 channel = 0; channel < channels; ++channel) {
                const float sample = decoded[static_cast<size_t>(frame) * channels + channel];
                if (std::isfinite(sample)) peak = std::max(peak, std::min(1.0f, std::abs(sample)));
            }
            peaks.add(peak);
        }
        if (readResult == MA_AT_END || (readResult == MA_SUCCESS && count == 0)) {
            result.peaks = peaks.finish();
            result.durationSeconds = static_cast<double>(peaks.frames) / source.decoder.outputSampleRate;
            return result;
        }
        if (readResult != MA_SUCCESS) return result;
    }
    return result;
}

struct Stream {
    explicit Stream(uint64_t serialIn) : serial(serialIn) {}
    const uint64_t serial;
    std::array<float, kBufferFrames * kChannels> samples{};
    std::atomic<uint64_t> read{0};
    std::atomic<uint64_t> write{0};
    std::atomic<bool> eof{false};
    std::atomic<bool> ended{false};
};
} // namespace

struct AudioEngine::Impl {
    ma_context context{};
    ma_device device{};
    bool contextReady = false;
    bool deviceReady = false;
    std::atomic<bool> initialized{false};
    std::atomic<bool> quitting{false};
    std::atomic<bool> paused{false};
    std::atomic<float> volume{0.8f};
    std::atomic<uint64_t> requested{0};
    std::atomic<uint64_t> firstBufferSerial{0};
    std::atomic<uint64_t> failedPlaybackSerial{0};

    // The audio callback never takes a mutex, opens a file, or runs a decoder.
    // A hazard pointer lets the worker retire old rings without blocking audio.
    std::atomic<Stream*> current{nullptr};
    std::atomic<Stream*> callbackHazard{nullptr};
    std::atomic<Stream*> snapshotHazard{nullptr};
    std::vector<Stream*> retired;

    mutable std::mutex stateMutex;
    AudioSnapshot state;
    std::function<void()> notify;
    std::mutex requestMutex;
    std::condition_variable wake;
    std::wstring pendingPath;
    std::thread worker;

    // Independent from all playback locks, I/O, decoders, and callback buffers.
    mutable std::mutex waveformMutex;
    std::condition_variable waveformWake;
    WaveformSnapshot waveform;
    uint64_t waveformSerial = 0;
    std::chrono::steady_clock::time_point waveformRequestedAt;
    std::thread waveformWorker;

    void signal() {
        std::function<void()> callback;
        { std::lock_guard<std::mutex> lock(stateMutex); callback = notify; }
        if (callback) callback();
    }

    void update(uint64_t serial, AudioState newState, const std::wstring& error = L"", double duration = -2.0) {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (requested.load() != serial) return;
            state.state = (newState == AudioState::Playing || newState == AudioState::Paused) ? (paused.load() ? AudioState::Paused : AudioState::Playing) : newState;
            state.error = error;
            if (duration != -2.0) state.durationSeconds = duration;
        }
        if (newState == AudioState::Error) {
            failedPlaybackSerial.store(serial);
            waveformWake.notify_one();
        }
        signal();
    }

    void scheduleWaveform(uint64_t serial, const std::wstring& path) {
        {
            std::lock_guard<std::mutex> lock(waveformMutex);
            if (requested.load() != serial) return;
            waveformSerial = serial;
            waveformRequestedAt = std::chrono::steady_clock::now();
            waveform = {path, {}, -1.0, !path.empty()};
        }
        waveformWake.notify_one();
    }

    void publishWaveform(uint64_t serial, WaveformSnapshot result) {
        const double decodedDuration = result.durationSeconds;
        {
            std::lock_guard<std::mutex> lock(waveformMutex);
            if (quitting.load() || requested.load() != serial || waveformSerial != serial) return;
            waveform = std::move(result);
        }
        if (decodedDuration > 0) {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (requested.load() == serial && state.durationSeconds < 0) state.durationSeconds = decodedDuration;
        }
        signal();
    }

    void runWaveforms() {
#ifdef _WIN32
        // Windows background mode also lowers I/O priority. Playback retains
        // its normal worker priority and the output device's realtime callback.
        if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
        struct CacheEntry { WaveformFileStamp stamp; WaveformSnapshot result; };
        std::deque<CacheEntry> cache;
        uint64_t handledSerial = 0;
        while (!quitting.load()) {
            uint64_t serial;
            std::wstring path;
            {
                std::unique_lock<std::mutex> lock(waveformMutex);
                waveformWake.wait(lock, [&] { return quitting.load() || waveformSerial != handledSerial; });
                if (quitting.load()) break;
                serial = waveformSerial;
                path = waveform.path;
                if (path.empty()) { handledSerial = serial; continue; }
                const auto due = waveformRequestedAt + std::chrono::milliseconds(120);
                if (waveformWake.wait_until(lock, due, [&] { return quitting.load() || waveformSerial != serial; })) continue;
                // Playback publishes this gate without taking waveformMutex.
                // A bounded wait also covers a notification arriving just before
                // the condition variable sleeps, without coupling audio locks.
                while (!quitting.load() && waveformSerial == serial && requested.load() == serial && firstBufferSerial.load() != serial && failedPlaybackSerial.load() != serial) waveformWake.wait_for(lock, std::chrono::milliseconds(20));
                if (quitting.load()) break;
                if (waveformSerial != serial) continue;
                handledSerial = serial;
                if (requested.load() != serial) continue;
            }
            const auto cancelled = [&] { return quitting.load() || requested.load() != serial; };
            if (failedPlaybackSerial.load() == serial) { publishWaveform(serial, {path, {}, -1.0, false}); continue; }
            if (cancelled()) continue;
            const auto stamp = waveformStamp(path);
            if (cancelled()) continue;
            const auto cached = std::find_if(cache.begin(), cache.end(), [&](const CacheEntry& item) { return item.stamp == stamp; });
            if (cached != cache.end()) {
                CacheEntry hit = std::move(*cached);
                cache.erase(cached);
                publishWaveform(serial, hit.result);
                cache.push_front(std::move(hit));
                continue;
            }
            auto result = scanWaveform(path, cancelled);
            if (cancelled()) continue;
            if (!result.peaks.empty() && stamp.valid) {
                cache.push_front({stamp, result});
                if (cache.size() > 8) cache.pop_back();
            }
            publishWaveform(serial, std::move(result));
        }
    }

    void retireCurrent(Stream* replacement) {
        if (auto* old = current.exchange(replacement)) retired.push_back(old);
        collectRetired();
    }

    void collectRetired() {
        const auto* held = callbackHazard.load();
        const auto* inspected = snapshotHazard.load();
        retired.erase(std::remove_if(retired.begin(), retired.end(), [held, inspected](Stream* stream) {
            if (stream == held || stream == inspected) return false;
            delete stream;
            return true;
        }), retired.end());
    }

    static void audioCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
        auto& self = *static_cast<Impl*>(device->pUserData);
        auto* out = static_cast<float*>(output);
        std::fill_n(out, static_cast<size_t>(frameCount) * kChannels, 0.0f);
        Stream* stream;
        do {
            stream = self.current.load();
            self.callbackHazard.store(stream);
        } while (stream != self.current.load());
        if (stream && stream->serial == self.requested.load() && !self.paused.load()) {
            const uint64_t read = stream->read.load(std::memory_order_relaxed);
            const uint64_t written = stream->write.load(std::memory_order_acquire);
            const uint64_t count = std::min<uint64_t>(frameCount, written - read);
            const float gain = self.volume.load(std::memory_order_relaxed);
            for (uint64_t frame = 0; frame < count; ++frame) {
                const auto offset = ((read + frame) & (kBufferFrames - 1)) * kChannels;
                out[frame * 2] = stream->samples[offset] * gain;
                out[frame * 2 + 1] = stream->samples[offset + 1] * gain;
            }
            stream->read.store(read + count, std::memory_order_release);
            if (stream->eof.load(std::memory_order_acquire) && read + count == stream->write.load(std::memory_order_acquire)) stream->ended.store(true);
        }
        self.callbackHazard.store(nullptr);
    }

    void run() {
        uint64_t activeSerial = 0;
        ma_decoder decoder{};
        bool decoderOpen = false;
        Stream* active = nullptr;
        std::array<float, kDecodeFrames * kChannels> decoded{};
        while (!quitting.load()) {
            const auto serial = requested.load();
            if (serial != activeSerial) {
                retireCurrent(nullptr);
                active = nullptr;
                if (decoderOpen) { ma_decoder_uninit(&decoder); decoderOpen = false; }
                activeSerial = serial;
                std::wstring path;
                {
                    std::lock_guard<std::mutex> lock(requestMutex);
                    if (requested.load() != serial) continue;
                    path = pendingPath;
                }
                if (path.empty()) continue;
                auto config = ma_decoder_config_init(ma_format_f32, kChannels, kRate);
                const auto result = openDecoder(path, config, &decoder);
                if (result != MA_SUCCESS) {
                    if (requested.load() == serial) update(serial, AudioState::Error, L"Cannot play this file (unsupported or damaged audio).");
                    continue;
                }
                decoderOpen = true;
                if (requested.load() != serial || quitting.load()) continue;
                active = new Stream(serial);
                // Publication precedes filling, so the first available block can
                // play immediately; there is no full-buffer pre-roll requirement.
                retireCurrent(active);
                const double duration = cheapDecoderDuration(decoder, encoding(path));
                update(serial, paused.load() ? AudioState::Paused : AudioState::Playing, L"", duration);
            }
            if (active && active->serial == requested.load()) {
                if (active->ended.load()) {
                    update(activeSerial, AudioState::Finished);
                    // A finished stream remains selected; Space starts it again.
                    active = nullptr;
                    if (decoderOpen) { ma_decoder_uninit(&decoder); decoderOpen = false; }
                    continue;
                }
                const uint64_t read = active->read.load(std::memory_order_acquire);
                const uint64_t write = active->write.load(std::memory_order_relaxed);
                if (!active->eof.load() && write - read <= kBufferFrames - kDecodeFrames) {
                    ma_uint64 count = 0;
                    const auto result = ma_decoder_read_pcm_frames(&decoder, decoded.data(), kDecodeFrames, &count);
                    if (requested.load() != activeSerial) continue;
                    for (uint64_t frame = 0; frame < count; ++frame) {
                        const auto offset = ((write + frame) & (kBufferFrames - 1)) * kChannels;
                        active->samples[offset] = decoded[frame * 2];
                        active->samples[offset + 1] = decoded[frame * 2 + 1];
                    }
                    active->write.store(write + count, std::memory_order_release);
                    if (count && firstBufferSerial.load() != activeSerial) {
                        firstBufferSerial.store(activeSerial);
                        waveformWake.notify_one();
                    }
                    if (result == MA_AT_END || (result == MA_SUCCESS && count == 0)) {
                        active->eof.store(true, std::memory_order_release);
                        if (!write && !count) { failedPlaybackSerial.store(activeSerial); waveformWake.notify_one(); }
                    }
                    else if (result != MA_SUCCESS) {
                        retireCurrent(nullptr);
                        active = nullptr;
                        ma_decoder_uninit(&decoder);
                        decoderOpen = false;
                        update(activeSerial, AudioState::Error, L"The audio file could not be decoded.");
                    }
                    continue;
                }
            }
            collectRetired();
            std::unique_lock<std::mutex> lock(requestMutex);
            wake.wait_for(lock, std::chrono::milliseconds(3), [&] { return quitting.load() || requested.load() != activeSerial; });
        }
        retireCurrent(nullptr);
        if (decoderOpen) ma_decoder_uninit(&decoder);
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::initialize(bool silentTestDevice) {
    auto& self = *impl_;
    if (self.initialized.load()) return true;
    ma_result result;
    if (silentTestDevice) {
        const ma_backend backend = ma_backend_null;
        result = ma_context_init(&backend, 1, nullptr, &self.context);
    } else result = ma_context_init(nullptr, 0, nullptr, &self.context);
    if (result != MA_SUCCESS) {
        std::lock_guard<std::mutex> lock(self.stateMutex);
        self.state.state = AudioState::Error;
        self.state.error = L"No audio output is available.";
        return false;
    }
    self.contextReady = true;
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = kRate;
    config.periodSizeInMilliseconds = 5;
    config.periods = 2;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = Impl::audioCallback;
    config.pUserData = &self;
    result = ma_device_init(&self.context, &config, &self.device);
    if (result != MA_SUCCESS) {
        ma_context_uninit(&self.context);
        self.contextReady = false;
        std::lock_guard<std::mutex> lock(self.stateMutex);
        self.state.state = AudioState::Error;
        self.state.error = L"Cannot open the default audio output.";
        return false;
    }
    self.deviceReady = true;
    if (ma_device_start(&self.device) != MA_SUCCESS) {
        shutdown();
        std::lock_guard<std::mutex> lock(self.stateMutex);
        self.state.state = AudioState::Error;
        self.state.error = L"Cannot start audio playback.";
        return false;
    }
    self.quitting.store(false);
    self.initialized.store(true);
    self.worker = std::thread([&self] { self.run(); });
    self.waveformWorker = std::thread([&self] { self.runWaveforms(); });
    return true;
}

void AudioEngine::shutdown() {
    auto& self = *impl_;
    self.quitting.store(true);
    {
        std::lock_guard<std::mutex> lock(self.requestMutex);
        self.pendingPath.clear();
        self.requested.fetch_add(1);
    }
    {
        std::lock_guard<std::mutex> lock(self.waveformMutex);
        self.waveformSerial = self.requested.load();
        self.waveform = {};
    }
    self.wake.notify_all();
    self.waveformWake.notify_all();
    if (self.worker.joinable()) self.worker.join();
    if (self.waveformWorker.joinable()) self.waveformWorker.join();
    if (self.deviceReady) { ma_device_uninit(&self.device); self.deviceReady = false; }
    if (self.contextReady) { ma_context_uninit(&self.context); self.contextReady = false; }
    self.callbackHazard.store(nullptr);
    self.collectRetired();
    self.initialized.store(false);
}

void AudioEngine::setNotify(std::function<void()> notify) {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->notify = std::move(notify);
}

void AudioEngine::play(const std::wstring& path) {
    auto& self = *impl_;
    if (!self.initialized.load()) return;
    uint64_t serial;
    {
        std::lock_guard<std::mutex> lock(self.requestMutex);
        self.pendingPath = path;
        serial = self.requested.fetch_add(1) + 1; // Silences the previous ring on the next device block.
        self.paused.store(false);
        std::lock_guard<std::mutex> stateLock(self.stateMutex);
        self.state = {path, L"", AudioState::Loading, -1.0, 0.0};
    }
    self.wake.notify_one();
    self.scheduleWaveform(serial, path);
    self.signal();
}

void AudioEngine::togglePause() {
    auto& self = *impl_;
    const auto snap = snapshot();
    if ((snap.state == AudioState::Finished || snap.state == AudioState::Stopped || snap.state == AudioState::Error) && !snap.path.empty()) { play(snap.path); return; }
    if (snap.state != AudioState::Playing && snap.state != AudioState::Paused && snap.state != AudioState::Loading) return;
    const bool pauseNow = !self.paused.load();
    self.paused.store(pauseNow);
    self.update(self.requested.load(), pauseNow ? AudioState::Paused : AudioState::Playing);
    self.wake.notify_one();
}

void AudioEngine::stop() {
    auto& self = *impl_;
    uint64_t serial;
    {
        std::lock_guard<std::mutex> lock(self.requestMutex);
        self.pendingPath.clear();
        serial = self.requested.fetch_add(1) + 1;
        self.paused.store(false);
        std::lock_guard<std::mutex> stateLock(self.stateMutex);
        self.state.state = AudioState::Stopped;
        self.state.error.clear();
        self.state.positionSeconds = 0;
    }
    self.wake.notify_one();
    self.scheduleWaveform(serial, L"");
    self.signal();
}

void AudioEngine::setVolume(float volume) { impl_->volume.store(std::clamp(volume, 0.0f, 1.0f)); }

AudioSnapshot AudioEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto result = impl_->state;
    Stream* stream;
    do {
        stream = impl_->current.load();
        impl_->snapshotHazard.store(stream);
    } while (stream != impl_->current.load());
    result.positionSeconds = (stream && stream->serial == impl_->requested.load()) ? static_cast<double>(stream->read.load()) / kRate : 0.0;
    impl_->snapshotHazard.store(nullptr);
    return result;
}

WaveformSnapshot AudioEngine::waveformSnapshot() const {
    std::lock_guard<std::mutex> lock(impl_->waveformMutex);
    return impl_->waveform;
}

double probeDuration(const std::wstring& path) {
    const auto format = encoding(path);
    if (format == ma_encoding_format_unknown && !isAiff(path)) return -1.0;
    if (format == ma_encoding_format_vorbis) {
        FILE* file = openFile(path);
        if (!file) return -1.0;
        int error = 0;
        auto* vorbis = stb_vorbis_open_file(file, 1, &error, nullptr);
        if (!vorbis) return -1.0; // stb_vorbis owns/closes file even when initialization fails.
        const double result = stb_vorbis_stream_length_in_seconds(vorbis);
        stb_vorbis_close(vorbis);
        return result > 0 ? result : -1.0;
    }
    ma_decoder decoder{};
    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (openDecoder(path, config, &decoder) != MA_SUCCESS) return -1.0;
    const double duration = cheapDecoderDuration(decoder, format);
    ma_decoder_uninit(&decoder);
    return duration;
}

} // namespace abser

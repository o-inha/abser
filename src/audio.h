#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace abser {

enum class AudioState { Stopped, Loading, Playing, Paused, Finished, Error };

struct AudioSnapshot {
    std::wstring path;
    std::wstring error;
    AudioState state = AudioState::Stopped;
    double durationSeconds = -1.0;
    double positionSeconds = 0.0;
};

struct WaveformSnapshot {
    std::wstring path;
    // 512 linear, absolute peak amplitudes, from 0 to 1. Mirror around the
    // centre line when drawing. Empty while loading or for unreadable files.
    std::vector<float> peaks;
    double durationSeconds = -1.0;
    bool loading = false;
};

// One persistent output device; opening and decoding files happens on a worker.
// Public controls return immediately even when a slow file is being opened.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // The silent test device runs the same decoding/buffering/callback pipeline.
    bool initialize(bool silentTestDevice = false);
    void shutdown();
    void setNotify(std::function<void()> notify);
    void play(const std::wstring& path);
    void togglePause();
    // Idempotent: pauses active/loading playback and never restarts a file.
    void pause();
    void stop();
    void setVolume(float volume);
    AudioSnapshot snapshot() const;
    // Copies a tiny completed waveform; never performs file access or decoding.
    WaveformSnapshot waveformSnapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads header/seek metadata, never decodes or scans an entire MP3. Returns -1
// for invalid files and formats whose duration is not cheaply available.
double probeDuration(const std::wstring& path);

} // namespace abser

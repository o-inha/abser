#include "audio.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using abser::AudioState;

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void word(std::ofstream& out, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 255));
}
void makeWav(const std::filesystem::path& file, double seconds, double frequency) {
    std::ofstream out(file, std::ios::binary);
    const uint32_t frames = static_cast<uint32_t>(48000 * seconds);
    out.write("RIFF", 4); word(out, 36 + frames * 2, 4); out.write("WAVEfmt ", 8);
    word(out, 16, 4); word(out, 1, 2); word(out, 1, 2); word(out, 48000, 4);
    word(out, 96000, 4); word(out, 2, 2); word(out, 16, 2); out.write("data", 4); word(out, frames * 2, 4);
    for (uint32_t i = 0; i < frames; ++i) word(out, static_cast<uint16_t>(static_cast<int16_t>(std::sin(i * frequency * 6.283185307179586 / 48000) * 4000)), 2);
}
void bigWord(std::ofstream& out, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.put(static_cast<char>((value >> (8 * i)) & 255));
}
void makeAiff(const std::filesystem::path& file, unsigned bits, const std::string& compression = "", bool soundFirst = false) {
    std::ofstream out(file, std::ios::binary);
    const uint32_t frames = 4800;
    const bool aifc = !compression.empty();
    const bool floating = compression == "fl32" || compression == "fl64";
    const bool little = compression == "sowt";
    const uint32_t commonBytes = aifc ? 24 : 18;
    const uint32_t soundBytes = 8 + frames * (bits / 8);
    out.write("FORM", 4); bigWord(out, 4 + (aifc ? 12 : 0) + 8 + commonBytes + 8 + soundBytes, 4); out.write(aifc ? "AIFC" : "AIFF", 4);
    if (aifc) { out.write("FVER", 4); bigWord(out, 4, 4); bigWord(out, 0xA2805140, 4); }
    auto common = [&] {
        out.write("COMM", 4); bigWord(out, commonBytes, 4); bigWord(out, 1, 2); bigWord(out, frames, 4); bigWord(out, bits, 2);
        bigWord(out, 0x400e, 2); bigWord(out, UINT64_C(0xbb80000000000000), 8); // 48000 in IEEE extended80
        if (aifc) { out.write(compression.c_str(), 4); bigWord(out, 0, 2); }
    };
    auto sound = [&] {
        out.write("SSND", 4); bigWord(out, soundBytes, 4); bigWord(out, 0, 8);
        for (uint32_t i = 0; i < frames; ++i) {
            const double sample = std::sin(i * 440 * 6.283185307179586 / 48000) * .125;
            uint64_t value = 0;
            if (floating) {
                if (bits == 32) { const float number = static_cast<float>(sample); uint32_t raw; std::memcpy(&raw, &number, 4); value = raw; }
                else std::memcpy(&value, &sample, 8);
            } else value = static_cast<uint64_t>(static_cast<int64_t>(sample * std::ldexp(1.0, bits - 1)));
            if (little) word(out, static_cast<uint32_t>(value), bits / 8);
            else bigWord(out, value, bits / 8);
        }
    };
    if (soundFirst) { sound(); common(); } else { common(); sound(); }
}
template<class Predicate> bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do { if (predicate()) return true; std::this_thread::sleep_for(3ms); } while (std::chrono::steady_clock::now() < end);
    return false;
}
}

int main() {
    try {
        const auto dir = std::filesystem::temp_directory_path() / ("abser-audio-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        const auto first = dir / "first.wav";
        const auto second = dir / "second.wav";
        const auto shortFile = dir / "short.wav";
        const auto broken = dir / "broken.wav";
        makeWav(first, 3, 220); makeWav(second, 3, 660); makeWav(shortFile, .08, 440);
        { std::ofstream out(broken); out << "This is not audio"; }
        check(std::abs(abser::probeDuration(first.wstring()) - 3.0) < .001, "WAV duration must be read accurately");
        check(abser::probeDuration(broken.wstring()) < 0, "Broken file must have unknown duration");

        abser::AudioEngine audio;
        check(audio.initialize(true), "Silent test output must initialize");
        audio.play(first.wstring());
        check(eventually([&] { auto s = audio.snapshot(); return s.state == AudioState::Playing && s.positionSeconds > .025; }), "WAV must start and advance");
        audio.togglePause();
        check(audio.snapshot().state == AudioState::Paused, "Space pause must change state");
        std::this_thread::sleep_for(20ms);
        const double pausedAt = audio.snapshot().positionSeconds;
        std::this_thread::sleep_for(45ms);
        check(std::abs(audio.snapshot().positionSeconds - pausedAt) < .001, "Paused stream must not advance");
        audio.togglePause();
        check(eventually([&] { return audio.snapshot().positionSeconds > pausedAt + .02; }), "Resume must continue at paused position");

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 500; ++i) audio.play(i % 2 ? first.wstring() : second.wstring());
        audio.play(second.wstring());
        check(std::chrono::steady_clock::now() - start < 500ms, "Rapid selection must not wait for decoder work");
        check(eventually([&] { auto s = audio.snapshot(); return s.path == second.wstring() && s.state == AudioState::Playing && s.positionSeconds > .015 && s.positionSeconds < .5; }), "Latest selection must win and start at the beginning");
        for (int i = 0; i < 100; ++i) { audio.play(first.wstring()); audio.stop(); }
        audio.stop();
        std::this_thread::sleep_for(25ms);
        check(audio.snapshot().state == AudioState::Stopped && audio.snapshot().positionSeconds < .001, "Stop must reset playback position");
        audio.togglePause();
        check(eventually([&] { return audio.snapshot().state == AudioState::Playing; }), "Space after Stop must restart selected file");
        audio.play(shortFile.wstring());
        check(eventually([&] { return audio.snapshot().state == AudioState::Finished; }), "Short file must reach EOF without auto-playing another file");
        check(std::abs(audio.snapshot().positionSeconds - .08) < .003, "EOF must preserve actual duration");
        audio.togglePause();
        check(eventually([&] { return audio.snapshot().state == AudioState::Playing; }), "Space at EOF must restart");
        audio.play(broken.wstring());
        check(eventually([&] { return audio.snapshot().state == AudioState::Error; }), "Damaged file must report error");
        audio.play(first.wstring());
        check(eventually([&] { return audio.snapshot().state == AudioState::Playing; }), "Good file must play after damaged file");
        std::thread metadata([&] { for (int i = 0; i < 100; ++i) check(std::abs(abser::probeDuration(second.wstring()) - 3.0) < .001, "Concurrent metadata probing must not disturb playback"); });
        metadata.join();
        check(audio.snapshot().state == AudioState::Playing, "Concurrent metadata must preserve playback");
        for (const auto& spec : {std::pair<unsigned, std::string>{8, ""}, {16, ""}, {24, ""}, {32, ""}, {16, "sowt"}, {32, "fl32"}, {64, "fl64"}}) {
            const auto aiff = dir / ("sample" + std::to_string(spec.first) + spec.second + (spec.second.empty() ? ".aiff" : ".aifc"));
            makeAiff(aiff, spec.first, spec.second, spec.first == 24);
            check(std::abs(abser::probeDuration(aiff.wstring()) - .1) < .001, "AIFF/AIFC header duration must be accurate");
            audio.play(aiff.wstring());
            check(eventually([&] { return audio.snapshot().state == AudioState::Finished || audio.snapshot().state == AudioState::Error; }), "AIFF playback must finish");
            check(audio.snapshot().state == AudioState::Finished, "AIFF PCM, sowt, and float forms must decode");
            check(std::abs(audio.snapshot().positionSeconds - .1) < .003, "AIFF rendered duration must agree with header");
        }
#ifdef _WIN32
        const auto unicode = dir / std::filesystem::path(L"\u65e5\u672c\u8a9e-\u00e4\u00f6.wav");
        makeWav(unicode, .08, 440);
        check(std::abs(abser::probeDuration(unicode.wstring()) - .08) < .001, "Unicode file path duration must work");
        audio.play(unicode.wstring());
        check(eventually([&] { return audio.snapshot().state == AudioState::Finished; }), "Unicode file path playback must work");
#endif
        audio.shutdown();
        std::filesystem::remove_all(dir);
        std::cout << "Audio tests passed: duration, playback, pause/resume, rapid switching, stop/reset, EOF, malformed file recovery, AIFF/AIFC PCM/float/little-endian.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

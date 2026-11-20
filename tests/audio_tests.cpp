#include "audio.h"

#include <algorithm>
#include <array>
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
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

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
void makePeakWav(const std::filesystem::path& file, uint32_t frames, uint16_t channels, const std::vector<std::pair<uint32_t, int16_t>>& impulses, bool oppositeChannels = false) {
    std::ofstream out(file, std::ios::binary);
    const uint32_t dataBytes = frames * channels * 2;
    out.write("RIFF", 4); word(out, 36 + dataBytes, 4); out.write("WAVEfmt ", 8);
    word(out, 16, 4); word(out, 1, 2); word(out, channels, 2); word(out, 48000, 4);
    word(out, 48000 * channels * 2, 4); word(out, channels * 2, 2); word(out, 16, 2);
    out.write("data", 4); word(out, dataBytes, 4);
    // Sparse silence keeps long-file cancellation fixtures inexpensive.
    if (dataBytes) { out.seekp(44 + static_cast<std::streamoff>(dataBytes) - 1); out.put(0); }
    for (const auto& impulse : impulses) {
        out.seekp(44 + static_cast<std::streamoff>(impulse.first) * channels * 2);
        for (uint16_t channel = 0; channel < channels; ++channel) word(out, static_cast<uint16_t>(oppositeChannels && channel % 2 ? -impulse.second : impulse.second), 2);
    }
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
// Small synthetic sine fixtures; tests need no external audio conversion tool.
void writeBase64(const std::filesystem::path& file, const char* encoded) {
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::ofstream out(file, std::ios::binary);
    uint32_t buffer = 0;
    unsigned bits = 0;
    for (const char* current = encoded; *current && *current != '='; ++current) {
        const char* value = std::strchr(alphabet, *current);
        if (!value) continue;
        buffer = (buffer << 6) | static_cast<uint32_t>(value - alphabet);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.put(static_cast<char>((buffer >> bits) & 255)); }
    }
}
const char* kFlacFixture =
    "ZkxhQwAAACISABIAAAR+AATaCsRA8AAAInRkuA5Nycm9XKMIppQqHAbshAAALAwAAABMYXZmNjEuNy4xMDMBAAAAFAAAAGVuY29k"
    "ZXI9TGF2ZjYxLjcuMTAz//hZCAA8TgAAAQAB/wL9A/gE7gXgBszmp64XhfObtV9kv0AgK8P/gAySeQzJJJPDMzmQmhnJSZkk0zJ8"
    "/JPJ55DPJyeScknmTkyaQyTmTnMzMmTmTMmfJNwyfJPkzMzOSHkPkyGkyZzJJ8zyT8nzmZJ5JzlIQ2ZOGZJyTJMzTJJwmQ5hOTKZ"
    "NmZ5nyUnJ5Mmc+STknMmcknOcmTMznMk0kzyZMmc5nk5zJMpDMpkzJD5JnJJkkk5kmSeZyfJ8k+cmeZzyZPJOZmSZJzCZMkyTklO"
    "ZM05uQ5nMk+SSdITJzk8JnOZzPJzJJnMhmTyTMkknJZM5k5PM+ZJzJ+QyTyTMyHknhPMySeZJnM/JZnmeczOSeQyckyZJmZIZZM5"
    "PDyZ5zDPMzJ4ZknJOZkzzJNJJPhJ5JMzkkkmyTMmZpk8ybJPmSemSTMzOYecyT5JJ4TM+cmchPwzyZzyZKQ8yZpJPIbJNhOZMmZk"
    "k85Jk+SeTMkzk5k8OkM5OSSpDOZmeGeQ+QmchknJhk5MknnPnJPMkzk5TJzJOmTITyZPJOZk5DOYTknOSTM5JnOTmeTJmUnhhnk5"
    "MwzMzkznCUmZh5nMzJvJknnOSWTkpk88yZmSckkmmSZyTLJJyYZh/IfyzOZmfJJLJ0kmcwzMkyZDPJJcOZzMP5M0kzmTkzLMPzJZ"
    "MnJkyc5nJk0nkzkmaZM6ZM8mST5nOZOTkOWZ5MzOSSZJM5mZw/IeTJkpJz5zmZmeTzJs5Jkk5mTJ5JM5IT5OQzQzMyTOZmcnymTT"
    "JJ6EmTkmknPOZJhyZkmcheSeSZzSSfJmZnKZzJyySeQzJJJPDMzmQmhnJSZkk0zJ8/JPJ55DPJyUMk5JPMnJk0hknMnOZmZMnMmZ"
    "M+Sbhk+SfJmZmckPIfJkNJkzmST5nkn5PnMyTyTnKQhsycMyTkmSZmmSThMhzCcmUybMzzPkpOTyZM58knJOZM5JOc5MmZnOYekm"
    "eTJkznM8nOZJlIZlMmZIfJM5JMkknMkyTzOTlJ+T5yZ5nPJk8k5mZJknMJkyTJOSU5kzTm5DmcyT5JJ0hMnOTwmc5nM8nMkmcyGZ"
    "PJMySSclkzmTk8z5knMn5DJPJMzIeSeE8zJJ5kmcz8lmeZ5zM5J5DJyTJkmZkhlkzk8PJnnMM8zMnhmSck5mTPMk0kk+EnkkzOSS"
    "SbJMyZmmTzJsk+ZJ6ZJMzM5h5zJPkknhMnyHJnIT8M8mc8mSkPMmaSTyGyTYTmTJmZJPOSZPknkzJM5OZPDpDOTkkqQzmZnhnkPk"
    "JnIZJyYZOTJJ5z5yTzJM5OUycyTpkyE8mTyTmZOQzmE5JzkkzOSZzk5nkyZlJ4YZ5OTMMzM5M5wlJmYeZzMybyZJ5zklk5KZPPMm"
    "ZknJJJpkmckyyScmGYfyH8szmZnySSydJJnMMzJMmQzySXDmczD+TNJM5k5MyzD8yWTJyZMnOZyZPMyZyTNMmdMmeTJJ8znMnJyH"
    "LM8mZnJJMkmczMyeZPJkyUk585zMzPJ5k2ckySczJk8kmckJ8nIZoZmZJnMzOT5TJpkk9CTJyTSTnnMkw5MyTOQvJPJM5pJPkzMz"
    "lM5k5ZJPIZkkknhmZzITQzkpMySaZk+fknk8pkPyclDJOSTzJyZNIZJzJzmZmTJzJmTPkm4ZMF0E//h5CAEQcwNO/Yz+i/+LAIwB"
    "jAKLA4cEf+aoNhbj8fezn15fW6BSg6yAEJMwymcz58nDZkz5kvM5JOZJOTmZJ5JzJSENJnhmSczJMnTJmnIcoTkymTkzPJDmTJkJ"
    "yTJnJ5JOSfJnJPOczMzOcoekmeTJkzmTOSZDmSZSGZTJmSfnOT5OcyZzMzkOUk8kmZMmeYZPJknJOZ5JSTTJ+ck0kpzJmkk8kOZz"
    "JMmSSdITJzJPCZzmczycySfOeTyTMkkmSZJhzCclJM5kknMn5Dk+TMs8m0J5mSTzJMmZPITmThJmczOSeQzyeZJmZmWTOfD5nnMM"
    "8zMnhJknJOZJM8ySwkz5nnM7JOTZJmTM0yZkklJJ8kknTJJmZnMPPknyZNnJ8nJnIT8M8mcPCZMhMmQyZJPCEsk2E5kyZmZPPJnz"
    "J5zJM5OZMw8hJycklkM5mZ4Z2H5LsOTkw5zyTnJkzkk5kmZJkpJOZJlMmQnnJ0k/zkzmTkn5JMzJJnOQ5nkyZJSeGGeTzMMzPnPy"
    "UmZh+czMnmTJMkknJOTmTJ5OZP58mdMkzknZk5KGYechySQpMmZhmckk4SSkkzmZmSfmeSS5zOZh/JmkmcwnJknJD5DJkmTkzkpD"
    "mec88mckzTJnTJnJMkmTMkkzJychk5nkzPyTzPmZ5PyeTJkySZ8JmTMkzJITkk2ckzPMyZPJnzCfJyGemSZJnMzMk8hmSUkknKEm"
    "TkmknPOUyhZKZOcheSeSZyUkknJJmSczOZOcknwz+fDLnLDoZzSZkk0zJ8JnJJyeGZDPJyUMk5JPMmkyaTJ5k5zMzJhOZMySTMhN"
    "kMkzJPCTMMpnM+fJw2ZM+ZLzOSTmSTk5mSeScyUhDSZ4ck8kyTJ0yZpyHKE5Mpk5MzyQ5kyZCckyZyeSTknyZyTznMzMznKHpJnk"
    "yZM5kzkmQ5kmUhmUyZkn5zk+TnMmczM5DlJPJJmTJnmGTyZJyTmeSUk0yfnJNJKcyZpJPJDmcyTJkknSEycyTwmc5nM8nMknznk8"
    "kzJJJkmSYcwnJSTOZJJzJ+Q5PkzLPJtCeZkk8yTJmTyE5k4SZnMzknkM8nmSZmZlkznw+Z5zDPMzJ4SZJyTmSTPMksJM+Z5zOyTk"
    "2SZkzNMmZJJSSfJJJ0ySZmZzDz5J8mTZyfJyZyE/DPJnDwmTITJkMmSTwhLJNhOZMmZmTzyZ8yecyTOTmTMPIScnJJZDOZmeGdh+"
    "S7Dk5JpOeSc5MmcknMkzJMlJJzJMpkyE85Okn+cmcyck/JJmZJM5yHM8mTJKTwwzyeZhmZ85+SkzMPzmZk8yZJkkk5JycyZPJzJ/"
    "Pkzpkmck7MnJQzDzkOSSFJkzMM5JJJwklJJnMzMk/M8klzmczD+TNJM5hOTJOSHyGTJMnJnJSHM8555M5JmmTOmTOSZJMmZJJmTk"
    "5DJzPJmfknmfMzyfk8mTJkkz4TMmZJmSQnJJs5JmeZkyeTPk/nkM9MkyTOZmZJ5DMkpJJOUJMnJNJOecplCyUyc4SySTyTOSkkk5"
    "JMyTmZjQSw==";
const char* kMp3Fixture =
    "//tQxAAACixDLlWUgAGBFWYjOtAAB6rDjjOF02TxocEIHFgfXh8YHEYEHonlsy5aD6g7E2vw+GBQFAQDAoJEDE5o0aNsH3iAEAQx"
    "OD/BB050+c5fznL+7p93Lg+D4Ph8EAxKAMH9IAEIAAEEABhqOhh7cQgAQxTkZGgxtMo1uIUAgcb7B0p2czA4omYlhMYRhIt7r9bB"
    "HQHT4DlEmC1fhah2jCiW/47hhhLiRHr/5ImReLxiXf/y6ZF4vIl0u/xEFQVER7/gqgBSsYA4AdmAPP/7UsQEggpgIRC98QABS4Sh"
    "oc/sSAHZgTgFyYAgBPGAjgcBltKwGZHsKjmFcA3JgpAHOYEmBeGBUgUhgXIEUYBiAVQNRw6CztWZp1+qijh78q3/rXb9NWjq/enG"
    "///vAgijAIFMYDQzWeTa0KMIFDUTNgmcszTENTMIOBKD78E3uJNueDbdkz85LoO/GKewCdHCjOtysWo35Kjvw/d9BFx5Vjb0WfuX"
    "nNCAG/ormHu60dEwEkBSMBQAcTAcwLgwMgFvMHdEgDMuIzgzGMRmMG0B//tSxA+DyzRXCg/sRsGNCuEBz9RIOzZUoyQxNzXjlsk2"
    "A1DBZpsus9VNvpCr6cijaJnztYA3zHeINXvTkFfeitiWs5KPbfrXVsfv6N3NbUGDgQZLJJphPHJ6qYVsIWGj5wHxowghgYWGC/gd"
    "CgAGpF4BpNVgaQlQGVioAUDxQBFy4X02f7c59P1ntIux+YZXXsLkmPHNsb3yi1xlLtl77nNvyxuwgmMYm8j2ZQhRkUumB1wAyKq0"
    "ZMXmWohoDubz6GCWBipkH7CqZAIGBmCIgaT/+1LEDwIJzCMQzf9CQV0K4ZX9iNhsl5lSRxmZ3/BuyyBsDWey26X36PIXxfKPUjTO"
    "uR6e6j9/+zsvk2jJgvakPlW/ToLCAwDACZgGwBYYDmA3mBwAehhDoYiZwwvrmbhhiphEgIsfrem7xZsz8bHymcnpdh/5RT2CPqyf"
    "Iq/rjaL709T+9GUd9d8kk0jbjb1e0Vo+f662IOq33/zdAEBJjsiGPjKZZUxpymmC+iDhlIkBsZP+IDGCrAkZoKcZQMm0oxw06asS"
    "iQc6sus9eddsC//7UsQaAoqAJQyuf2JBcwrhQc/USHYKO739WRZ5vYpP6+qQt0dKHK6rlf3UTSFolF0UJ73pBQgMijM0UejhcNMJ"
    "2DWDQYWfoz/wNeMJ/BNwOONgDRSkAzqlwM3QwDHxKAOAYuQi5cM00PU3mv/nG1WMVYqpHlFfR13le917EDszvJWopfsRManJ9dat"
    "pdF15pZpAAAAtULiaSRZ0EhjAGTJxjBMEPMpeN0yhBCTBDA8Bg1LgNE4yDDQLada6+kbfs9F0drd+Ev0fLa+//Z/d6KN//tSxB+C"
    "B+wfIa17AmECg6U1juBMge842dtdkbLEAZsUYhi2cjbacbi6YhAgbylny6gVew+kBxrreJq/2rovut++g/+2vubuTpti1bzf6fZX"
    "XbR6Le8AACILMSi4xMSTG5+MyQUwQENCMcmZsDG9QzIwOIDnMzANABOimPNlN8RGgUCz2NiurXkfT7a/o12f6NK6u/9naUt/rsv3"
    "3fQFmVQCxQFDzQEjnwzDUD+NemDg1xBADDVBTOk2M4oMmlMhQMAId+kBxpnfXo09X01n9lb/+1LEPYIJBCMRDn9CQQUD42WvaEym"
    "Uvo/T1/7+27+n/+rsSoAAAB0AJpqNmEFmFMGMamj+GDcNaZveyJmwDSmDCDYDFxYo3BTpiB1TQZ7F7NWz7/d+R//36f9d/4p//Zz"
    "QXALMBcEUwgwSzC4F4MbI1Y+AOLzylWfMv8h0xjwsTBnDnMNMLkwyQ0TAZA/BwBau6CMSw+IhV93dyidRPNxzL2afp+7///T+v/0"
    "//6KAAAgCAYBIpDIaAAAAAMYCaMyosMEgHMRSIrZnFIEmNwlGf/7UsRWggesIRuteyJBNIsiArwwABY5QJrIxcDYwaAAAgIYBgF3"
    "0L0vFopE/6OBjCAhqAyq1/+09dEOSz5p/nK//9zmGNMRXSLlMOwzl3///ftxJZYtSjHKmlVWl////sQ/G5fLLGFLr8dU1N4YB8CR"
    "Kdlj3lAGH3VjcGv4Poy/2UxBTUUzLjEwMFVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
    "VVVVVVVVVVVVVVVVVVVVVVVVVVVV//tSxG8AEby5MbncAAAAADSDgAAEVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
    "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
    "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
    "VVVVVVU=";
const char* kOggFixture =
    "T2dnUwACAAAAAAAAAACN5aEdAAAAAAsEBK0BHgF2b3JiaXMAAAAAAUSsAAAAAAAAgDgBAAAAAAC4AU9nZ1MAAAAAAAAAAAAAjeWh"
    "HQEAAAAk5avrDj////////////////+BA3ZvcmJpcwwAAABMYXZmNjEuNy4xMDMBAAAAHwAAAGVuY29kZXI9TGF2YzYxLjE5LjEw"
    "MSBsaWJ2b3JiaXMBBXZvcmJpcyJCQ1YBAEAAACRzGCpGpXMWhBAaQlAZ4xxCzmvsGUJMEYIcMkxbyyVzkCGkoEKIWyiB0JBVAABA"
    "AACHQXgUhIpBCCGEJT1YkoMnPQghhIg5eBSEaUEIIYQQQgghhBBCCCGERTlokoMnQQgdhOMwOAyD5Tj4HIRFOVgQgydB6CCED0K4"
    "moOsOQghhCQ1SFCDBjnoHITCLCiKgsQwuBaEBDUojILkMMjUgwtCiJqDSTX4GoRnQXgWhGlBCCGEJEFIkIMGQcgYhEZBWJKDBjm4"
    "FITLQagahCo5CB+EIDRkFQCQAACgoiiKoigKEBqyCgDIAAAQQFEUx3EcyZEcybEcCwgNWQUAAAEACAAAoEiKpEiO5EiSJFmSJVmS"
    "JVmS5omqLMuyLMuyLMsyEBqyCgBIAABQUQxFcRQHCA1ZBQBkAAAIoDiKpViKpWiK54iOCISGrAIAgAAABAAAEDRDUzxHlETPVFXX"
    "tm3btm3btm3btm3btm1blmUZCA1ZBQBAAAAQ0mlmqQaIMAMZBkJDVgEACAAAgBGKMMSA0JBVAABAAACAGEoOogmtOd+c46BZDppK"
    "sTkdnEi1eZKbirk555xzzsnmnDHOOeecopxZDJoJrTnnnMSgWQqaCa0555wnsXnQmiqtOeeccc7pYJwRxjnnnCateZCajbU555wF"
    "rWmOmkuxOeecSLl5UptLtTnnnHPOOeecc84555zqxekcnBPOOeecqL25lpvQxTnnnE/G6d6cEM4555xzzjnnnHPOOeecIDRkFQAA"
    "BABAEIaNYdwpCNLnaCBGEWIaMulB9+gwCRqDnELq0ehopJQ6CCWVcVJKJwgNWQUAAAIAQAghhRRSSCGFFFJIIYUUYoghhhhyyimn"
    "oIJKKqmooowyyyyzzDLLLLPMOuyssw47DDHEEEMrrcRSU2011lhr7jnnmoO0VlprrbVSSimllFIKQkNWAQAgAAAEQgYZZJBRSCGF"
    "FGKIKaeccgoqqIDQkFUAACAAgAAAAABP8hzRER3RER3RER3RER3R8RzPESVREiVREi3TMjXTU0VVdWXXlnVZt31b2IVd933d933d"
    "+HVhWJZlWZZlWZZlWZZlWZZlWZYgNGQVAAACAAAghBBCSCGFFFJIKcYYc8w56CSUEAgNWQUAAAIACAAAAHAUR3EcyZEcSbIkS9Ik"
    "zdIsT/M0TxM9URRF0zRV0RVdUTdtUTZl0zVdUzZdVVZtV5ZtW7Z125dl2/d93/d93/d93/d93/d9XQdCQ1YBABIAADqSIymSIimS"
    "4ziOJElAaMgqAEAGAEAAAIriKI7jOJIkSZIlaZJneZaomZrpmZ4qqkBoyCoAABAAQAAAAAAAAIqmeIqpeIqoeI7oiJJomZaoqZor"
    "yqbsuq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq4LhIasAgAkAAB0JEdyJEdSJEVSJEdygNCQVQCADACAAAAcwzEk"
    "RXIsy9I0T/M0TxM90RM901NFV3SB0JBVAAAgAIAAAAAAAAAMybAUy9EcTRIl1VItVVMt1VJF1VNVVVVVVVVVVVVVVVVVVVVVVVVV"
    "VVVVVVVVVVVVVVVVVVVN0zRNEwgNWQkAAAEA0FpzzK2XjkHorJfIKKSg10455qTXzCiCnOcQMWOYx1IxQwzGlkGElAVCQ1YEAFEA"
    "AIAxyDHEHHLOSeokRc45Kh2lxjlHqaPUUUqxplo7SqW2VGvjnKPUUcoopVpLqx2lVGuqsQAAgAAHAIAAC6HQkBUBQBQAAIEMUgop"
    "hZRizinnkFLKOeYcYoo5p5xjzjkonZTKOSedkxIppZxjzinnnJTOSeack9JJKAAAIMABACDAQig0ZEUAECcA4HAcTZM0TRQlTRNF"
    "TxRd1xNF1ZU0zTQ1UVRVTRRN1VRVWRZNVZYlTTNNTRRVUxNFVRVVU5ZNVbVlzzRt2VRV3RZV1bZlW/Z9V5Z13TNN2RZV1bZNVbV1"
    "V5Z1XbZt3Zc0zTQ1UVRVTRRV11RV2zZV1bY1UXRdUVVlWVRVWXZdWddVV9Z9TRRV1VNN2RVVVZZV2dVlVZZ1X3RV3VZd2ddVWdZ9"
    "29aFX9Z9wqiqum7Krq6rsqz7si77uu3rlEnTTFMTRVXVRFFVTVe1bVN1bVsTRdcVVdWWRVN1ZVWWfV91ZdnXRNF1RVWVZVFVZVmV"
    "ZV13ZVe3RVXVbVV2fd90XV2XdV1YZlv3hdN1dV2VZd9XZVn3ZV3H1nXf90zTtk3X1XXTVXXf1nXlmW3b+EVV1XVVloVflWXf14Xh"
    "eW7dF55RVXXdlF1fV2VZF25fN9q+bjyvbWPbPrKvIwxHvrAsXds2ur5NmHXd6BtD4TeGNNO0bdNVdd10XV+Xdd1o67pQVFVdV2XZ"
    "91VX9n1b94Xh9n3fGFXX91VZFobVlp1h932l7guVVbaF39Z155htXVh+4+j8vjJ0dVto67qxzL6uPLtxdIY+AgAABhwAAAJMKAOF"
    "hqwIAOIEABiEnENMQYgUgxBCSCmEkFLEGITMOSkZc1JCKamFUlKLGIOQOSYlc05KKKGlUEpLoYTWQimxhVJabK3VmlqLNYTSWiil"
    "tVBKi6mlGltrNUaMQcick5I5J6WU0loopbXMOSqdg5Q6CCmllFosKcVYOSclg45KByGlkkpMJaUYQyqxlZRiLCnF2FpsucWYcyil"
    "xZJKbCWlWFtMObYYc44Yg5A5JyVzTkoopbVSUmuVc1I6CCllDkoqKcVYSkoxc05KByGlDkJKJaUYU0qxhVJiKynVWEpqscWYc0sx"
    "1lBSiyWlGEtKMbYYc26x5dZBaC2kEmMoJcYWY66ttRpDKbGVlGIsKdUWY629xZhzKCXGkkqNJaVYW425xhhzTrHlmlqsucXYa225"
    "9Zpz0Km1WlNMubYYc465BVlz7r2D0FoopcVQSoyttVpbjDmHUmIrKdVYSoq1xZhza7H2UEqMJaVYS0o1thhrjjX2mlqrtcWYa2qx"
    "5ppz7zHm2FNrNbcYa06x5Vpz7r3m1mMBAAADDgAAASaUgUJDVgIAUQAABCFKMQahQYgx56Q0CDHmnJSKMecgpFIx5hyEUjLnIJSS"
    "UuYchFJSCqWkklJroZRSUmqtAACAAgcAgAAbNCUWByg0ZCUAkAoAYHAcy/I8UTRV2XYsyfNE0TRV1bYdy/I8UTRNVbVty/NE0TRV"
    "1XV13fI8UTRVVXVdXfdEUTVV1XVlWfc9UTRVVXVdWfZ901RV1XVlWbaFXzRVV3VdWZZl31hd1XVlWbZ1WxhW1XVdWZZtWzeGW9d1"
    "3feFYTk6t27ruu/7wvE7xwAA8AQHAKACG1ZHOCkaCyw0ZCUAkAEAQBiDkEFIIYMQUkghpRBSSgkAABhwAAAIMKEMFBqyEgCIAgAA"
    "CJFSSimNlFJKKaWRUkoppZQSQgghhBBCCCGEEEIIIYQQQgghhBBCCCGEEEIIIYQQQggFAPhPOAD4P9igKbE4QKEhKwGAcAAAwBil"
    "mHIMOgkpNYw5BqGUlFJqrWGMMQilpNRaS5VzEEpJqbXYYqycg1BSSq3FGmMHIaXWWqyx1po7CCmlFmusOdgcSmktxlhzzr33kFJr"
    "MdZac++9l9ZirDXn3IMQwrQUY6659uB77ym2WmvNPfgghFCx1Vpz8EEIIYSLMffcg/A9CCFcjDnnHoTwwQdhAAB3gwMARIKNM6wk"
    "nRWOBhcashIACAkAIBBiijHnnIMQQgiRUow55xyEEEIoJVKKMeecgw5CCCVkjDnnHIQQQiillIwx55yDEEIJpZSSOecchBBCKKWU"
    "UjLnoIMQQgmllFJK5xyEEEIIpZRSSumggxBCCaWUUkopIYQQQgmllFJKKSWEEEIJpZRSSimlhBBKKKWUUkoppZQQQimllFJKKaWU"
    "EkIopZRSSimllJJCKaWUUkoppZRSUiillFJKKaWUUkoJpZRSSimllJRSSQUAABw4AAAEGEEnGVUWYaMJFx6AQkNWAgBAAAAUxFZT"
    "iZ1BzDFnqSEIMaipQkophjFDyiCmKVMKIYUhc4ohAqHFVkvFAAAAEAQACAgJADBAUDADAAwOED4HQSdAcLQBAAhCZIZINCwEhweV"
    "ABExFQAkJijkAkCFxUXaxQV0GeCCLu46EEIQghDE4gAKSMDBCTc88YYn3OAEnaJSBwEAAAAAcAAADwAAxwUQEdEcRobGBkeHxwdI"
    "SAAAAAAAyADABwDAIQJERDSHkaGxwdHh8QESEgAAAAAAAAAAAAQEBAAAAAAAAgAAAAQET2dnUwAEdCIAAAAAAACN5aEdAgAAAI+a"
    "1cQLHzwnJSUlJSUlakVc3as6q7qw/1oCBBAAwIzaLbY333zzzTAMwzAMw3oAmtg9B2/SnltcBWYiQCoAAAAAAAAAAAAAAPr9YJ/O"
    "AdG1F1zwn6MOe/zSDBfs0a1btz98y5tvnjxuVAAA3ti9WFcp84gwMXb8rWA6AAAAAGAAAAAAAAAAvs4GAACDh+d5AQAT3ti9qFcp"
    "/RZlwM79DUwHAAAAAAAAAAAAAAAwZxEAgAQzXUldAN7YvahXKf0WZcDO/QqmAwAAAAAAAAAAAAAA+PENAABMvlAeAwDe2L1YVynz"
    "YW3Cya0KpgMAAAAAAAAAAAAAAMjqJwAAyAuP1wEA3ti9WFcp87AyMU78mWA6AAAAAAAAAAAAAACAr9sAAGBQ8N4NAN7YvVg3KfMW"
    "acLM/QmmAwAAAAAAAAAAAAAA2B4FAABE/95JPwDe2L1YVynzFmnCiT8bmA4AAAAAAAAAAAAAAOB6IwAAyGp1ZgAAvqi9Dl6/7xGv"
    "N+YJH5AKACCJAQAAAAAAAFwlcIaV9KmtTOSW00+YYV3roONbU2Qzfr+9cmq3XPqHSpquXq9h2D8Mabp6XWE4zTCk6ed1RYfTDEOa"
    "vn1d0eH4MDjp6nX5cHwYnPTzqvRuG/6V/HeoiNiauJ5wgNPby/yxnDsjIyNjzGMbYDAAAAAAAAAAoK7fvaTL3bxhpfu5/N/xreeb"
    "d/YQ6r7SdLrpjo97PQwDAA==";
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
        const auto brokenOgg = dir / "broken.ogg";
        makeWav(first, 3, 220); makeWav(second, 3, 660); makeWav(shortFile, .08, 440);
        { std::ofstream out(broken); out << "This is not audio"; }
        { std::ofstream out(brokenOgg); out << "This is not Ogg audio"; }
        check(std::abs(abser::probeDuration(first.wstring()) - 3.0) < .001, "WAV duration must be read accurately");
        check(abser::probeDuration(broken.wstring()) < 0, "Broken file must have unknown duration");
        check(abser::probeDuration(brokenOgg.wstring()) < 0, "Malformed OGG probing must close its handle exactly once");

        abser::AudioEngine audio;
        check(audio.initialize(true), "Silent test output must initialize");
        // Submit then immediately pause, before the asynchronous open/decode is
        // guaranteed to have completed. The worker must preserve this intent.
        audio.play(first.wstring());
        audio.pause();
        check(audio.snapshot().state == AudioState::Paused && audio.snapshot().path == first.wstring(), "Immediate pause must preserve the selected loading file");
        std::this_thread::sleep_for(20ms);
        const double loadingPausedAt = audio.snapshot().positionSeconds;
        audio.pause();
        std::this_thread::sleep_for(45ms);
        check(audio.snapshot().state == AudioState::Paused && std::abs(audio.snapshot().positionSeconds - loadingPausedAt) < .001, "Decoder startup must not resume a file paused during loading");
        audio.play(first.wstring());
        check(eventually([&] { auto s = audio.snapshot(); return s.state == AudioState::Playing && s.positionSeconds > .025; }), "WAV must start and advance");
        check(audio.waveformSnapshot().loading, "Playback must start while waveform work is still deferred");
        audio.togglePause();
        check(audio.snapshot().state == AudioState::Paused, "Space pause must change state");
        std::this_thread::sleep_for(20ms);
        const double pausedAt = audio.snapshot().positionSeconds;
        std::this_thread::sleep_for(45ms);
        check(std::abs(audio.snapshot().positionSeconds - pausedAt) < .001, "Paused stream must not advance");
        audio.togglePause();
        check(eventually([&] { return audio.snapshot().positionSeconds > pausedAt + .02; }), "Resume must continue at paused position");
        audio.pause();
        check(audio.snapshot().state == AudioState::Paused, "Idempotent pause must pause playing audio");
        std::this_thread::sleep_for(20ms);
        const auto dragPaused = audio.snapshot();
        audio.pause();
        audio.pause();
        std::this_thread::sleep_for(45ms);
        check(audio.snapshot().state == AudioState::Paused && audio.snapshot().path == dragPaused.path && std::abs(audio.snapshot().positionSeconds - dragPaused.positionSeconds) < .001, "Repeated pause must preserve state, selection, and frozen position");
        audio.togglePause();
        check(eventually([&] { return audio.snapshot().positionSeconds > dragPaused.positionSeconds + .02; }), "Space must resume an idempotently paused file in place");

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 500; ++i) audio.play(i % 2 ? first.wstring() : second.wstring());
        audio.play(second.wstring());
        check(std::chrono::steady_clock::now() - start < 500ms, "Rapid selection must not wait for decoder work");
        check(eventually([&] { auto s = audio.snapshot(); return s.path == second.wstring() && s.state == AudioState::Playing && s.positionSeconds > .015 && s.positionSeconds < .5; }), "Latest selection must win and start at the beginning");
        for (int i = 0; i < 100; ++i) { audio.play(first.wstring()); audio.stop(); }
        audio.stop();
        std::this_thread::sleep_for(25ms);
        check(audio.snapshot().state == AudioState::Stopped && audio.snapshot().positionSeconds < .001, "Stop must reset playback position");
        const auto stoppedSelection = audio.snapshot().path;
        audio.pause();
        audio.pause();
        std::this_thread::sleep_for(25ms);
        check(audio.snapshot().state == AudioState::Stopped && audio.snapshot().path == stoppedSelection && audio.snapshot().positionSeconds < .001, "Pause must never restart a stopped file or change its selection");
        audio.togglePause();
        check(eventually([&] { return audio.snapshot().state == AudioState::Playing; }), "Space after Stop must restart selected file");
        audio.play(shortFile.wstring());
        check(eventually([&] { return audio.snapshot().state == AudioState::Finished; }), "Short file must reach EOF without auto-playing another file");
        check(std::abs(audio.snapshot().positionSeconds - .08) < .003, "EOF must preserve actual duration");
        audio.pause();
        audio.pause();
        std::this_thread::sleep_for(25ms);
        check(audio.snapshot().state == AudioState::Finished && audio.snapshot().path == shortFile.wstring() && std::abs(audio.snapshot().positionSeconds - .08) < .003, "Pause at EOF must never restart the selected file");
        audio.togglePause();
        check(eventually([&] { return audio.snapshot().state == AudioState::Playing; }), "Space at EOF must restart");
        audio.play(broken.wstring());
        check(eventually([&] { return audio.snapshot().state == AudioState::Error; }), "Damaged file must report error");
        const auto decodeError = audio.snapshot().error;
        audio.pause();
        audio.pause();
        std::this_thread::sleep_for(20ms);
        check(audio.snapshot().state == AudioState::Error && audio.snapshot().path == broken.wstring() && audio.snapshot().error == decodeError, "Pause must preserve a failed file without retrying playback");
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
            check(eventually([&] { const auto w = audio.waveformSnapshot(); return w.path == aiff.wstring() && !w.loading && !w.peaks.empty(); }), "Every supported AIFF variant must produce a waveform");
        }
#ifdef _WIN32
        const auto unicode = dir / std::filesystem::path(L"\u65e5\u672c\u8a9e-\u00e4\u00f6.wav");
        makeWav(unicode, .08, 440);
        check(std::abs(abser::probeDuration(unicode.wstring()) - .08) < .001, "Unicode file path duration must work");
        audio.play(unicode.wstring());
        check(eventually([&] { return audio.snapshot().state == AudioState::Finished; }), "Unicode file path playback must work");
        check(eventually([&] { auto w = audio.waveformSnapshot(); return w.path == unicode.wstring() && !w.loading && !w.peaks.empty(); }), "Unicode file path waveform must work");
#endif
        for (const auto& compressed : {std::pair<const char*, const char*>{"sample.flac", kFlacFixture}, {"sample.mp3", kMp3Fixture}, {"sample.ogg", kOggFixture}}) {
            const auto file = dir / compressed.first;
            writeBase64(file, compressed.second);
            if (file.extension() == ".mp3") check(abser::probeDuration(file.wstring()) < 0, "Untagged MP3 metadata probe must not scan all frames");
            audio.play(file.wstring());
            check(eventually([&] { auto w = audio.waveformSnapshot(); return w.path == file.wstring() && !w.loading && !w.peaks.empty(); }), "FLAC/MP3/OGG must produce waveforms without external codecs");
            check(eventually([&] { return audio.snapshot().state == AudioState::Finished || audio.snapshot().state == AudioState::Error; }), "Compressed playback must finish");
            check(audio.snapshot().state == AudioState::Finished, "Shared decoder I/O must support every compressed format");
            const auto compressedWave = audio.waveformSnapshot();
            check(compressedWave.peaks.size() == 512 && *std::max_element(compressedWave.peaks.begin(), compressedWave.peaks.end()) > .05f, "Compressed waveform must contain audible peaks");
            check(std::abs(compressedWave.durationSeconds - audio.snapshot().positionSeconds) < .002, "Full waveform scan must recover compressed duration accurately, including untagged MP3");
        }
        const auto impulseFile = dir / "impulses.wav";
        makePeakWav(impulseFile, 48000, 1, {{24000, 24576}, {47999, -16384}});
        audio.play(impulseFile.wstring());
        check(eventually([&] { const auto w = audio.waveformSnapshot(); return w.path == impulseFile.wstring() && !w.loading && !w.peaks.empty(); }), "Waveform must complete asynchronously");
        auto wave = audio.waveformSnapshot();
        check(wave.peaks.size() == 512, "Waveform memory must stay at 512 display bins");
        check(std::abs(wave.durationSeconds - 1.0) < .0001, "Waveform scan must report exact decoded duration");
        check(*std::max_element(wave.peaks.begin() + 255, wave.peaks.begin() + 258) > .749f, "A one-frame impulse must survive waveform reduction");
        check(wave.peaks.back() > .499f, "A final-frame impulse must not be lost");
        check(*std::max_element(wave.peaks.begin(), wave.peaks.begin() + 128) == 0, "Silence must stay silent in waveform bins");
        for (float peak : wave.peaks) check(std::isfinite(peak) && peak >= 0 && peak <= 1, "Waveform bins must be finite drawing amplitudes");

        const auto stereo = dir / "opposite-phase.wav";
        makePeakWav(stereo, 4800, 2, {{1200, 24576}}, true);
        audio.play(stereo.wstring());
        check(eventually([&] { auto w = audio.waveformSnapshot(); return w.path == stereo.wstring() && !w.loading && w.peaks.size() == 512; }), "Stereo waveform must complete");
        wave = audio.waveformSnapshot();
        check(*std::max_element(wave.peaks.begin(), wave.peaks.end()) > .749f, "Opposite-phase channels must not cancel in the waveform");
        check(std::abs(wave.durationSeconds - .1) < .0001, "Stereo duration must count frames rather than samples");

        // Same path, changed length/data must invalidate the bounded cache.
        audio.stop();
        std::this_thread::sleep_for(15ms);
        makePeakWav(stereo, 9600, 2, {{9000, 8192}}, true);
        audio.play(stereo.wstring());
        check(eventually([&] { auto w = audio.waveformSnapshot(); return w.path == stereo.wstring() && !w.loading && std::abs(w.durationSeconds - .2) < .0001; }), "Changed file must invalidate its waveform cache entry");
        wave = audio.waveformSnapshot();
        check(*std::max_element(wave.peaks.begin(), wave.peaks.end()) > .249f && *std::max_element(wave.peaks.begin(), wave.peaks.end()) < .251f, "Updated waveform must reflect changed audio");

        const auto longFile = dir / "long-silence.wav";
        makePeakWav(longFile, 32 * 1024 * 1024, 1, {{32 * 1024 * 1024 - 1, 32767}});
        audio.play(longFile.wstring());
        check(eventually([&] { return audio.snapshot().positionSeconds > .14; }), "Long file must play while waveform work is scheduled");
        const auto switchStart = std::chrono::steady_clock::now();
        audio.play(shortFile.wstring());
        check(std::chrono::steady_clock::now() - switchStart < 100ms, "Switching away from waveform scan must not wait for analysis");
        check(eventually([&] { auto w = audio.waveformSnapshot(); return w.path == shortFile.wstring() && !w.loading && !w.peaks.empty(); }), "Only the latest waveform request may publish");
        std::this_thread::sleep_for(60ms);
        check(audio.waveformSnapshot().path == shortFile.wstring() && std::abs(audio.waveformSnapshot().durationSeconds - .08) < .0001, "Cancelled long scan must never overwrite the new waveform");

        audio.play(broken.wstring());
        check(eventually([&] { auto w = audio.waveformSnapshot(); return w.path == broken.wstring() && !w.loading; }), "Invalid audio must clear waveform loading state");
        check(audio.waveformSnapshot().peaks.empty(), "Invalid audio must not show a previous waveform");
        audio.play(longFile.wstring());
        audio.stop();
        check(!audio.waveformSnapshot().loading && audio.waveformSnapshot().path.empty(), "Stop must cancel and clear pending waveform work");
#ifdef _WIN32
        audio.play(first.wstring());
        check(eventually([&] { return audio.snapshot().positionSeconds > .025; }), "Move test must hold an active decoder handle");
        const HANDLE deleteAccess = CreateFileW(first.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(deleteAccess != INVALID_HANDLE_VALUE, "Active decoder handles must share DELETE access with Explorer");
        CloseHandle(deleteAccess);
        const auto moved = dir / "moved-while-playing.wav";
        check(MoveFileExW(first.c_str(), moved.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE, "Playing file must be movable without stopping playback");
        const double movedAt = audio.snapshot().positionSeconds;
        check(eventually([&] { return audio.snapshot().positionSeconds > movedAt + .02; }), "Playback must continue through a file move");
        check(MoveFileExW(moved.c_str(), first.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE, "Playing file can be renamed back");
#endif
        audio.play(longFile.wstring());
        const auto shutdownStart = std::chrono::steady_clock::now();
        audio.shutdown();
        check(std::chrono::steady_clock::now() - shutdownStart < 1500ms, "Shutdown must cancel pending waveform work");
        std::filesystem::remove_all(dir);
        std::cout << "Audio tests passed: duration, playback, pause/resume, rapid switching, stop/reset, EOF, malformed file recovery, AIFF/AIFC PCM/float/little-endian, waveform peaks/duration/cancellation/cache, file sharing, idempotent pause.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

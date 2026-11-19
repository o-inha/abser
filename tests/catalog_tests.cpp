#include "catalog.h"
#include <cwctype>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace abser;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::wstring asWide(const fs::path& value) {
#ifdef _WIN32
    return value.wstring();
#else
    // All test fixture directory paths are ASCII; Unicode names are kept separately.
    const std::string s = value.string();
    return std::wstring(s.begin(), s.end());
#endif
}

void writeText(const fs::path& file, const std::string& contents) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << contents;
    if (!out) throw std::runtime_error("Could not create fixture");
}

struct Fixture {
    fs::path root = fs::temp_directory_path() /
        ("abser-catalog-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
};

void testEnumerationAndInput(const fs::path& root) {
    fs::path audio = root / "audio files";
    fs::create_directories(audio / "nested");
    writeText(audio / "take10.WAV", "not actually audio");
    writeText(audio / "take2.mp3", "not actually audio");
    writeText(audio / "take1.flac", "not actually audio");
    writeText(audio / "clip.OGG", "not actually audio");
    writeText(audio / "readme.txt", "ignore");
    writeText(audio / "nested" / "hidden.wav", "ignore");
    fs::create_directory(audio / "directory.wav");
    MetadataStore metadata(root / "state" / "metadata.txt");
    const std::wstring normalized = normalizeDirectoryInput(L"  \"" + asWide(audio / "nested" / "..") + L"\" \r\n");
    require(normalized == normalizeDirectoryInput(asWide(audio)), "Quoted path normalization failed");
    auto result = scanDirectory(L"\"" + asWide(audio) + L"\"", metadata);
    require(result.error.empty(), "Valid directory scan failed");
    require(result.files.size() == 4, "Scan included directories, unsupported files or nested files");
    require(result.files[0].name == L"clip.OGG" && result.files[2].name == L"take2.mp3" &&
            result.files[3].name == L"take10.WAV", "Scan natural order incorrect");
    for (const auto& file : result.files) {
        require(file.duration == -1.0, "Directory scan must not decode audio");
        require(file.modified > 0, "File modification date missing");
    }
    require(!fs::exists(audio / "metadata.txt"), "Category file leaked into audio directory");
    require(!scanDirectory(asWide(audio / "missing"), metadata).error.empty(), "Missing directory should report an error");
    require(!scanDirectory(asWide(audio / "readme.txt"), metadata).error.empty(), "File path should not be a directory");
    require(!scanDirectory(L"", metadata).error.empty(), "Empty input should report an error");
    require(isAudioFile(L"audio.WAVE") && !isAudioFile(L"audio.wav.exe"), "Extension filter is incorrect");
#ifdef _WIN32
    _wputenv_s(L"ABSER_TEST_DIRECTORY", asWide(audio).c_str());
#else
    setenv("ABSER_TEST_DIRECTORY", audio.string().c_str(), 1);
#endif
    require(normalizeDirectoryInput(L"%ABSER_TEST_DIRECTORY%") == normalizeDirectoryInput(asWide(audio)),
            "Environment variable expansion failed");
}

void testMetadataRoundtrip(const fs::path& root) {
    const fs::path state = root / "state" / "metadata.txt";
    MetadataStore metadata(state);
    require(metadata.load(), "Missing metadata should be a successful empty load");
    require(!fs::exists(state), "Load must not create metadata directories or files");
    const std::wstring file = asWide(root) + L"/日本語/ääni \"take\".wav";
    const std::wstring other = asWide(root) + L"/other.wav";
    metadata.setCategory(file, 10);
    metadata.setCategory(other, 3);
    metadata.setCategory(other, 0);
    metadata.setCategory(file, -1);
    metadata.setCategory(file, 11);
    require(metadata.category(file) == 10 && metadata.category(other) == 0, "Category assignment/clearing failed");
    metadata.settings.volume = 0.37f;
    metadata.settings.windowX = -600;
    metadata.settings.windowWidth = 320;
    metadata.settings.columnWidths = {{34, 220, 60, 115}};
    metadata.settings.lastFolder = asWide(root) + L"/日本語/äänet";
    metadata.settings.sortColumn = SortColumn::Modified;
    metadata.settings.ascending = false;
    metadata.settings.categoryMask = (1u << 0) | (1u << 10);
    std::wstring error;
    require(metadata.save(&error) && error.empty(), "Metadata save failed");
    MetadataStore loaded(state);
    require(loaded.load(&error) && error.empty(), "Metadata reload failed");
    require(loaded.category(file) == 10 && loaded.category(other) == 0, "Unicode/quoted category path did not roundtrip");
    require(std::fabs(loaded.settings.volume - 0.37f) < 0.00001f && loaded.settings.windowX == -600 &&
            loaded.settings.windowWidth == 320, "Window or volume settings did not roundtrip");
    require(loaded.settings.columnWidths == metadata.settings.columnWidths &&
            loaded.settings.lastFolder == metadata.settings.lastFolder &&
            loaded.settings.categoryMask == metadata.settings.categoryMask &&
            loaded.settings.sortColumn == SortColumn::Modified && !loaded.settings.ascending,
            "Filter/column/folder settings did not roundtrip");
    loaded.setCategory(file, 2);
    require(loaded.save(), "Atomic metadata replacement failed");
    MetadataStore replaced(state);
    require(replaced.load() && replaced.category(file) == 2, "Replacement did not contain latest assignment");
    for (const auto& item : fs::directory_iterator(state.parent_path()))
        require(item.path().filename() == "metadata.txt", "Temporary save file was left behind");
#ifdef _WIN32
    std::wstring uppercase = file;
    uppercase[0] = static_cast<wchar_t>(std::towupper(uppercase[0]));
    require(loaded.category(uppercase) == 2, "Windows category paths should be case insensitive");
#endif
}

void testDamagedMetadata(const fs::path& root) {
    fs::path state = root / "damaged.txt";
    const std::wstring file = asWide(root / "valid.wav");
    MetadataStore metadata(state);
    metadata.setCategory(file, 4);
    require(metadata.save(), "Initial damaged-store fixture save failed");
    {
        std::ofstream append(state, std::ios::binary | std::ios::app);
        append << "S volume 8\nS width -1\nS mask 65000\nC 99 \"invalid.wav\"\n"
               << "S col1 nonsense\nC 4 \"\xff\"\nS sort -3\nS folder \"\xff\"\n";
    }
    MetadataStore loaded(state);
    require(loaded.load() && loaded.category(file) == 4, "Bad rows discarded valid metadata");
    require(loaded.settings.volume == 0.8f && loaded.settings.windowWidth == 380 &&
            loaded.settings.categoryMask == kAllCategories && loaded.settings.sortColumn == SortColumn::Name,
            "Out-of-range settings were accepted");
    writeText(state, "INVALID FILE\nC 3 \"valid.wav\"\n");
    std::wstring error;
    require(!loaded.load(&error) && !error.empty() && loaded.category(file) == 4,
            "Bad file format should preserve current in-memory metadata and report an error");
    fs::path blocked = root / "not-a-directory";
    writeText(blocked, "blocked");
    MetadataStore unwritable(blocked / "metadata.txt");
    require(!unwritable.save(&error) && !error.empty(), "Failed save should report an error");
}

void testSortingAndFiltering() {
    std::vector<FileEntry> files{
        {L"a/take10.wav", L"take10.wav", 2, 1.5, 500},
        {L"a/take2.wav", L"take2.wav", 0, 5.0, 100},
        {L"a/take1.wav", L"take1.wav", 10, -1.0, 200},
        {L"a/take3.wav", L"take3.wav", 2, 1.5, 300}
    };
    sortFiles(files, SortColumn::Name, true);
    require(files[0].name == L"take1.wav" && files[1].name == L"take2.wav" && files[3].name == L"take10.wav",
            "Natural ascending name order failed");
    sortFiles(files, SortColumn::Name, false);
    require(files[0].name == L"take10.wav" && files[3].name == L"take1.wav", "Reverse name sort failed");
    sortFiles(files, SortColumn::Category, true);
    require(files[0].category == 0 && files[3].category == 10, "Category sort failed");
    sortFiles(files, SortColumn::Duration, true);
    require(files[0].name == L"take3.wav" && files[2].duration == 5.0 && files[3].duration < 0,
            "Duration sort/tie handling failed");
    sortFiles(files, SortColumn::Duration, false);
    require(files[0].duration == 5.0 && files.back().duration < 0, "Unknown durations should stay last when reversed");
    sortFiles(files, SortColumn::Modified, false);
    require(files.front().modified == 500 && files.back().modified == 100, "Date sort failed");
    const auto filtered = filterFiles(files, (1u << 0) | (1u << 10));
    require(filtered.size() == 2 && filtered[0].category == 10 && filtered[1].category == 0,
            "Category filtering lost order or selected incorrect categories");
    require(filterFiles(files, 0).empty() && filterFiles(files, kAllCategories).size() == files.size(),
            "All/none filtering failed");
    require(naturalCompare(L"take99999999999999999999999", L"take100000000000000000000000") < 0,
            "Natural sorting overflowed a long number");
}
} // namespace

int main() {
    try {
        Fixture fixture;
        testEnumerationAndInput(fixture.root);
        testMetadataRoundtrip(fixture.root);
        testDamagedMetadata(fixture.root);
        testSortingAndFiltering();
        std::cout << "Catalog tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Catalog test failed: " << error.what() << '\n';
        return 1;
    }
}

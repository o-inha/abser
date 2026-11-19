#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace abser {

// Category zero is unassigned; the ten named colour categories are 1..10.
constexpr unsigned kCategoryCount = 11;
constexpr uint16_t kAllCategories = (1u << kCategoryCount) - 1u;

enum class SortColumn { Category = 0, Name = 1, Duration = 2, Modified = 3 };

struct FileEntry {
    std::wstring path;
    std::wstring name;
    int category = 0;
    double duration = -1.0; // Unknown until the background metadata worker reads it.
    int64_t modified = 0;  // Unix seconds, for local display.
};

struct Settings {
    float volume = 0.8f;
    int windowX = 100;
    int windowY = 100;
    int windowWidth = 380;
    int windowHeight = 760;
    std::array<int, 4> columnWidths{{34, 135, 56, 114}};
    SortColumn sortColumn = SortColumn::Name;
    bool ascending = true;
    uint16_t categoryMask = kAllCategories;
    std::wstring lastFolder;
};

class MetadataStore {
public:
    // Defaults to %LOCALAPPDATA%\Abser\metadata.txt on Windows.
    MetadataStore();
    explicit MetadataStore(std::filesystem::path file);
    bool load(std::wstring* error = nullptr);
    // Writes a temporary file and atomically replaces the previous store.
    bool save(std::wstring* error = nullptr) const;
    int category(const std::wstring& fullPath) const;
    void setCategory(const std::wstring& fullPath, int category);
    const std::filesystem::path& dataFile() const { return file_; }
    Settings settings;

private:
    std::filesystem::path file_;
    std::unordered_map<std::wstring, int> categories_;
};

struct ScanResult {
    std::wstring folder;
    std::vector<FileEntry> files;
    std::wstring error;
};

std::wstring normalizeDirectoryInput(const std::wstring& input);
bool isAudioFile(const std::wstring& filename);
ScanResult scanDirectory(const std::wstring& input, const MetadataStore& metadata);
// Natural filename order, including numeric segments (take2 before take10).
int naturalCompare(const std::wstring& left, const std::wstring& right);
void sortFiles(std::vector<FileEntry>& files, SortColumn column, bool ascending);
std::vector<FileEntry> filterFiles(const std::vector<FileEntry>& files, uint16_t categoryMask);

} // namespace abser

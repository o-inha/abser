#include "catalog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

namespace abser {
namespace {

std::string utf8(const std::wstring& value) {
#ifdef _WIN32
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode path");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(value);
#endif
}

std::wstring wide(const std::string& value) {
#ifdef _WIN32
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8 metadata");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                       static_cast<int>(value.size()), out.data(), n);
    return out;
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(value);
#endif
}

std::filesystem::path pathFromWide(const std::wstring& value) {
#ifdef _WIN32
    return std::filesystem::path(value);
#else
    return std::filesystem::u8path(utf8(value));
#endif
}

std::wstring pathToWide(const std::filesystem::path& value) {
#ifdef _WIN32
    return value.wstring();
#else
    return wide(value.u8string());
#endif
}

std::wstring lowercase(std::wstring value) {
#ifdef _WIN32
    if (!value.empty()) {
        const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                      value.data(), static_cast<int>(value.size()), nullptr, 0,
                                      nullptr, nullptr, 0);
        if (size > 0) {
            std::wstring lower(size, L'\0');
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                          static_cast<int>(value.size()), lower.data(), size, nullptr, nullptr, 0);
            return lower;
        }
    }
#endif
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::wstring trim(std::wstring value) {
    auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}

std::wstring expandEnvironment(std::wstring input) {
#ifdef _WIN32
    const DWORD size = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (size && size < 1024 * 1024) {
        std::wstring expanded(size, L'\0');
        if (ExpandEnvironmentStringsW(input.c_str(), expanded.data(), size)) {
            expanded.resize(size - 1);
            return expanded;
        }
    }
#else
    size_t start = 0;
    while ((start = input.find(L'%', start)) != std::wstring::npos) {
        const size_t end = input.find(L'%', start + 1);
        if (end == std::wstring::npos) break;
        const std::string name = utf8(input.substr(start + 1, end - start - 1));
        if (const char* value = std::getenv(name.c_str())) {
            const std::wstring replacement = wide(value);
            input.replace(start, end - start + 1, replacement);
            start += replacement.size();
        } else start = end + 1;
    }
#endif
    return input;
}

std::wstring categoryKey(const std::wstring& path) {
    std::wstring normalized = normalizeDirectoryInput(path);
#ifdef _WIN32
    // Extended-length and ordinary spellings name the same file.
    if (normalized.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        normalized = L"\\\\" + normalized.substr(8);
    else if (normalized.compare(0, 4, L"\\\\?\\") == 0)
        normalized.erase(0, 4);
    return lowercase(std::move(normalized));
#else
    return normalized;
#endif
}

std::filesystem::path defaultMetadataFile() {
#ifdef _WIN32
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
        std::filesystem::path result = std::filesystem::path(local) / L"Abser" / L"metadata.txt";
        CoTaskMemFree(local);
        return result;
    }
    throw std::runtime_error("Windows could not locate Local AppData");
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"))
        return std::filesystem::u8path(config) / "Abser" / "metadata.txt";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::u8path(home) / ".config" / "Abser" / "metadata.txt";
    return std::filesystem::temp_directory_path() / "Abser" / "metadata.txt";
#endif
}

void setError(std::wstring* error, const wchar_t* value) {
    if (error) *error = value;
}

bool onlyWhitespace(std::istringstream& line) {
    line >> std::ws;
    return line.eof();
}

template<class T> bool readNumber(std::istringstream& line, T& value) {
    T result{};
    if (!(line >> result) || !onlyWhitespace(line)) return false;
    value = result;
    return true;
}

void readSetting(std::istringstream& line, Settings& settings) {
    std::string name;
    if (!(line >> name)) return;
    int n = 0;
    if (name == "volume") {
        float v = 0;
        if (readNumber(line, v) && std::isfinite(v) && v >= 0 && v <= 1) settings.volume = v;
    } else if (name == "folder") {
        std::string p;
        if ((line >> std::quoted(p)) && onlyWhitespace(line)) settings.lastFolder = wide(p);
    } else if (name == "x") {
        if (readNumber(line, n) && n >= -100000 && n <= 100000) settings.windowX = n;
    } else if (name == "y") {
        if (readNumber(line, n) && n >= -100000 && n <= 100000) settings.windowY = n;
    } else if (name == "width") {
        if (readNumber(line, n) && n >= 280 && n <= 16384) settings.windowWidth = n;
    } else if (name == "height") {
        if (readNumber(line, n) && n >= 200 && n <= 16384) settings.windowHeight = n;
    } else if (name == "sort") {
        if (readNumber(line, n) && n >= 0 && n <= 3) settings.sortColumn = static_cast<SortColumn>(n);
    } else if (name == "ascending") {
        if (readNumber(line, n) && (n == 0 || n == 1)) settings.ascending = n != 0;
    } else if (name == "mask") {
        if (readNumber(line, n) && n >= 0 && n <= kAllCategories) settings.categoryMask = static_cast<uint16_t>(n);
    } else if (name.size() == 4 && name.compare(0, 3, "col") == 0 && name[3] >= '0' && name[3] <= '3') {
        if (readNumber(line, n) && n >= 16 && n <= 16384) settings.columnWidths[name[3] - '0'] = n;
    }
}

} // namespace

MetadataStore::MetadataStore() : file_(defaultMetadataFile()) {}
MetadataStore::MetadataStore(std::filesystem::path file) : file_(std::move(file)) {}

bool MetadataStore::load(std::wstring* error) {
    if (error) error->clear();
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        if (!ec) return true;
        setError(error, L"Could not read saved categories.");
        return false;
    }
    if (std::filesystem::file_size(file_, ec) > 64 * 1024 * 1024 || ec) {
        setError(error, L"The saved category file could not be read.");
        return false;
    }
    std::ifstream input(file_, std::ios::binary);
    std::string header;
    if (!input || !std::getline(input, header) || (header != "ABSER 1" && header != "ABSER 1\r")) {
        setError(error, L"The saved category file has an unrecognized format.");
        return false;
    }
    Settings loadedSettings;
    std::unordered_map<std::wstring, int> loadedCategories;
    std::string raw;
    while (std::getline(input, raw)) {
        if (raw.size() > 1024 * 1024) continue;
        std::istringstream line(raw);
        line.imbue(std::locale::classic());
        char type = '\0';
        if (!(line >> type)) continue;
        try {
            if (type == 'S') readSetting(line, loadedSettings);
            else if (type == 'C') {
                int category = 0;
                std::string path;
                if ((line >> category >> std::quoted(path)) && onlyWhitespace(line) &&
                    category > 0 && category < static_cast<int>(kCategoryCount) && !path.empty()) {
                    const std::wstring key = categoryKey(wide(path));
                    if (!key.empty()) loadedCategories[key] = category;
                }
            }
        } catch (const std::exception&) {
            // One malformed UTF-8 record does not discard other assignments.
        }
    }
    if (input.bad()) {
        setError(error, L"Could not finish reading saved categories.");
        return false;
    }
    settings = std::move(loadedSettings);
    categories_ = std::move(loadedCategories);
    return true;
}

bool MetadataStore::save(std::wstring* error) const {
    if (error) error->clear();
    std::filesystem::path temporary;
    try {
        std::error_code ec;
        if (!file_.parent_path().empty()) std::filesystem::create_directories(file_.parent_path(), ec);
        if (ec) throw std::system_error(ec);
        temporary = file_;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        temporary += ".tmp-" + std::to_string(nonce);
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.imbue(std::locale::classic());
        if (!out) throw std::runtime_error("Cannot create metadata temporary file");
        out << "ABSER 1\n"
            << "S volume " << std::setprecision(9) << settings.volume << '\n'
            << "S folder " << std::quoted(utf8(settings.lastFolder)) << '\n'
            << "S x " << settings.windowX << '\n'
            << "S y " << settings.windowY << '\n'
            << "S width " << settings.windowWidth << '\n'
            << "S height " << settings.windowHeight << '\n'
            << "S sort " << static_cast<int>(settings.sortColumn) << '\n'
            << "S ascending " << static_cast<int>(settings.ascending) << '\n'
            << "S mask " << settings.categoryMask << '\n';
        for (size_t i = 0; i < settings.columnWidths.size(); ++i)
            out << "S col" << i << ' ' << settings.columnWidths[i] << '\n';
        // Stable ordering makes the human-readable store easy to inspect or back up.
        std::vector<std::pair<std::wstring, int>> records(categories_.begin(), categories_.end());
        std::sort(records.begin(), records.end());
        for (const auto& record : records)
            out << "C " << record.second << ' ' << std::quoted(utf8(record.first)) << '\n';
        out.flush();
        if (!out) throw std::runtime_error("Cannot write metadata");
        out.close();
        if (!out) throw std::runtime_error("Cannot close metadata");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace metadata");
#else
        std::filesystem::rename(temporary, file_);
#endif
        return true;
    } catch (const std::exception&) {
        std::error_code ec;
        if (!temporary.empty()) std::filesystem::remove(temporary, ec);
        setError(error, L"Could not save categories in AppData. Check that it is writable.");
        return false;
    }
}

int MetadataStore::category(const std::wstring& path) const {
    const auto found = categories_.find(categoryKey(path));
    return found == categories_.end() ? 0 : found->second;
}

void MetadataStore::setCategory(const std::wstring& path, int category) {
    if (category < 0 || category >= static_cast<int>(kCategoryCount) || path.empty()) return;
    const std::wstring key = categoryKey(path);
    if (key.empty()) return;
    if (category == 0) categories_.erase(key);
    else categories_[key] = category;
}

std::wstring normalizeDirectoryInput(const std::wstring& input) {
    std::wstring clean = trim(input);
    if (clean.size() >= 2 && clean.front() == L'"' && clean.back() == L'"')
        clean = clean.substr(1, clean.size() - 2);
    if (clean.empty() || clean.find(L'\0') != std::wstring::npos) return {};
    clean = expandEnvironment(std::move(clean));
    try {
#ifdef _WIN32
        const DWORD size = GetFullPathNameW(clean.c_str(), 0, nullptr, nullptr);
        if (!size || size > 32768) return {};
        std::wstring full(size, L'\0');
        const DWORD written = GetFullPathNameW(clean.c_str(), size, full.data(), nullptr);
        if (!written || written >= size) return {};
        full.resize(written);
        auto p = pathFromWide(full).lexically_normal();
#else
        auto p = std::filesystem::absolute(pathFromWide(clean)).lexically_normal();
#endif
        if (p != p.root_path() && p.filename().empty()) p = p.parent_path();
        return pathToWide(p);
    } catch (const std::exception&) { return {}; }
}

bool isAudioFile(const std::wstring& filename) {
    const size_t dot = filename.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const auto ext = lowercase(filename.substr(dot));
    return ext == L".wav" || ext == L".wave" || ext == L".mp3" ||
           ext == L".flac" || ext == L".ogg";
}

ScanResult scanDirectory(const std::wstring& input, const MetadataStore& metadata) {
    ScanResult result;
    result.folder = normalizeDirectoryInput(input);
    if (result.folder.empty()) {
        result.error = L"Enter a folder path.";
        return result;
    }
    try {
#ifdef _WIN32
        std::wstring nativeFolder = result.folder;
        if (nativeFolder.compare(0, 4, L"\\\\?\\") != 0) {
            if (nativeFolder.compare(0, 2, L"\\\\") == 0) nativeFolder = L"\\\\?\\UNC\\" + nativeFolder.substr(2);
            else nativeFolder = L"\\\\?\\" + nativeFolder;
        }
        const DWORD attributes = GetFileAttributesW(nativeFolder.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            result.error = L"Folder not found or unavailable.";
            return result;
        }
        std::wstring pattern = nativeFolder;
        if (pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found,
                                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) result.error = L"Could not read this folder.";
            return result;
        }
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !isAudioFile(found.cFileName)) continue;
            FileEntry entry;
            entry.name = found.cFileName;
            entry.path = result.folder;
            if (entry.path.back() != L'\\') entry.path += L'\\';
            entry.path += entry.name;
            const uint64_t ticks = (static_cast<uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) |
                                   found.ftLastWriteTime.dwLowDateTime;
            entry.modified = static_cast<int64_t>(ticks / 10000000ULL) - 11644473600LL;
            entry.category = metadata.category(entry.path);
            result.files.push_back(std::move(entry));
        } while (FindNextFileW(search, &found));
        const DWORD endError = GetLastError();
        FindClose(search);
        if (endError != ERROR_NO_MORE_FILES) result.error = L"Some files could not be read from this folder.";
#else
        std::error_code ec;
        const auto dir = pathFromWide(result.folder);
        if (!std::filesystem::is_directory(dir, ec) || ec) {
            result.error = L"Folder not found or unavailable.";
            return result;
        }
        std::filesystem::directory_iterator it(dir, ec), end;
        if (ec) { result.error = L"Could not read this folder."; return result; }
        for (; it != end; it.increment(ec)) {
            if (ec) { result.error = L"Some files could not be read from this folder."; break; }
            const auto& file = *it;
            const auto name = pathToWide(file.path().filename());
            if (!isAudioFile(name) || !file.is_regular_file(ec) || ec) { ec.clear(); continue; }
            FileEntry entry;
            entry.path = pathToWide(file.path());
            entry.name = name;
            entry.category = metadata.category(entry.path);
            const auto modified = file.last_write_time(ec);
            if (!ec) {
                const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    modified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                entry.modified = std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
            }
            ec.clear();
            result.files.push_back(std::move(entry));
        }
#endif
        sortFiles(result.files, metadata.settings.sortColumn, metadata.settings.ascending);
    } catch (const std::exception&) { result.error = L"Could not read this folder."; }
    return result;
}

int naturalCompare(const std::wstring& left, const std::wstring& right) {
#ifdef _WIN32
    const int result = StrCmpLogicalW(left.c_str(), right.c_str());
    return (result > 0) - (result < 0);
#else
    size_t i = 0, j = 0;
    while (i < left.size() && j < right.size()) {
        if (left[i] >= L'0' && left[i] <= L'9' && right[j] >= L'0' && right[j] <= L'9') {
            size_t endLeft = i, endRight = j;
            while (endLeft < left.size() && left[endLeft] >= L'0' && left[endLeft] <= L'9') ++endLeft;
            while (endRight < right.size() && right[endRight] >= L'0' && right[endRight] <= L'9') ++endRight;
            size_t digitsLeft = i, digitsRight = j;
            while (digitsLeft + 1 < endLeft && left[digitsLeft] == L'0') ++digitsLeft;
            while (digitsRight + 1 < endRight && right[digitsRight] == L'0') ++digitsRight;
            const size_t lengthLeft = endLeft - digitsLeft, lengthRight = endRight - digitsRight;
            if (lengthLeft != lengthRight) return lengthLeft < lengthRight ? -1 : 1;
            const int numeric = left.compare(digitsLeft, lengthLeft, right, digitsRight, lengthRight);
            if (numeric) return numeric < 0 ? -1 : 1;
            i = endLeft; j = endRight;
        } else {
            const wchar_t a = static_cast<wchar_t>(std::towlower(left[i++]));
            const wchar_t b = static_cast<wchar_t>(std::towlower(right[j++]));
            if (a != b) return a < b ? -1 : 1;
        }
    }
    if (i == left.size() && j == right.size()) return 0;
    return i == left.size() ? -1 : 1;
#endif
}

void sortFiles(std::vector<FileEntry>& files, SortColumn column, bool ascending) {
    std::stable_sort(files.begin(), files.end(), [=](const FileEntry& a, const FileEntry& b) {
        int compare = 0;
        switch (column) {
        case SortColumn::Category: compare = (a.category > b.category) - (a.category < b.category); break;
        case SortColumn::Duration:
            // Unknown durations always remain after known durations, in either direction.
            if ((a.duration < 0) != (b.duration < 0)) return a.duration >= 0;
            compare = (a.duration > b.duration) - (a.duration < b.duration); break;
        case SortColumn::Modified: compare = (a.modified > b.modified) - (a.modified < b.modified); break;
        case SortColumn::Name: compare = naturalCompare(a.name, b.name); break;
        }
        if (compare) return ascending ? compare < 0 : compare > 0;
        const int nameOrder = naturalCompare(a.name, b.name);
        if (nameOrder) return ascending ? nameOrder < 0 : nameOrder > 0;
        return ascending ? a.path < b.path : a.path > b.path;
    });
}

std::vector<FileEntry> filterFiles(const std::vector<FileEntry>& files, uint16_t categoryMask) {
    std::vector<FileEntry> filtered;
    filtered.reserve(files.size());
    for (const auto& file : files)
        if (file.category >= 0 && file.category < static_cast<int>(kCategoryCount) &&
            (categoryMask & (1u << file.category))) filtered.push_back(file);
    return filtered;
}

} // namespace abser

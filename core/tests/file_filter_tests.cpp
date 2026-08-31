#include <doctest/doctest.h>

#include "shell/file_dialogs.hpp"

#include <string>
#include <vector>

using namespace midi_composer;

namespace {

// The Win32 filter is a run of NUL-terminated strings ending in an empty one,
// which is exactly the thing ordinary string comparison cannot see. Split it
// back into entries so a test can say what it means.
std::vector<std::wstring> entries(const std::wstring& filter) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start < filter.size()) {
        const size_t end = filter.find(L'\0', start);
        if (end == std::wstring::npos) break;
        if (end == start) break;              // the empty entry that ends the list
        out.push_back(filter.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

/** Whether the filter is terminated the way GetOpenFileNameW requires. */
bool properly_terminated(const std::wstring& filter) {
    return filter.size() >= 2 && filter[filter.size() - 1] == L'\0' &&
           filter[filter.size() - 2] == L'\0';
}

} // namespace

TEST_CASE("One declared pattern becomes a labelled entry plus All Files") {
    const auto filter = shell::make_file_filter("*.spc");
    CHECK(properly_terminated(filter));
    CHECK(entries(filter) == std::vector<std::wstring>{
        L"Supported files (*.spc)", L"*.spc", L"All Files (*.*)", L"*.*"});
}

TEST_CASE("Several patterns are joined into one entry") {
    const auto filter = shell::make_file_filter("*.spc;*.sf2");
    CHECK(entries(filter) == std::vector<std::wstring>{
        L"Supported files (*.spc;*.sf2)", L"*.spc;*.sf2", L"All Files (*.*)", L"*.*"});
}

TEST_CASE("Whitespace and empty pieces are dropped") {
    const auto filter = shell::make_file_filter("  *.spc ;; *.sf2  ;");
    CHECK(entries(filter) == std::vector<std::wstring>{
        L"Supported files (*.spc;*.sf2)", L"*.spc;*.sf2", L"All Files (*.*)", L"*.*"});
}

TEST_CASE("Declaring nothing offers All Files rather than an empty list") {
    // A parameter with no filter is a parameter that takes any file, and a
    // dialog showing nothing would look like a broken dialog.
    for (const auto* pattern : {"", "   ", ";;;"}) {
        const auto filter = shell::make_file_filter(pattern);
        CHECK(properly_terminated(filter));
        CHECK(entries(filter) == std::vector<std::wstring>{L"All Files (*.*)", L"*.*"});
    }
}

TEST_CASE("All Files is always last, so it can never hide the declared pattern") {
    const auto declared = entries(shell::make_file_filter("*.sf2"));
    REQUIRE(declared.size() == 4);
    CHECK(declared[1] == L"*.sf2");
    CHECK(declared[3] == L"*.*");
}

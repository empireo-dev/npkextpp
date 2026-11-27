#include "npk/inputs.hpp"

#include "npk/paths.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace fs = std::filesystem;

namespace npk {

namespace {

// --- Wildcards -----------------------------------------------------------------

struct Token {
    enum Kind { literal, any_one, any_run, set } kind = literal;
    char c = 0;
    bool negated = false;
    std::string members;                           // single characters
    std::vector<std::pair<char, char>> ranges;     // inclusive

    [[nodiscard]] bool matches(char x) const
    {
        switch (kind) {
        case literal: return x == c;
        case any_one: return true;
        case any_run: return true;
        case set: {
            bool in = members.find(x) != std::string::npos;
            for (const auto& [lo, hi] : ranges) in = in || (x >= lo && x <= hi);
            return in != negated;
        }
        }
        return false;
    }
};

Token make_token(Token::Kind kind, char c = 0)
{
    Token token;
    token.kind = kind;
    token.c = c;
    return token;
}

std::vector<Token> tokenize(std::string_view pattern)
{
    std::vector<Token> tokens;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*') {
            if (tokens.empty() || tokens.back().kind != Token::any_run) tokens.push_back(make_token(Token::any_run));
        } else if (c == '?') {
            tokens.push_back(make_token(Token::any_one));
        } else if (c == '[') {
            // Find the closing ']' ('!' and a ']' right after '[' are part of the set).
            std::size_t j = i + 1;
            if (j < pattern.size() && pattern[j] == '!') ++j;
            if (j < pattern.size() && pattern[j] == ']') ++j;
            while (j < pattern.size() && pattern[j] != ']') ++j;
            if (j >= pattern.size()) {
                tokens.push_back(make_token(Token::literal, '['));   // no closing bracket: a plain '['
                continue;
            }
            Token token = make_token(Token::set);
            std::size_t k = i + 1;
            if (pattern[k] == '!') {
                token.negated = true;
                ++k;
            }
            while (k < j) {
                if (k + 2 < j && pattern[k + 1] == '-') {
                    token.ranges.emplace_back(pattern[k], pattern[k + 2]);
                    k += 3;
                } else {
                    token.members += pattern[k];
                    ++k;
                }
            }
            tokens.push_back(std::move(token));
            i = j;
        } else {
            tokens.push_back(make_token(Token::literal, c));
        }
    }
    return tokens;
}

// --- difflib-style similarity -------------------------------------------------------

struct Match {
    std::size_t i = 0, j = 0, size = 0;
};

// Longest common block of a[alo,ahi) and b[blo,bhi); ties go to the earliest in a,
// then the earliest in b (as difflib.SequenceMatcher.find_longest_match).
Match longest_match(std::string_view a, std::string_view b, std::size_t alo, std::size_t ahi, std::size_t blo,
                    std::size_t bhi)
{
    Match best{alo, blo, 0};
    std::vector<std::size_t> previous(b.size() + 1, 0), current(b.size() + 1, 0);
    for (std::size_t i = alo; i < ahi; ++i) {
        std::ranges::fill(current, 0);
        for (std::size_t j = blo; j < bhi; ++j) {
            if (a[i] != b[j]) continue;
            const std::size_t k = (j > blo ? previous[j - 1] : 0) + 1;
            current[j] = k;
            if (k > best.size) best = {i + 1 - k, j + 1 - k, k};
        }
        std::swap(previous, current);
    }
    return best;
}

std::size_t matching_characters(std::string_view a, std::string_view b, std::size_t alo, std::size_t ahi,
                                std::size_t blo, std::size_t bhi)
{
    const Match m = longest_match(a, b, alo, ahi, blo, bhi);
    if (m.size == 0) return 0;
    std::size_t total = m.size;
    if (alo < m.i && blo < m.j) total += matching_characters(a, b, alo, m.i, blo, m.j);
    if (m.i + m.size < ahi && m.j + m.size < bhi) total += matching_characters(a, b, m.i + m.size, ahi, m.j + m.size, bhi);
    return total;
}

double similarity(std::string_view a, std::string_view b)
{
    const std::size_t length = a.size() + b.size();
    if (length == 0) return 1.0;
    return 2.0 * static_cast<double>(matching_characters(a, b, 0, a.size(), 0, b.size())) / static_cast<double>(length);
}

// --- Globbing ------------------------------------------------------------------------

bool has_wildcard(std::string_view part)
{
    return part.find_first_of("*?[") != std::string_view::npos;
}

bool part_matches(const fs::path& name, std::string_view pattern)
{
#if defined(_WIN32)
    return wildcard_match(lower_ascii(to_utf8(name)), lower_ascii(pattern));   // Windows names ignore case
#else
    return wildcard_match(to_utf8(name), pattern);
#endif
}

void glob_walk(const fs::path& folder, std::span<const std::string> parts, std::vector<fs::path>& out)
{
    if (parts.empty()) {
        out.push_back(folder);
        return;
    }
    const std::string& part = parts.front();
    const auto rest = parts.subspan(1);
    std::error_code error;

    if (part == "**") {
        glob_walk(folder, rest, out);
        fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error);
        for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            std::error_code type_error;
            if (it->is_directory(type_error)) glob_walk(it->path(), rest, out);
        }
    } else if (part == ".") {
        glob_walk(folder, rest, out);
    } else if (part == "..") {
        glob_walk(folder / "..", rest, out);
    } else if (has_wildcard(part)) {
        fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error);
        for (; !error && it != fs::directory_iterator(); it.increment(error)) {
            if (part_matches(it->path().filename(), part)) glob_walk(it->path(), rest, out);
        }
    } else {
        const fs::path next = folder / from_utf8(part);
        if (fs::exists(next, error)) glob_walk(next, rest, out);
    }
}

bool is_file(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

bool name_matches(const fs::path& archive, std::string_view pattern)
{
    const std::string name = lower_ascii(to_utf8(archive.filename()));
    const std::string lowered = lower_ascii(pattern);
    return wildcard_match(name, lowered) || wildcard_match(name, lowered + ".npk");
}

void drop_duplicates(std::vector<fs::path>& paths)
{
    std::set<fs::path> seen;
    std::erase_if(paths, [&seen](const fs::path& path) { return !seen.insert(path).second; });
}

} // namespace

bool wildcard_match(std::string_view text, std::string_view pattern)
{
    const std::vector<Token> tokens = tokenize(pattern);
    std::size_t t = 0, p = 0;
    std::size_t star = std::string_view::npos, star_text = 0;
    while (t < text.size()) {
        if (p < tokens.size() && tokens[p].kind == Token::any_run) {
            star = p++;
            star_text = t;
        } else if (p < tokens.size() && tokens[p].matches(text[t])) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++star_text;
        } else {
            return false;
        }
    }
    while (p < tokens.size() && tokens[p].kind == Token::any_run) ++p;
    return p == tokens.size();
}

std::vector<std::string> close_matches(std::string_view word, std::span<const std::string> candidates, std::size_t n,
                                       double cutoff)
{
    std::vector<std::pair<double, std::string>> scored;
    for (const std::string& candidate : candidates) {
        const double score = similarity(candidate, word);
        if (score >= cutoff) scored.emplace_back(score, candidate);
    }
    std::ranges::sort(scored, std::greater<>{});
    if (scored.size() > n) scored.resize(n);
    std::vector<std::string> out;
    for (auto& [score, candidate] : scored) out.push_back(std::move(candidate));
    return out;
}

std::vector<fs::path> glob(const fs::path& root, std::string_view pattern)
{
    std::vector<std::string> parts;
    std::string normalized(pattern);
    std::ranges::replace(normalized, '\\', '/');
    std::size_t start = 0;
    while (start <= normalized.size()) {
        std::size_t end = normalized.find('/', start);
        if (end == std::string::npos) end = normalized.size();
        if (end > start) parts.push_back(normalized.substr(start, end - start));
        start = end + 1;
    }
    std::vector<fs::path> out;
    glob_walk(root, parts, out);
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

Result<std::vector<fs::path>> filter_by_name(std::span<const fs::path> archives, std::span<const std::string> patterns)
{
    std::vector<fs::path> selected;
    std::vector<std::string> unmatched;
    for (const std::string& pattern : patterns) {
        bool any = false;
        for (const fs::path& archive : archives) {
            if (name_matches(archive, pattern)) {
                selected.push_back(archive);
                any = true;
            }
        }
        if (!any) unmatched.push_back(pattern);
    }

    if (!unmatched.empty()) {
        std::set<std::string> names;
        for (const fs::path& archive : archives) names.insert(to_utf8(archive.filename()));
        const std::vector<std::string> known(names.begin(), names.end());
        std::string message;
        for (const std::string& pattern : unmatched) {
            if (!message.empty()) message += '\n';
            message += "no archive named '" + pattern + "'";
            const std::vector<std::string> close = close_matches(pattern, known, 4, 0.5);
            if (!close.empty()) {
                message += " (did you mean ";
                for (std::size_t i = 0; i < close.size(); ++i) message += (i ? ", " : "") + close[i];
                message += "?)";
            }
        }
        return make_error(Errc::invalid_input, std::move(message));
    }
    drop_duplicates(selected);
    return selected;
}

Result<std::vector<fs::path>> resolve_inputs(std::span<const std::string> inputs, const Installation* installation)
{
    std::vector<fs::path> found;
    std::vector<std::string> names;
    for (const std::string& value : inputs) {
        const fs::path path = from_utf8(value);
        std::error_code error;
        if (is_file(path)) {
            found.push_back(path);
        } else if (fs::is_directory(path, error)) {
            // A folder: every archive below it (folders marked .noscan left out).
            const FolderScan scan = scan_folder(path);
            if (scan.archives.empty()) {
                return make_error(Errc::invalid_input, "no .npk archives in the folder " + value);
            }
            found.insert(found.end(), scan.archives.begin(), scan.archives.end());
        } else if (installation == nullptr || path.is_absolute()) {
            return make_error(Errc::invalid_input,
                              "archive not found: " + value +
                                  (installation ? "" : " (to search by name, also give --install DIR)"));
        } else if (value.find_first_of("/\\") != std::string::npos) {
            std::vector<fs::path> matches;
            for (fs::path& match : glob(installation->root(), value)) {
                if (is_file(match) && iequals_ascii(to_utf8(match.extension()), ".npk")) {
                    matches.push_back(std::move(match));
                }
            }
            if (matches.empty()) {
                return make_error(Errc::invalid_input,
                                  "no archive at '" + value + "' in " + to_utf8(installation->root()));
            }
            found.insert(found.end(), matches.begin(), matches.end());
        } else {
            names.push_back(value);
        }
    }

    if (!names.empty()) {
        auto by_name = filter_by_name(installation->archives(), names);
        if (!by_name) return std::unexpected(std::move(by_name.error()));
        found.insert(found.end(), by_name->begin(), by_name->end());
    }
    drop_duplicates(found);
    return found;
}

} // namespace npk

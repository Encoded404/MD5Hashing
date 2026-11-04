module solve.detail;

import std;

namespace solve::detail {

namespace {

constexpr std::array<std::uint8_t, 256> MakeDelimiterTable() {
    std::array<std::uint8_t, 256> table{};
    for (const char ch : {' ', '\t', '\r', '\n', ',', ';', '|', '/',
                          '[', ']', '{', '}', ':', '"', '(', ')'}) {
        table[static_cast<unsigned char>(ch)] = 1;
    }
    return table;
}

constexpr auto kDelimiterTable = MakeDelimiterTable();

}  // namespace

WordlistReader::WordlistReader(const std::filesystem::path& path)
    : file_(path, std::ios::binary) {
    if (!file_) {
        error_ = "cannot open wordlist: " + path.string();
        return;
    }

    std::array<unsigned char, 4> probe{};
    file_.read(reinterpret_cast<char*>(probe.data()),
               static_cast<std::streamsize>(probe.size()));
    const std::streamsize got = file_.gcount();
    file_.clear();

    if (got >= 2 && probe[0] == 0x1F && probe[1] == 0x8B) {
        error_ = "wordlist is gzip-compressed; decompress it first";
        return;
    }
    if (got >= 2 && ((probe[0] == 0xFF && probe[1] == 0xFE) ||
                     (probe[0] == 0xFE && probe[1] == 0xFF))) {
        error_ = "wordlist is UTF-16; convert it to UTF-8";
        return;
    }

    std::streamoff offset = 0;
    if (got >= 3 && probe[0] == 0xEF && probe[1] == 0xBB && probe[2] == 0xBF) {
        // UTF-8 BOM: start reading after it.
        offset = 3;
    } else {
        for (std::streamsize i = 0; i < got; ++i) {
            if (probe[i] == 0) {
                error_ = "wordlist looks binary or UTF-16; convert it to UTF-8";
                return;
            }
        }
    }

    file_.seekg(offset);
    if (!file_) {
        error_ = "cannot read wordlist: " + path.string();
    }
}

bool WordlistReader::next(std::string& token) {
    token.clear();
    while (true) {
        if (pos_ == end_ && !fill()) {
            // A final token need not be followed by a delimiter.
            return !token.empty();
        }

        if (at_line_start_) {
            const char ch = buffer_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\r') {
                ++pos_;
                continue;
            }
            if (ch == '\n') {
                ++pos_;
                continue;
            }
            if (ch == '#') {
                skip_comment();
                continue;
            }
            if (ch == '/') {
                // "//" starts a comment only at the start of a line.
                // The second byte may live in the next chunk.
                if (pos_ + 1 == end_) {
                    if (!eof_ && fill()) {
                        continue;
                    }
                    // A lone '/' at end of file is just a delimiter.
                    at_line_start_ = false;
                    ++pos_;
                    continue;
                }
                if (buffer_[pos_ + 1] == '/') {
                    skip_comment();
                    continue;
                }
            }
            at_line_start_ = false;
        }

        const char ch = buffer_[pos_];
        if (IsDelimiter(ch)) {
            ++pos_;
            if (ch == '\n') {
                at_line_start_ = true;
            }
            continue;
        }

        const std::size_t begin = pos_;
        while (pos_ < end_ && !IsDelimiter(buffer_[pos_])) {
            ++pos_;
        }

        token.append(buffer_.data() + begin, pos_ - begin);
        if (pos_ == end_) {
            // The token may continue in the next chunk.
            // What has been copied is already in `token`, and `fill` compacts the unread tail to the front.
            if (!fill()) {
                return true;
            }
            // The token ended exactly at the old chunk boundary if the first new byte is a delimiter.
            // Consume it and finish the token instead of merging the next entry into this one.
            if (IsDelimiter(buffer_[pos_])) {
                const char delimiter = buffer_[pos_];
                ++pos_;
                if (delimiter == '\n') {
                    at_line_start_ = true;
                }
                return true;
            }
            continue;
        }
        const char delimiter = buffer_[pos_];
        ++pos_;
        if (delimiter == '\n') {
            at_line_start_ = true;
        }
        return true;
    }
}

bool WordlistReader::IsDelimiter(char ch) {
    return kDelimiterTable[static_cast<unsigned char>(ch)] != 0;
}

bool WordlistReader::fill() {
    if (pos_ > 0) {
        const std::size_t remaining = end_ - pos_;
        if (remaining != 0) {
            std::memmove(buffer_.data(), buffer_.data() + pos_, remaining);
        }
        end_ = remaining;
        pos_ = 0;
    }
    if (eof_) {
        return end_ != 0;
    }
    if (buffer_.empty()) {
        // Header-only validation (PlanSearch) never calls this, so a 256 KiB buffer is allocated only when the file is actually read.
        buffer_.resize(kReadChunk);
    }
    if (end_ == buffer_.size()) {
        buffer_.resize(buffer_.size() * 2);
    }
    const std::size_t want = buffer_.size() - end_;
    file_.read(buffer_.data() + end_, static_cast<std::streamsize>(want));
    const std::streamsize got = file_.gcount();
    if (got <= 0) {
        eof_ = true;
        if (file_.bad()) {
            error_ = "cannot read wordlist";
        }
        return end_ != 0;
    }
    end_ += static_cast<std::size_t>(got);
    if (static_cast<std::size_t>(got) < want) {
        eof_ = true;
        if (file_.bad()) {
            error_ = "cannot read wordlist";
        }
    }
    return true;
}

void WordlistReader::skip_comment() {
    while (true) {
        if (pos_ == end_ && !fill()) {
            at_line_start_ = true;
            return;
        }
        const void* newline = std::memchr(buffer_.data() + pos_, '\n', end_ - pos_);
        if (newline != nullptr) {
            pos_ = static_cast<std::size_t>(
                       static_cast<const char*>(newline) - buffer_.data()) +
                   1;
            at_line_start_ = true;
            return;
        }
        pos_ = end_;
    }
}

std::optional<WordlistCount> CountWordlistEntries(WordlistReader& reader,
                                                  std::uint64_t limit,
                                                  std::string& error) {
    WordlistCount count;
    std::string token;
    while (reader.next(token)) {
        ++count.entries;
        if (count.entries == limit) {
            count.reached_limit = true;
            break;
        }
    }
    if (!reader.error().empty()) {
        error = reader.error();
        return std::nullopt;
    }
    return count;
}

}  // namespace solve::detail

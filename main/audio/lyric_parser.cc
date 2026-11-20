#include "lyric_parser.h"
#include <algorithm>
#include <cstdlib>
#include <cctype>

int LyricParser::ParseTimestamp(const std::string& tag) {
    // tag 形如 "01:23.45" 或 "01:23.456"（不含方括号）
    // 也容许 "01:23"（无百分秒）
    // 严格要求冒号左侧是纯数字，避免把 [ar:周杰伦] / [ti:歌名] 等元数据当成 0ms 时间戳
    size_t colon = tag.find(':');
    if (colon == std::string::npos || colon == 0) return -1;
    for (size_t k = 0; k < colon; ++k) {
        if (!isdigit((unsigned char)tag[k])) return -1;
    }

    int min = atoi(tag.substr(0, colon).c_str());
    std::string sec_part = tag.substr(colon + 1);

    size_t dot = sec_part.find('.');
    int sec = 0;
    int ms = 0;
    if (dot == std::string::npos) {
        for (char c : sec_part) {
            if (!isdigit((unsigned char)c)) return -1;
        }
        sec = atoi(sec_part.c_str());
    } else {
        std::string sec_only = sec_part.substr(0, dot);
        if (sec_only.empty()) return -1;
        for (char c : sec_only) {
            if (!isdigit((unsigned char)c)) return -1;
        }
        sec = atoi(sec_only.c_str());
        std::string frac = sec_part.substr(dot + 1);
        if (frac.empty()) return -1;
        for (char c : frac) {
            if (!isdigit((unsigned char)c)) return -1;
        }
        // 补齐/截断到 3 位毫秒
        while (frac.length() < 3) frac += '0';
        if (frac.length() > 3) frac = frac.substr(0, 3);
        ms = atoi(frac.c_str());
    }
    return min * 60000 + sec * 1000 + ms;
}

bool LyricParser::Parse(const std::string& lrc_text) {
    lines_.clear();

    size_t pos = 0;
    while (pos < lrc_text.size()) {
        size_t end = lrc_text.find('\n', pos);
        if (end == std::string::npos) end = lrc_text.size();
        std::string line = lrc_text.substr(pos, end - pos);
        pos = end + 1;

        // 去掉行尾 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // 收集本行所有时间标签 [mm:ss.xx]，文本是最后一个 ']' 之后的内容
        std::vector<int> timestamps;
        size_t last_bracket = std::string::npos;
        size_t i = 0;
        while (i < line.size()) {
            if (line[i] == '[') {
                size_t close = line.find(']', i);
                if (close == std::string::npos) break;
                std::string tag = line.substr(i + 1, close - i - 1);
                int ts = ParseTimestamp(tag);
                if (ts >= 0) {
                    timestamps.push_back(ts);
                }
                last_bracket = close;
                i = close + 1;
            } else {
                i++;
            }
        }

        // 没有有效时间戳（如 [ar:歌手] 元数据、空行、注释）→ 跳过
        if (timestamps.empty()) continue;

        // 文本 = 最后一个时间标签之后的剩余内容
        std::string text;
        if (last_bracket != std::string::npos && last_bracket + 1 < line.size()) {
            text = line.substr(last_bracket + 1);
        }
        // 去掉首尾空白
        size_t b = text.find_first_not_of(" \t");
        if (b == std::string::npos) {
            text.clear();
        } else {
            size_t e = text.find_last_not_of(" \t");
            text = text.substr(b, e - b + 1);
        }

        // 一个时间戳生成一行歌词
        for (int ts : timestamps) {
            lines_.push_back({ts, text});
        }
    }

    // 按时间升序排序（同一时间的多行保持稳定顺序）
    std::stable_sort(lines_.begin(), lines_.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.time_ms < b.time_ms; });

    return !lines_.empty();
}

int LyricParser::GetLineIndexAt(int position_ms) const {
    if (lines_.empty()) return -1;
    // 前奏：还没到第一行时间
    if (position_ms < lines_[0].time_ms) return -1;

    // 二分查找最后一个 time_ms <= position_ms 的行
    int lo = 0, hi = (int)lines_.size() - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (lines_[mid].time_ms <= position_ms) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

std::string LyricParser::GetLineText(int index) const {
    if (index < 0 || index >= (int)lines_.size()) return "";
    return lines_[index].text;
}

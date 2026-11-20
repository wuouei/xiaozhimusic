#ifndef LYRIC_PARSER_H
#define LYRIC_PARSER_H

#include <string>
#include <vector>
#include <cstdint>

// 单行歌词：时间戳（毫秒）+ 文本
struct LyricLine {
    int time_ms;        // 该行开始时间（毫秒，相对歌曲开头）
    std::string text;   // 歌词文本（可能为空，表示间奏）
};

// LRC 歌词解析器
// 支持标准 LRC 格式：[mm:ss.xx]歌词文本  或  [mm:ss.xxx]歌词文本
// 也支持一行多个时间戳、以及 [ar:]/[ti:] 等元数据标签（自动忽略）
class LyricParser {
public:
    // 解析 LRC 文本，结果按时间升序排列。返回是否解析出至少一行有效歌词。
    bool Parse(const std::string& lrc_text);

    // 根据播放进度（毫秒）返回当前应显示的行索引。
    // 若 position < 第一行时间，返回 -1（尚未开始，如歌曲前奏）。
    int GetLineIndexAt(int position_ms) const;

    // 获取指定行文本；索引越界返回空串
    std::string GetLineText(int index) const;

    // 获取歌词总行数
    size_t size() const { return lines_.size(); }

    // 清空
    void Clear() { lines_.clear(); }

    // 是否有歌词
    bool empty() const { return lines_.empty(); }

private:
    // 解析 [mm:ss.xx] 形式的时间标签，成功返回毫秒数，失败返回 -1
    static int ParseTimestamp(const std::string& tag);

    std::vector<LyricLine> lines_;
};

#endif // LYRIC_PARSER_H

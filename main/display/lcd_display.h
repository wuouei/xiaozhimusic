#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>

// Theme color structure
struct ThemeColors {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
    lv_color_t accent;        // 霓虹强调色（科幻主题的状态栏/边框高亮）
    lv_color_t lyric_highlight; // 歌词当前行高亮色
    lv_color_t lyric_panel_bg;  // 歌词面板背景色
};


class LcdDisplay : public Display {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* sci_fi_decor_ = nullptr;   // 待机界面科幻装饰层（圆环/角标/点阵）

    DisplayFonts fonts_;
    ThemeColors current_theme_;

    void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // 添加protected构造函数
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, DisplayFonts fonts, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetIcon(const char* icon) override;
    virtual void SetPreviewImage(const lv_img_dsc_t* img_dsc) override;
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    virtual void SetChatMessage(const char* role, const char* content) override; 
#endif  

    // Add theme switching function
    virtual void SetTheme(const std::string& theme_name) override;

    // 歌词显示（底部叠加层，懒创建）
    virtual void ShowLyric(int line_index, const std::string& text,
                           const std::string& prev_text, const std::string& next_text,
                           const std::string& song, const std::string& artist) override;

private:
    lv_obj_t* lyric_panel_ = nullptr;   // 歌词背景面板（全屏）
    lv_obj_t* lyric_focus_bg_ = nullptr; // 当前行高亮发光框（突出显示正在播放的歌词）
    lv_obj_t* lyric_focus_bar_ = nullptr; // 高亮框左侧播放指示条
    lv_obj_t* lyric_label_ = nullptr;   // 当前歌词行
    lv_obj_t* lyric_prev_label_ = nullptr;  // 上一行
    lv_obj_t* lyric_next_label_ = nullptr;  // 下一行
    lv_obj_t* lyric_info_label_ = nullptr;  // 歌曲名-歌手（只在开头显示几秒）
    lv_obj_t* lyric_eq_bars_[9] = {nullptr}; // 终端风格均衡器装饰条
    lv_obj_t* lyric_progress_fill_ = nullptr; // 进度条亮绿填充
    lv_timer_t* lyric_eq_timer_ = nullptr;   // 均衡器跳动动画定时器
    lv_timer_t* lyric_info_timer_ = nullptr;  // 歌名自动隐藏定时器
    std::string lyric_info_text_;             // 已显示的歌名，避免重复触发
    void EnsureLyricPanel();
    void ShowLyricInfo(const std::string& info);  // 显示歌名并重启隐藏定时器
    void HideLyricInfo();                         // 隐藏歌名并回收其占用的高度
    void BuildSciFiDecor();                       // 构建待机界面科幻装饰层（仅科幻主题）
};

// RGB LCD显示器
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,
                  DisplayFonts fonts);
};

// MIPI LCD显示器
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   DisplayFonts fonts);
};

// // SPI LCD显示器
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,
                  DisplayFonts fonts);
};

// QSPI LCD显示器
class QspiLcdDisplay : public LcdDisplay {
public:
    QspiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   DisplayFonts fonts);
};

// MCU8080 LCD显示器
class Mcu8080LcdDisplay : public LcdDisplay {
public:
    Mcu8080LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, int offset_x, int offset_y,
                      bool mirror_x, bool mirror_y, bool swap_xy,
                      DisplayFonts fonts);
};
#endif // LCD_DISPLAY_H

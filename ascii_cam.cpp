// MIT License
//
// Copyright (c) 2025
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* ASCII_CHARS = " .,:;irsXA253hMHGS#9B&@";
constexpr double CHAR_ASPECT = 0.55;
constexpr double FONT_SCALE = 1.0;
constexpr const char* RESET = "\033[0m";
constexpr const char* CURSOR_HOME = "\033[H";
constexpr const char* CLEAR_TO_END = "\033[J";

struct TerminalSize {
    int columns = 80;
    int rows = 24;
};

struct AsciiFrame {
    std::string text;
    int width = 0;
    int height = 0;
};

void enable_vt_mode() {
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

class KeyboardListener {
public:
    KeyboardListener() {
#ifndef _WIN32
        fd_ = STDIN_FILENO;
        if (tcgetattr(fd_, &old_settings_) == 0) {
            termios raw = old_settings_;
            raw.c_lflag &= static_cast<unsigned int>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            enabled_ = tcsetattr(fd_, TCSADRAIN, &raw) == 0;
        }
#endif
    }

    ~KeyboardListener() {
#ifndef _WIN32
        if (enabled_) {
            tcsetattr(fd_, TCSADRAIN, &old_settings_);
        }
#endif
    }

    KeyboardListener(const KeyboardListener&) = delete;
    KeyboardListener& operator=(const KeyboardListener&) = delete;

    int poll() {
#ifdef _WIN32
        if (!_kbhit()) {
            return -1;
        }
        int key = _getwch();
        if ((key == 0 || key == 0xE0) && _kbhit()) {
            _getwch();
            return -1;
        }
        return key;
#else
        unsigned char ch = 0;
        ssize_t bytes_read = read(fd_, &ch, 1);
        if (bytes_read != 1) {
            return -1;
        }
        if (ch == '\033') {
            unsigned char ignored = 0;
            while (read(fd_, &ignored, 1) == 1) {
            }
            return '\033';
        }
        return ch;
#endif
    }

private:
#ifndef _WIN32
    int fd_ = -1;
    termios old_settings_ {};
    bool enabled_ = false;
#endif
};

TerminalSize get_terminal_size() {
    TerminalSize size;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info)) {
        size.columns = info.srWindow.Right - info.srWindow.Left + 1;
        size.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    winsize window {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
        if (window.ws_col > 0) {
            size.columns = window.ws_col;
        }
        if (window.ws_row > 0) {
            size.rows = window.ws_row;
        }
    }
#endif
    size.columns = std::max(1, size.columns);
    size.rows = std::max(1, size.rows);
    return size;
}

std::pair<int, int> compute_ascii_shape(const cv::Size& frame_size, const TerminalSize& term_size) {
    const int width = std::max(1, frame_size.width);
    const int height = std::max(1, frame_size.height);
    const int columns = std::max(1, term_size.columns);
    const int rows = std::max(1, term_size.rows);

    const double scale_width = columns / (FONT_SCALE * width);
    const double scale_height = rows / (height * CHAR_ASPECT);
    const double scale = std::min(scale_width, scale_height);
    if (scale <= 0.0) {
        return {0, 0};
    }

    int ascii_width = std::max(1, static_cast<int>(width * scale));
    int ascii_height = std::max(1, static_cast<int>(height * scale * CHAR_ASPECT));
    ascii_width = std::min(ascii_width, columns);
    ascii_height = std::min(ascii_height, rows);
    return {ascii_width, ascii_height};
}

AsciiFrame frame_to_ascii(const cv::Mat& frame, const TerminalSize& term_size, bool color_enabled) {
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    auto [ascii_width, ascii_height] = compute_ascii_shape(gray.size(), term_size);
    if (ascii_width < 1 || ascii_height < 1) {
        return {};
    }

    cv::Mat resized_gray;
    cv::resize(gray, resized_gray, cv::Size(ascii_width, ascii_height), 0, 0, cv::INTER_LINEAR);

    cv::Mat resized_color;
    if (color_enabled) {
        cv::resize(frame, resized_color, cv::Size(ascii_width, ascii_height), 0, 0, cv::INTER_LINEAR);
    }

    const int max_index = static_cast<int>(std::char_traits<char>::length(ASCII_CHARS)) - 1;
    std::ostringstream output;
    for (int y = 0; y < ascii_height; ++y) {
        for (int x = 0; x < ascii_width; ++x) {
            const int pixel = resized_gray.at<unsigned char>(y, x);
            const int index = pixel * max_index / 255;
            const char ch = ASCII_CHARS[index];

            if (color_enabled) {
                const cv::Vec3b bgr = resized_color.at<cv::Vec3b>(y, x);
                output << "\033[38;2;" << static_cast<int>(bgr[2]) << ';'
                       << static_cast<int>(bgr[1]) << ';'
                       << static_cast<int>(bgr[0]) << 'm';
            }
            output << ch;
        }
        if (color_enabled) {
            output << RESET;
        }
        if (y + 1 < ascii_height) {
            output << '\n';
        }
    }

    return {output.str(), ascii_width, ascii_height};
}

void render_frame(const AsciiFrame& ascii_frame, const TerminalSize& term_size) {
    std::cout << CURSOR_HOME << CLEAR_TO_END;
    if (!ascii_frame.text.empty()) {
        const int top_padding = std::max(0, (term_size.rows - ascii_frame.height) / 2);
        const int left_padding = std::max(0, (term_size.columns - ascii_frame.width) / 2);
        const std::string left_margin(static_cast<size_t>(left_padding), ' ');

        for (int i = 0; i < top_padding; ++i) {
            std::cout << '\n';
        }

        std::istringstream lines(ascii_frame.text);
        std::string line;
        bool first = true;
        while (std::getline(lines, line)) {
            if (!first) {
                std::cout << '\n';
            }
            first = false;
            std::cout << left_margin << line;
        }
    }
    std::cout.flush();
}

}  // namespace

int main() {
    enable_vt_mode();

    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "Unable to open camera.\n";
        return 1;
    }

    std::cout << "\033[2J";
    std::cout.flush();

    bool color_enabled = false;
    KeyboardListener keyboard;

    while (true) {
        cv::Mat frame;
        if (!cap.read(frame)) {
            continue;
        }

        cv::flip(frame, frame, 1);

        const int key = keyboard.poll();
        if (key != -1) {
            const int lower_key = std::tolower(static_cast<unsigned char>(key));
            if (lower_key == 'q' || key == '\033') {
                break;
            }
            if (lower_key == 'k') {
                color_enabled = !color_enabled;
            }
        }

        const TerminalSize term_size = get_terminal_size();
        const AsciiFrame ascii_frame = frame_to_ascii(frame, term_size, color_enabled);
        render_frame(ascii_frame, term_size);
    }

    cap.release();
    std::cout << RESET << '\n';
    std::cout.flush();
    return 0;
}

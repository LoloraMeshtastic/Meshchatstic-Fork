#include "VirtualKeyboard.h"
#include "configuration.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "main.h"
#include <Arduino.h>
#include <vector>
#include <algorithm>

class OLEDDisplay; // forward

namespace graphics
{

VirtualKeyboard::VirtualKeyboard()
    : cursorRow(0), cursorCol(0), lastActivityTime(millis()), gridVisible(true)
{
    initializeKeyboard();
    // Posicion inicial del cursor (fila 2, col 5 -> 'h' en el layout)
    cursorRow = 2;
    cursorCol = 5;
}

VirtualKeyboard::~VirtualKeyboard() {}

void VirtualKeyboard::initializeKeyboard()
{
    // Layout 4x11 con última columna de acciones
    static const char LAYOUT[KEYBOARD_ROWS][KEYBOARD_COLS] = {
        {'1','2','3','4','5','6','7','8','9','0','\b'},
        {'q','w','e','r','t','y','u','i','o','p','\n'},
        {'a','s','d','f','g','h','j','k','l',';',' '},
        {'z','x','c','v','b','n','m','.',',','?','\x1b'}
    };

    // Asegurar tamaño consistente
    constexpr int LAYOUT_ROWS = (int)(sizeof(LAYOUT) / sizeof(LAYOUT[0]));
    constexpr int LAYOUT_COLS = (int)(sizeof(LAYOUT[0]) / sizeof(LAYOUT[0][0]));
    static_assert(LAYOUT_ROWS == KEYBOARD_ROWS, "LAYOUT rows must equal KEYBOARD_ROWS");
    static_assert(LAYOUT_COLS == KEYBOARD_COLS, "LAYOUT cols must equal KEYBOARD_COLS");

    // Inicializa vacío
    for (int r = 0; r < LAYOUT_ROWS; ++r) {
        for (int c = 0; c < LAYOUT_COLS; ++c) {
            keyboard[r][c] = {0, VK_CHAR, 0, 0, 0, 0};
        }
    }

    // Rellena
    for (int r = 0; r < LAYOUT_ROWS; ++r) {
        for (int c = 0; c < LAYOUT_COLS; ++c) {
            char ch = LAYOUT[r][c];
            VirtualKeyType type = VK_CHAR;
            if (ch == '\b')      type = VK_BACKSPACE;
            else if (ch == '\n') type = VK_ENTER;
            else if (ch == '\x1b') type = VK_ESC;
            else if (ch == ' ')  type = VK_SPACE;

            uint8_t w = (type == VK_BACKSPACE || type == VK_ENTER || type == VK_SPACE) ? (KEY_WIDTH * 3) : KEY_WIDTH;
            keyboard[r][c] = { ch, type, uint8_t(c * KEY_WIDTH), uint8_t(r * KEY_HEIGHT), w, KEY_HEIGHT };
        }
    }
}

void VirtualKeyboard::draw(OLEDDisplay *display, int16_t offsetX, int16_t offsetY)
{
    // Si estamos en modo "freetext" (CardKB), no dibujamos la rejilla.
    if (!gridVisible) {
        drawInputArea(display, offsetX, offsetY, display->getHeight());
        return;
    }

    display->setColor(WHITE);
    display->setFont(FONT_SMALL);

    const int screenW = display->getWidth();
    const int screenH = display->getHeight();
    const bool isWide = screenW >= 200;

    // Medimos etiquetas acciones (columna derecha)
    const int wENTER = display->getStringWidth("ENTER");
    const int lastColPad = (screenW <= 128 ? 2 : 6);
    const int reservedLastColW = wENTER + lastColPad;

    // Reparto columnas
    int cellW = 0, leftoverW = 0;
    {
        const int leftCols = KEYBOARD_COLS - 1; // 10 columnas de char
        int usableW = screenW - reservedLastColW;
        if (usableW < leftCols) usableW = leftCols;
        cellW = usableW / leftCols;
        leftoverW = usableW - cellW * leftCols;
    }

    // Alto por celda y posición Y de teclado
    int cellH = KEY_HEIGHT;
    int keyboardStartY = 0;

    if (screenH <= 64) {
        const int headerHeight = headerText.empty() ? 0 : (FONT_HEIGHT_SMALL - 2);
        const int gapBelowHeader = 0;
        const int singleLineBoxHeight = FONT_HEIGHT_SMALL;
        const int gapAboveKeyboard = 0;
        keyboardStartY = offsetY + headerHeight + gapBelowHeader + singleLineBoxHeight + gapAboveKeyboard;
        keyboardStartY = std::max(0, std::min(keyboardStartY, screenH));
        int keyboardHeight = screenH - keyboardStartY;
        cellH = std::max(1, keyboardHeight / KEYBOARD_ROWS);
    } else if (isWide) {
        cellH = std::max((int)KEY_HEIGHT, cellW);

        display->setFont(FONT_SMALL);
        const int headerHeight = headerText.empty() ? 0 : (FONT_HEIGHT_SMALL + 1);
        const int headerToBoxGap = 1;
        const int gapAboveKb = 1;
        const int minBoxHeightForTwoLines = 2 * FONT_HEIGHT_SMALL + 2;
        int maxKeyboardHeight = screenH - (offsetY + headerHeight + headerToBoxGap + minBoxHeightForTwoLines + gapAboveKb);
        int maxCellHAllowed = maxKeyboardHeight / KEYBOARD_ROWS;
        if (maxCellHAllowed < (int)KEY_HEIGHT) maxCellHAllowed = KEY_HEIGHT;
        if (maxCellHAllowed > 0 && cellH > maxCellHAllowed) cellH = maxCellHAllowed;

        int keyboardHeight = KEYBOARD_ROWS * cellH;
        keyboardStartY = screenH - keyboardHeight;
        keyboardStartY = std::max(0, keyboardStartY);
    } else {
        cellH = KEY_HEIGHT;
        int keyboardHeight = KEYBOARD_ROWS * cellH;
        keyboardStartY = screenH - keyboardHeight;
        keyboardStartY = std::max(0, keyboardStartY);
    }

    // Dibuja caja de entrada
    drawInputArea(display, offsetX, offsetY, keyboardStartY);

    // Precalcula X/W por columna
    int colX[KEYBOARD_COLS], colW[KEYBOARD_COLS];
    int runningX = offsetX;
    for (int c = 0; c < KEYBOARD_COLS - 1; ++c) {
        int wcol = cellW + (c < leftoverW ? 1 : 0);
        colX[c] = runningX;
        colW[c] = wcol;
        runningX += wcol;
    }
    colX[KEYBOARD_COLS - 1] = runningX;
    colW[KEYBOARD_COLS - 1] = reservedLastColW;

    // Dibuja rejilla
    for (int r = 0; r < KEYBOARD_ROWS; ++r) {
        for (int c = 0; c < KEYBOARD_COLS; ++c) {
            const VirtualKey &k = keyboard[r][c];
            if (k.character != 0 || k.type != VK_CHAR) {
                const bool isLastCol = (c == KEYBOARD_COLS - 1);
                int x = colX[c];
                int w = colW[c];
                int y = offsetY + keyboardStartY + r * cellH;
                int h = cellH;
                bool selected = (r == cursorRow && c == cursorCol);
                drawKey(display, k, selected, x, y, (uint8_t)w, (uint8_t)h, isLastCol);
            }
        }
    }
}

void VirtualKeyboard::drawInputArea(OLEDDisplay *display, int16_t offsetX, int16_t offsetY, int16_t keyboardStartY)
{
    display->setColor(WHITE);

    const int screenWidth  = display->getWidth();
    const int screenHeight = display->getHeight();
    const int inputLineH   = FONT_HEIGHT_SMALL;

    display->setFont(FONT_SMALL);
    int headerHeight = 0;
    if (!headerText.empty()) {
        display->drawString(offsetX + 2, offsetY, headerText.c_str());
        headerHeight = (screenHeight <= 64) ? (FONT_HEIGHT_SMALL - 2) : FONT_HEIGHT_SMALL;
    }

    const int boxX = offsetX;
    const int boxWidth = screenWidth;
    int boxY;
    int boxHeight;

    if (screenHeight <= 64) {
        const int gapBelowHeader = 0;
        const int fixedBoxHeight = inputLineH;
        const int gapAboveKeyboard = 0;
        boxY = offsetY + headerHeight + gapBelowHeader;
        boxHeight = fixedBoxHeight;
        if (boxY + boxHeight + gapAboveKeyboard > keyboardStartY) {
            int over = boxY + boxHeight + gapAboveKeyboard - keyboardStartY;
            boxHeight = std::max(1, fixedBoxHeight - over);
        }
    } else {
        const int gapBelowHeader = 1;
        int gapAboveKeyboard = 1;
        int tmpBoxY = offsetY + headerHeight + gapBelowHeader;
        const int minBoxHeight = inputLineH + 2;
        int availableH = keyboardStartY - tmpBoxY - gapAboveKeyboard;
        if (availableH < minBoxHeight) availableH = minBoxHeight;
        boxY = tmpBoxY;
        boxHeight = availableH;
    }

    // Marco
    display->drawRect(boxX, boxY, boxWidth, boxHeight);

    display->setFont(FONT_SMALL);

    const int textX = boxX + 2;
    const int maxTextWidth = boxWidth - 4;
    const int maxLines = (boxHeight - 2) / inputLineH;

    if (maxLines >= 2) {
        const int innerLeft   = boxX + 1;
        const int innerRight  = boxX + boxWidth - 2;
        const int innerTop    = boxY + 1;
        const int innerBottom = boxY + boxHeight - 2;

        std::vector<std::string> lines;
        {
            std::string remaining = inputText;
            while (!remaining.empty()) {
                int bestLen = 0;
                for (int len = 1; len <= (int)remaining.size(); ++len) {
                    int w = display->getStringWidth(remaining.substr(0, len).c_str());
                    if (w <= maxTextWidth) bestLen = len;
                    else break;
                }
                if (bestLen == 0) bestLen = 1;
                lines.emplace_back(remaining.substr(0, bestLen));
                remaining.erase(0, bestLen);
            }
        }

        const bool scrolledUp = ((int)lines.size() > maxLines);
        int caretX = textX;
        int caretY = innerTop;

        const int topInset = 2;
        const int lineStep = std::max(1, inputLineH - 1);
        int lineY = innerTop + topInset;

        if (scrolledUp) {
            const int firstLineTop = lineY;
            const int gapMidY = innerTop + (firstLineTop - innerTop) / 2 + 1;
            const int centerX = boxX + boxWidth / 2;
            const int dotSpacing = 3;
            const int dotSize = 1;
            display->fillRect(centerX - dotSpacing, gapMidY, dotSize, dotSize);
            display->fillRect(centerX,              gapMidY, dotSize, dotSize);
            display->fillRect(centerX + dotSpacing, gapMidY, dotSize, dotSize);
        }

        const int linesCapacity = std::max(1, (innerBottom - lineY + 1) / lineStep);
        const int linesToShow   = std::min((int)lines.size(), linesCapacity);
        const int startIndex    = scrolledUp ? ((int)lines.size() - linesToShow) : 0;

        for (int i = 0; i < linesToShow; ++i) {
            const std::string &chunk = lines[startIndex + i];
            display->drawString(textX, lineY, chunk.c_str());
            caretX = textX + display->getStringWidth(chunk.c_str());
            caretY = lineY;
            lineY += lineStep;
        }

        int caretPadY = (boxHeight >= inputLineH + 4) ? 3 : 2;
        int cursorTop = caretY + caretPadY;
        int cursorH   = lineStep - caretPadY * 2;
        if (cursorH < 1) cursorH = 1;
        if (cursorTop < innerTop) cursorTop = innerTop;
        if (cursorTop + cursorH - 1 > innerBottom) cursorH = innerBottom - cursorTop + 1;
        if (caretX >= innerLeft && caretX <= innerRight) {
            display->drawVerticalLine(caretX, cursorTop, cursorH);
        }
    } else {
        std::string scrolled = inputText;
        int textW = display->getStringWidth(scrolled.c_str());
        if (textW > maxTextWidth) {
            while (textW > maxTextWidth && !scrolled.empty()) {
                scrolled.erase(0, 1);
                textW = display->getStringWidth(scrolled.c_str());
            }
            if (scrolled != inputText) {
                scrolled = "..." + scrolled;
                textW = display->getStringWidth(scrolled.c_str());
                while (textW > maxTextWidth && scrolled.size() > 3) {
                    scrolled.erase(3, 1);
                    textW = display->getStringWidth(scrolled.c_str());
                }
            }
        }

        int innerTop    = boxY + 1;
        int innerBottom = boxY + boxHeight - 2;

        int textY;
        if (screenHeight <= 64) {
            textY = boxY + (boxHeight - FONT_HEIGHT_SMALL) / 2;
        } else {
            int innerH = innerBottom - innerTop + 1;
            textY = innerTop + std::max(0, (innerH - FONT_HEIGHT_SMALL) / 2);
            if (textY < innerTop) textY = innerTop;
            int maxTop = innerBottom - FONT_HEIGHT_SMALL + 1;
            if (textY > maxTop) textY = maxTop;
        }

        if (!scrolled.empty()) {
            display->drawString(textX, textY, scrolled.c_str());
        }

        int cursorX = textX + display->getStringWidth(scrolled.c_str());
        if (screenHeight > 64) {
            int innerRight = boxX + boxWidth - 2;
            if (cursorX > innerRight) cursorX = innerRight;
        }

        int cursorTop, cursorH;
        if (screenHeight <= 64) {
            cursorH = 10;
            cursorTop = boxY + (boxHeight - cursorH) / 2;
        } else {
            int innerLeft  = boxX + 1;
            int innerRight = boxX + boxWidth - 2;
            cursorTop = boxY + 2;
            cursorH   = boxHeight - 4;
            if (cursorH < 1) cursorH = 1;
            if (cursorTop < innerTop) cursorTop = innerTop;
            if (cursorTop + cursorH - 1 > innerBottom) cursorH = innerBottom - cursorTop + 1;
            if (cursorH < 1) cursorH = 1;
            if (cursorX < innerLeft || cursorX > innerRight) return;
        }

        display->drawVerticalLine(cursorX, cursorTop, cursorH);
    }
}

void VirtualKeyboard::drawKey(OLEDDisplay *display, const VirtualKey &key, bool selected, int16_t x, int16_t y, uint8_t width,
                              uint8_t height, bool isLastCol)
{
    display->setFont(FONT_SMALL);
    const int fontH = FONT_HEIGHT_SMALL;

    std::string keyText;
    if (key.type == VK_BACKSPACE || key.type == VK_ENTER || key.type == VK_SPACE || key.type == VK_ESC) {
        keyText = (key.type == VK_BACKSPACE) ? "BACK"
               : (key.type == VK_ENTER)     ? "ENTER"
               : (key.type == VK_SPACE)     ? "SPACE"
               :                              "ESC";
    } else {
        char c = getCharForKey(key, false);
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        keyText = (key.character == ' ' || key.character == '_') ? "_" : std::string(1, c);
    }

    int textWidth = display->getStringWidth(keyText.c_str());
    int textX;
    if (isLastCol) {
        const int rightPad = 1;
        textX = x + width - textWidth - rightPad;
        if (textX < x) textX = x;
    } else {
        textX = x + (width - textWidth) / 2;
        if (display->getHeight() <= 64 && (key.character >= '0' && key.character <= '9')) {
            textX = x + (width - textWidth + 1) / 2;
        }
    }

    int contentTop = y;
    int contentH   = height;

    if (selected) {
        display->setColor(WHITE);
        bool isAction = (key.type == VK_BACKSPACE || key.type == VK_ENTER || key.type == VK_SPACE || key.type == VK_ESC);

        if (display->getHeight() <= 64 && !isAction) {
            display->fillRect(x, y, width, height);
        } else if (isAction) {
            const int padX = 1;
            const int padY = 2;
            int hlW = textWidth + padX * 2;
            int hlX = textX - padX;

            if (hlX < x) {
                hlW -= (x - hlX);
                hlX = x;
            }
            int maxW = (x + width) - hlX;
            hlW = std::max(1, std::min(hlW, maxW));

            int hlH = std::min(fontH + padY * 2, (int)height);
            int hlY = y + (height - hlH) / 2;
            display->fillRect(hlX, hlY, hlW, hlH);
            contentTop = hlY;
            contentH   = hlH;
        } else {
            display->fillRect(x, y, width, height);
        }
        display->setColor(BLACK);
    } else {
        display->setColor(WHITE);
    }

    int centeredTextY = (display->getHeight() <= 64)
                        ? (y + (height - fontH) / 2)
                        : (contentTop + (contentH - fontH) / 2);

    if (display->getHeight() > 64) {
        centeredTextY = std::max(contentTop, std::min(centeredTextY, contentTop + contentH - fontH));
    }

    if (display->getHeight() <= 64 && keyText.size() == 1) {
        char ch = keyText[0];
        if (ch == '.' || ch == ',' || ch == ';') {
            centeredTextY -= 1;
        }
    }
    display->drawString(textX, centeredTextY, keyText.c_str());
}

char VirtualKeyboard::getCharForKey(const VirtualKey &key, bool isLongPress)
{
    if (key.type != VK_CHAR) return key.character;

    char c = key.character;
    if (isLongPress && c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    return c;
}

void VirtualKeyboard::moveCursorDelta(int dRow, int dCol)
{
    resetTimeout();
    int r = (int)cursorRow + dRow;
    int c = (int)cursorCol + dCol;
    if (r < 0) r = KEYBOARD_ROWS - 1;
    else if (r >= KEYBOARD_ROWS) r = 0;
    if (c < 0) c = KEYBOARD_COLS - 1;
    else if (c >= KEYBOARD_COLS) c = 0;
    cursorRow = (uint8_t)r;
    cursorCol = (uint8_t)c;
}

void VirtualKeyboard::moveCursorUp()    { moveCursorDelta(-1, 0); }
void VirtualKeyboard::moveCursorDown()  { moveCursorDelta( 1, 0); }
void VirtualKeyboard::moveCursorLeft()
{
    resetTimeout();
    if (cursorCol > 0) {
        cursorCol--;
    } else {
        if (cursorRow > 0) {
            cursorRow--;
            cursorCol = KEYBOARD_COLS - 1;
        } else {
            cursorRow = KEYBOARD_ROWS - 1;
            cursorCol = KEYBOARD_COLS - 1;
        }
    }
}
void VirtualKeyboard::moveCursorRight()
{
    resetTimeout();
    if (cursorCol < KEYBOARD_COLS - 1) {
        cursorCol++;
    } else {
        if (cursorRow < KEYBOARD_ROWS - 1) {
            cursorRow++;
            cursorCol = 0;
        } else {
            cursorRow = 0;
            cursorCol = 0;
        }
    }
}

void VirtualKeyboard::handlePress()
{
    resetTimeout();
    const VirtualKey &key = keyboard[cursorRow][cursorCol];
    if (key.character == 0 && key.type == VK_CHAR) return;

    if (key.type == VK_CHAR) {
        insertCharacter(getCharForKey(key, false));
        return;
    }

    switch (key.type) {
        case VK_BACKSPACE: deleteCharacter(); break;
        case VK_ENTER:     submitText();      break;
        case VK_SPACE:     insertCharacter(' '); break;
        case VK_ESC:
            if (onTextEntered) {
                auto cb = onTextEntered;
                onTextEntered = nullptr;
                inputText.clear();
                cb("");
            }
            return;
        default: break;
    }
}

void VirtualKeyboard::handleLongPress()
{
    resetTimeout();
    const VirtualKey &key = keyboard[cursorRow][cursorCol];
    if (key.character == 0 && key.type == VK_CHAR) return;

    if (key.type == VK_CHAR) {
        insertCharacter(getCharForKey(key, true));
        return;
    }

    switch (key.type) {
        case VK_BACKSPACE:
            for (int i = 0; i < 5 && !inputText.empty(); ++i) deleteCharacter();
            break;
        case VK_ENTER: submitText(); break;
        case VK_SPACE: insertCharacter(' '); break;
        case VK_ESC:
            if (onTextEntered) onTextEntered("");
            break;
        default: break;
    }
}

void VirtualKeyboard::insertCharacter(char c)
{
    if (inputText.length() < 160) inputText += c;
}

void VirtualKeyboard::deleteCharacter()
{
    if (!inputText.empty()) inputText.pop_back();
}

void VirtualKeyboard::submitText()
{
    LOG_INFO("Virtual keyboard: submitting text '%s'", inputText.c_str());
    if (!inputText.empty() && onTextEntered) {
        auto cb = onTextEntered;
        auto txt = inputText;
        onTextEntered = nullptr;
        cb(txt);
    } else if (inputText.empty()) {
        LOG_INFO("Virtual keyboard: empty text submitted, ignoring - keyboard remains active");
    } else {
        if (screen) screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
    }
}

void VirtualKeyboard::setInputText(const std::string &text) { inputText = text; }
std::string VirtualKeyboard::getInputText() const { return inputText; }
void VirtualKeyboard::setHeader(const std::string &header) { headerText = header; }
void VirtualKeyboard::setCallback(std::function<void(const std::string &)> callback) { onTextEntered = callback; }
void VirtualKeyboard::resetTimeout() { lastActivityTime = millis(); }
bool VirtualKeyboard::isTimedOut() const { return (millis() - lastActivityTime) > TIMEOUT_MS; }

} // namespace graphics

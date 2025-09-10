#pragma once

#include <Arduino.h>
#include <functional>
#include <string>

class OLEDDisplay;

namespace graphics
{

// Tamaño del teclado (4x11)
static constexpr int KEYBOARD_ROWS = 4;
static constexpr int KEYBOARD_COLS = 11;

// Medidas por defecto (el dibujo real calcula dimensiones dinámicamente,
// pero las estructuras internas usan estos valores base)
static constexpr uint8_t KEY_WIDTH  = 16;
static constexpr uint8_t KEY_HEIGHT = 18;

// Timeout del teclado virtual (ms)
static constexpr uint32_t TIMEOUT_MS = 60000; // 60s

enum VirtualKeyType : uint8_t {
    VK_CHAR = 0,
    VK_BACKSPACE,
    VK_ENTER,
    VK_SPACE,
    VK_ESC
};

struct VirtualKey {
    char           character;   // carácter base
    VirtualKeyType type;        // tipo de tecla
    uint8_t        x;           // posición relativa (no usada en el render dinámico)
    uint8_t        y;
    uint8_t        w;           // ancho sugerido
    uint8_t        h;           // alto sugerido
};

class VirtualKeyboard
{
public:
    VirtualKeyboard();
    ~VirtualKeyboard();

    // Dibuja el teclado/caja de texto
    void draw(OLEDDisplay *display, int16_t offsetX, int16_t offsetY);

    // Movimiento del cursor para selección de teclas (rejilla)
    void moveCursorUp();
    void moveCursorDown();
    void moveCursorLeft();
    void moveCursorRight();

    // Pulsaciones
    void handlePress();      // pulsación corta
    void handleLongPress();  // pulsación larga

    // API de edición de texto (DEBEN ser públicas para NotificationRenderer)
    void insertCharacter(char c);
    inline void backspace() { deleteCharacter(); }
    void submitText();

    // Texto y cabecera
    void setInputText(const std::string &text);
    std::string getInputText() const;
    void setHeader(const std::string &header);

    // Callback al enviar
    void setCallback(std::function<void(const std::string &)> callback);

    // Timeout
    void resetTimeout();
    bool isTimedOut() const;

    // Mostrar/ocultar rejilla de teclado (para CardKB = false)
    void setGridVisible(bool v) { gridVisible = v; }
    bool getGridVisible() const { return gridVisible; }

private:
    // Inicializa layout base
    void initializeKeyboard();

    // Dibuja el área de entrada (caja + texto + cursor)
    void drawInputArea(OLEDDisplay *display, int16_t offsetX, int16_t offsetY, int16_t keyboardStartY);

    // Dibuja una tecla
    void drawKey(OLEDDisplay *display, const VirtualKey &key, bool selected, int16_t x, int16_t y, uint8_t width,
                 uint8_t height, bool isLastCol);

    // Obtiene el carácter a insertar (con/ sin long press)
    char getCharForKey(const VirtualKey &key, bool isLongPress);

    void moveCursorDelta(int dRow, int dCol);
    void deleteCharacter();

private:
    VirtualKey keyboard[KEYBOARD_ROWS][KEYBOARD_COLS]{};
    uint8_t    cursorRow = 0;
    uint8_t    cursorCol = 0;

    std::string                        inputText;
    std::string                        headerText;
    std::function<void(const std::string &)> onTextEntered = nullptr;

    uint32_t lastActivityTime = 0;

    // Si false, no se dibuja la rejilla (modo "freetext" para CardKB)
    bool gridVisible = true;
};

} // namespace graphics

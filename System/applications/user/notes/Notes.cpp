#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "SystemUI.h"
#include "keyboard_gii39.h"
#include "sys_llapi.h"
#include "filesystem/fatfs/ff.h"

extern "C" {
extern const unsigned char VGA_Ascii_5x8[];
extern const unsigned char VGA_Ascii_6x12[];
extern const unsigned char VGA_Ascii_8x16[];
}

namespace {

constexpr size_t kNoteCapacity = 240;
constexpr int kColumns = 40;
constexpr int kRows = 7;
const char kNotePath[] = "/SKYNOTE.TXT";
const char kTempPath[] = "/SKYNOTE.TMP";
const char kBackupPath[] = "/SKYNOTE.BAK";

struct NoteState {
    char text[kNoteCapacity + 1];
    size_t length;
    size_t cursor;
    uint8_t alphaMode; // 0: digits, 1: uppercase, 2: lowercase
    bool dirty;
    bool readOnly;
    const char *status;
};

// A small framebuffer keeps this app independent of the main UI's display
// buffer, which SystemUISuspend() releases while another app is running.
class NoteDisplay {
    uint8_t *pixels;

public:
    NoteDisplay() : pixels((uint8_t *)pvPortMalloc(LCD_PIX_W * LCD_PIX_H)) {}
    ~NoteDisplay() { if (pixels) vPortFree(pixels); }
    bool ready() const { return pixels != NULL; }

    void clear(uint8_t color) {
        memset(pixels, color, LCD_PIX_W * LCD_PIX_H);
    }

    void rectangle(int x0, int y0, int x1, int y1, uint8_t color) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (x >= 0 && x < LCD_PIX_W && y >= 0 && y < LCD_PIX_H) {
                    pixels[y * LCD_PIX_W + x] = color;
                }
            }
        }
    }

    void text(int x, int y, const char *value, int height, uint8_t foreground, uint8_t background) {
        const unsigned char *font = height == 16 ? VGA_Ascii_8x16 :
                                    (height == 12 ? VGA_Ascii_6x12 : VGA_Ascii_5x8);
        const int width = height == 16 ? 8 : 6;
        const int inkWidth = height == 8 ? 5 : width;
        while (*value && x + width <= LCD_PIX_W) {
            const unsigned char ch = (unsigned char)*value++;
            if (ch < ' ' || ch > '~') continue;
            for (int dy = 0; dy < height; ++dy) {
                if (y + dy < 0 || y + dy >= LCD_PIX_H) continue;
                const uint8_t bits = font[(ch - ' ') * height + dy];
                for (int dx = 0; dx < width; ++dx) {
                    if (x + dx < 0 || x + dx >= LCD_PIX_W) continue;
                    pixels[(y + dy) * LCD_PIX_W + x + dx] =
                        dx < inkWidth && (bits & (0x80u >> dx)) ? foreground : background;
                }
            }
            x += width;
        }
    }

    void flush() {
        ll_disp_put_area(pixels, 0, 0, LCD_PIX_W - 1, LCD_PIX_H - 1);
    }
};

void loadNote(NoteState &note) {
    FIL file;
    FRESULT result = f_open(&file, kNotePath, FA_READ);
    bool recovered = false;
    if (result == FR_NO_FILE) {
        result = f_open(&file, kBackupPath, FA_READ);
        recovered = result == FR_OK;
    }
    if (result == FR_NO_FILE) {
        note.status = "New note";
        return;
    }
    if (result != FR_OK) {
        note.readOnly = true;
        note.status = "Load failed";
        return;
    }

    const FSIZE_t size = f_size(&file);
    if (size > kNoteCapacity) {
        f_close(&file);
        note.readOnly = true;
        note.status = "File too large";
        return;
    }

    UINT count = 0;
    const FRESULT readResult = f_read(&file, note.text, size, &count);
    const FRESULT closeResult = f_close(&file);
    if (readResult != FR_OK || closeResult != FR_OK || count != size) {
        note.text[0] = '\0';
        note.readOnly = true;
        note.status = "Load failed";
        return;
    }

    note.length = count;
    note.cursor = count;
    note.text[count] = '\0';
    // Leave files with unsupported bytes untouched; show placeholders and
    // allow the user to fix the original on a computer.
    bool unsupported = false;
    for (size_t i = 0; i < note.length; ++i) {
        const unsigned char c = note.text[i];
        if (c != '\n' && (c < 32 || c > 126)) {
            note.text[i] = '?';
            unsupported = true;
        }
    }
    note.readOnly = unsupported;
    note.status = unsupported ? "ASCII only" : (recovered ? "Backup loaded" : "Loaded");
}

bool saveNote(NoteState &note) {
    if (note.readOnly) {
        note.status = "Read only";
        return false;
    }

    FIL file;
    FRESULT result = f_open(&file, kTempPath, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) {
        note.status = "Save failed";
        return false;
    }

    UINT count = 0;
    result = f_write(&file, note.text, note.length, &count);
    if (result == FR_OK && count == note.length) {
        result = f_sync(&file);
    }
    const FRESULT closeResult = f_close(&file);
    if (result != FR_OK || count != note.length || closeResult != FR_OK) {
        note.status = "Save failed";
        return false;
    }

    FILINFO info;
    result = f_stat(kNotePath, &info);
    if (result == FR_OK) {
        result = f_unlink(kBackupPath);
        if (result != FR_OK && result != FR_NO_FILE) {
            note.status = "Save failed";
            return false;
        }
        result = f_rename(kNotePath, kBackupPath);
        if (result != FR_OK) {
            note.status = "Save failed";
            return false;
        }
    } else if (result != FR_NO_FILE) {
        note.status = "Save failed";
        return false;
    }

    result = f_rename(kTempPath, kNotePath);
    if (result != FR_OK) {
        // Preserve the previous note if the final rename fails.
        f_rename(kBackupPath, kNotePath);
        note.status = "Save failed";
        return false;
    }

    note.dirty = false;
    note.status = "Saved";
    return true;
}

void cursorPosition(const NoteState &note, size_t index, int &row, int &column) {
    row = 0;
    column = 0;
    for (size_t i = 0; i < index; ++i) {
        if (note.text[i] == '\n') {
            ++row;
            column = 0;
        } else if (++column == kColumns) {
            ++row;
            column = 0;
        }
    }
}

size_t cursorOnRow(const NoteState &note, int targetRow, int targetColumn) {
    int row = 0;
    int column = 0;
    size_t lastPosition = note.cursor;
    for (size_t i = 0; i <= note.length; ++i) {
        if (row == targetRow) {
            lastPosition = i;
            if (column >= targetColumn) {
                return i;
            }
        } else if (row > targetRow) {
            break;
        }
        if (i == note.length) {
            break;
        }
        if (note.text[i] == '\n') {
            ++row;
            column = 0;
        } else if (++column == kColumns) {
            ++row;
            column = 0;
        }
    }
    return lastPosition;
}

void drawNote(NoteDisplay &display, const NoteState &note) {
    int cursorRow, cursorColumn;
    cursorPosition(note, note.cursor, cursorRow, cursorColumn);
    const int firstRow = cursorRow >= kRows ? cursorRow - kRows + 1 : 0;

    display.clear(255);
    display.rectangle(0, 0, LCD_PIX_W - 1, 15, 0);
    display.text(4, 0, "SkyOS Notes", 16, 255, 0);

    char line[kColumns + 1];
    int row = 0;
    int column = 0;
    for (size_t i = 0; i <= note.length; ++i) {
        const bool atEnd = i == note.length;
        const char ch = atEnd ? '\0' : note.text[i];
        if (atEnd || ch == '\n') {
            line[column] = '\0';
            if (row >= firstRow && row < firstRow + kRows) {
                display.text(8, 20 + (row - firstRow) * 12, line, 12, 0, 255);
            }
            ++row;
            column = 0;
        } else {
            line[column++] = ch;
            if (column == kColumns) {
                line[column] = '\0';
                if (row >= firstRow && row < firstRow + kRows) {
                    display.text(8, 20 + (row - firstRow) * 12, line, 12, 0, 255);
                }
                ++row;
                column = 0;
            }
        }
    }

    const uint32_t cursorX = 8 + cursorColumn * 6;
    const uint32_t cursorY = 20 + (cursorRow - firstRow) * 12 + 11;
    display.rectangle(cursorX, cursorY, cursorX + 5, cursorY, 0);

    const char *mode = note.alphaMode == 0 ? "123" : (note.alphaMode == 1 ? "ABC" : "abc");
    char countText[24];
    snprintf(countText, sizeof(countText), "%s %u/%u %s", mode,
             (unsigned)note.length, (unsigned)kNoteCapacity, note.dirty ? "*" : "");
    display.text(4, 106, countText, 8, 0, 255);
    display.text(120, 106, note.status, 8, 0, 255);
    display.rectangle(0, 116, LCD_PIX_W - 1, LCD_PIX_H - 1, 0);
    display.text(4, 118, "ALPHA:mode  F2:save  F6:exit", 8, 255, 0);
    display.flush();
}

char inputCharacter(uint16_t key, uint8_t alphaMode) {
    if (alphaMode != 0) {
        char letter = 0;
        switch (key) {
        case KEY_VARS: letter = 'A'; break;
        case KEY_MATH: letter = 'B'; break;
        case KEY_ABC: letter = 'C'; break;
        case KEY_XTPHIN: letter = 'D'; break;
        case KEY_SIN: letter = 'E'; break;
        case KEY_COS: letter = 'F'; break;
        case KEY_TAN: letter = 'G'; break;
        case KEY_LN: letter = 'H'; break;
        case KEY_LOG: letter = 'I'; break;
        case KEY_X2: letter = 'J'; break;
        case KEY_XY: letter = 'K'; break;
        case KEY_LEFTBRACKET: letter = 'L'; break;
        case KEY_RIGHTBRACKET: letter = 'M'; break;
        case KEY_DIVISION: letter = 'N'; break;
        case KEY_COMMA: letter = 'O'; break;
        case KEY_7: letter = 'P'; break;
        case KEY_8: letter = 'Q'; break;
        case KEY_9: letter = 'R'; break;
        case KEY_MULTIPLICATION: letter = 'S'; break;
        case KEY_4: letter = 'T'; break;
        case KEY_5: letter = 'U'; break;
        case KEY_6: letter = 'V'; break;
        case KEY_SUBTRACTION: letter = 'W'; break;
        case KEY_1: letter = 'X'; break;
        case KEY_2: letter = 'Y'; break;
        case KEY_3: letter = 'Z'; break;
        case KEY_PLUS: return ' ';
        default: break;
        }
        if (letter != 0) {
            return alphaMode == 2 ? letter + ('a' - 'A') : letter;
        }
    }

    switch (key) {
    case KEY_0: return '0';
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_3: return '3';
    case KEY_4: return '4';
    case KEY_5: return '5';
    case KEY_6: return '6';
    case KEY_7: return '7';
    case KEY_8: return '8';
    case KEY_9: return '9';
    case KEY_DOT: return '.';
    case KEY_COMMA: return ',';
    case KEY_PLUS: return '+';
    case KEY_SUBTRACTION: return '-';
    case KEY_MULTIPLICATION: return '*';
    case KEY_DIVISION: return '/';
    case KEY_LEFTBRACKET: return '(';
    case KEY_RIGHTBRACKET: return ')';
    case KEY_NEGATIVE: return '_';
    case KEY_XY: return '^';
    default: return 0;
    }
}

void insertCharacter(NoteState &note, char ch) {
    if (note.length == kNoteCapacity) {
        note.status = "Note is full";
        return;
    }
    memmove(note.text + note.cursor + 1, note.text + note.cursor,
            note.length - note.cursor + 1);
    note.text[note.cursor++] = ch;
    ++note.length;
    note.dirty = true;
    note.status = "Editing";
}

bool handleKey(NoteState &note, uint16_t key) {
    if (key == KEY_F6 || key == KEY_ON) {
        if (note.dirty && !saveNote(note)) {
            return false;
        }
        return true;
    }
    if (key == KEY_F2) {
        saveNote(note);
        return false;
    }
    if (key == KEY_ALPHA) {
        note.alphaMode = (note.alphaMode + 1) % 3;
        note.status = "Input mode";
        return false;
    }
    if (key == KEY_F1) {
        note.cursor = 0;
        return false;
    }
    if (key == KEY_F3) {
        note.cursor = note.length;
        return false;
    }
    if (key == KEY_LEFT) {
        if (note.cursor > 0) --note.cursor;
        return false;
    }
    if (key == KEY_RIGHT) {
        if (note.cursor < note.length) ++note.cursor;
        return false;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        int row, column, lastRow, lastColumn;
        cursorPosition(note, note.cursor, row, column);
        cursorPosition(note, note.length, lastRow, lastColumn);
        const int target = row + (key == KEY_UP ? -1 : 1);
        if (target >= 0 && target <= lastRow) {
            note.cursor = cursorOnRow(note, target, column);
        }
        return false;
    }
    if (note.readOnly) {
        note.status = "Read only";
        return false;
    }
    if (key == KEY_BACKSPACE) {
        if (note.cursor > 0) {
            memmove(note.text + note.cursor - 1, note.text + note.cursor,
                    note.length - note.cursor + 1);
            --note.cursor;
            --note.length;
            note.dirty = true;
            note.status = "Editing";
        }
        return false;
    }
    const char ch = key == KEY_ENTER ? '\n' : inputCharacter(key, note.alphaMode);
    if (ch != 0) insertCharacter(note, ch);
    return false;
}

void notesTask(void *) {
    SystemUISuspend();
    {
        NoteDisplay display;
        if (!display.ready()) {
            printf("Notes: no display memory\n");
            SystemUIResume();
            vTaskDelete(NULL);
        }
        NoteState note = {};
        loadNote(note);
        drawNote(display, note);

        // Do not interpret the ENTER key that launched this app as note text.
        while (ll_vm_check_key() >> 16) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        uint16_t previousKey = 0;
        bool held = false;
        unsigned heldTicks = 0;
        bool exitApp = false;
        while (!exitApp) {
            const uint32_t raw = ll_vm_check_key();
            if (raw >> 16) {
                const uint16_t key = raw & 0xFFFF;
                const bool repeatable = key == KEY_BACKSPACE || key == KEY_LEFT || key == KEY_RIGHT;
                if (!held || key != previousKey) {
                    exitApp = handleKey(note, key);
                    drawNote(display, note);
                    held = true;
                    heldTicks = 0;
                    previousKey = key;
                } else if (repeatable && ++heldTicks >= 25 && heldTicks % 5 == 0) {
                    exitApp = handleKey(note, key);
                    drawNote(display, note);
                }
            } else {
                held = false;
                heldTicks = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    SystemUIResume();
    vTaskDelete(NULL);
}

} // namespace

extern "C" void Notes_Start(void) {
    if (xTaskCreate(notesTask, "Notes", 1024, NULL,
                    configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        printf("Notes: failed to create task\n");
    }
}

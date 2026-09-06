#pragma once
#include "raylib.h"

// Raylib key constants identify US physical positions. Match the current
// layout's unmodified letter instead, independently of Ctrl, Shift and Caps Lock.
// Do not consume GetKeyPressed/GetCharPressed: multiple controls may query a
// shortcut in one frame, and text fields still need their character events.
inline bool IsLetterPressed(char letter) {
    if (letter >= 'A' && letter <= 'Z') letter += 'a' - 'A';
    if (letter < 'a' || letter > 'z') return false;
    // 161/162 are GLFW's international printable keys, beyond KEY_GRAVE.
    for (int key = KEY_SPACE; key <= 162; ++key) {
        if (!IsKeyPressed(key)) continue;
        const char* name = GetKeyName(key);
        if (!name || !name[0] || name[1]) continue;
        char mapped = name[0];
        if (mapped >= 'A' && mapped <= 'Z') mapped += 'a' - 'A';
        if (mapped == letter) return true;
    }
    return false;
}

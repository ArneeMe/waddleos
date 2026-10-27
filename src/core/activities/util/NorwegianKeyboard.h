#pragma once

#include <components/keyboard/keyboard.h>

// The Norwegian keyboard: QWERTY with å closing the top row and ø æ the home
// row, as on a Norwegian keyboard, plus the digit row and the globe key that
// switches to English. The FreeInk SDK has no Norwegian layout, so it lives here.
namespace waddle::keyboard {

const freeink::ui::KeyboardLayout& norwegian(bool shifted);

}  // namespace waddle::keyboard

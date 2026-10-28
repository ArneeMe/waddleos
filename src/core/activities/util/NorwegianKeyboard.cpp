#include "NorwegianKeyboard.h"

namespace waddle::keyboard {

namespace {

using freeink::ui::KeyboardKey;
using freeink::ui::KeyboardLayout;
using freeink::ui::KeyboardRow;
using freeink::ui::KeyKind;
using freeink::ui::StateNormal;
namespace fui = freeink::ui;

// Same key shapes as the SDK's built-in layouts (FreeInkUI.cpp): character keys
// weigh 2, Shift and Delete 3. Letter ids are their code points.
#define K(label, value) KeyboardKey{label, label, KeyKind::Normal, StateNormal, value, 2, true}
#define KA(label, value, alt) KeyboardKey{label, label, KeyKind::Normal, StateNormal, value, 2, true, alt}
#define KS(label, kind, value, units) KeyboardKey{label, nullptr, kind, StateNormal, value, (units) * 2, true}
#define K15(label, kind, value) KeyboardKey{label, nullptr, kind, StateNormal, value, 3, true}

const KeyboardKey DIGITS[] = {KA("1", '1', "!"), KA("2", '2', "\""), KA("3", '3', "#"), KA("4", '4', "$"),
                              KA("5", '5', "%"), KA("6", '6', "&"),  KA("7", '7', "/"), KA("8", '8', "("),
                              KA("9", '9', ")"), KA("0", '0', "=")};

const KeyboardKey ROW1[] = {K("q", 'q'), K("w", 'w'), K("e", 'e'), K("r", 'r'), K("t", 't'), K("y", 'y'),
                            K("u", 'u'), K("i", 'i'), K("o", 'o'), K("p", 'p'), K("å", 0xE5)};
const KeyboardKey ROW2[] = {K("a", 'a'), K("s", 's'), K("d", 'd'), K("f", 'f'), K("g", 'g'), K("h", 'h'),
                            K("j", 'j'), K("k", 'k'), K("l", 'l'), K("ø", 0xF8), K("æ", 0xE6)};
const KeyboardKey ROW3[] = {K15(nullptr, KeyKind::Shift, fui::QWERTY_KEY_SHIFT),
                            K("z", 'z'), K("x", 'x'), K("c", 'c'), K("v", 'v'), K("b", 'b'), K("n", 'n'), K("m", 'm'),
                            K15("Del", KeyKind::Delete, fui::QWERTY_KEY_BACKSPACE)};

const KeyboardKey SHIFT_ROW1[] = {K("Q", 'Q'), K("W", 'W'), K("E", 'E'), K("R", 'R'), K("T", 'T'), K("Y", 'Y'),
                                  K("U", 'U'), K("I", 'I'), K("O", 'O'), K("P", 'P'), K("Å", 0xC5)};
const KeyboardKey SHIFT_ROW2[] = {K("A", 'A'), K("S", 'S'), K("D", 'D'), K("F", 'F'), K("G", 'G'), K("H", 'H'),
                                  K("J", 'J'), K("K", 'K'), K("L", 'L'), K("Ø", 0xD8), K("Æ", 0xC6)};
const KeyboardKey SHIFT_ROW3[] = {K15(nullptr, KeyKind::Shift, fui::QWERTY_KEY_SHIFT),
                                  K("Z", 'Z'), K("X", 'X'), K("C", 'C'), K("V", 'V'), K("B", 'B'), K("N", 'N'),
                                  K("M", 'M'), K15("Del", KeyKind::Delete, fui::QWERTY_KEY_BACKSPACE)};

// ?123, the globe (to English), space and OK.
const KeyboardKey BOTTOM[] = {KS("?123", KeyKind::Mode, fui::QWERTY_KEY_MODE, 2),
                              KS(nullptr, KeyKind::Lang, fui::QWERTY_KEY_LANG, 1),
                              KS("Space", KeyKind::Space, fui::QWERTY_KEY_SPACE, 5),
                              KS("OK", KeyKind::Ok, fui::QWERTY_KEY_ENTER, 2)};

// The digit row sizes itself: the letter rows have eleven keys.
const KeyboardRow ROWS[] = {{DIGITS, 10, 0, true}, {ROW1, 11, 0}, {ROW2, 11, 0}, {ROW3, 9, 0}, {BOTTOM, 4, 0}};
const KeyboardRow SHIFT_ROWS[] = {
    {DIGITS, 10, 0, true}, {SHIFT_ROW1, 11, 0}, {SHIFT_ROW2, 11, 0}, {SHIFT_ROW3, 9, 0}, {BOTTOM, 4, 0}};

const KeyboardLayout LAYOUT{ROWS, 5};
const KeyboardLayout SHIFT_LAYOUT{SHIFT_ROWS, 5};

}  // namespace

const KeyboardLayout& norwegian(const bool shifted) { return shifted ? SHIFT_LAYOUT : LAYOUT; }

}  // namespace waddle::keyboard

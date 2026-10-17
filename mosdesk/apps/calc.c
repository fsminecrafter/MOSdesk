#include "window.h"
#include "graphics.h"
#include "string.h"
#include "easyui.h"
#include <stdio.h>

#include "util.h"

int button0, button1, button2, button3, button4,
button5, button6, button7, button8, button9, number, buttonplus,
buttonminus, buttondiv, buttonmult, buttonequal, buttonclr, buttonback;

// Action functions

float add(float a, float b) {
    return a + b;
}

float sub(float a, float b) {      /* renamed from min: avoids clashing with a min() macro */
    return a - b;
}

float divide(float a, float b) {   /* renamed from div: div() already exists in stdlib.h */
    return a / b;
}

float mult(float a, float b) {
    return a * b;
}

// Helpers

static int is_op(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/';
}

/* Returns where the action is in the string, or -1 if there isn't one.
   It starts at index 1 so a minus sign at the front ("-5+3") counts as part of A. */
static int find_op(const char* s) {
    int i;
    for (i = 1; s[i]; i++)
        if (is_op(s[i])) return i;
    return -1;
}

/* Turns the first len characters of s into a float, e.g. "12.5" or "-3".
   Returns 1 if it worked, or 0 if the text isn't a number. */
static int parse_num(const char* s, int len, float* out) {
    float val = 0, scale = 1;
    int i = 0, neg = 0, digits = 0, dot = 0;

    while (i < len && s[i] == ' ') i++;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); i++; }

    for (; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (dot) { scale /= 10; val += (c - '0') * scale; }
            else     { val = val * 10 + (c - '0'); }
            digits++;
        }
        else if (c == '.' && !dot) dot = 1;
        else if (c == ' ') continue;
        else return 0;
    }
    if (!digits) return 0;
    *out = neg ? -val : val;
    return 1;
}

/* Writes a whole number as text, e.g. 123 -> "123".
   Returns a pointer to the end of what it wrote, so more text can be added after it. */
static char* put_long(char* out, long n) {
    char tmp[12];
    int i = 0;

    do {                                 /* pulls the digits off backwards: 123 -> '3','2','1' */
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    } while (n > 0);

    while (i > 0) *out++ = tmp[--i];     /* writes them in the right order */
    *out = '\0';
    return out;
}

/* Writes the float as text: "8" when it's a whole number, "2.5" otherwise (up to 4 decimals). */
static void format_result(float r, char* out) {
    int neg = r < 0;
    long whole, frac;
    int i;

    if (neg) r = -r;
    r += 0.00005f;                          /* round to 4 decimals */
    whole = (long)r;                        /* 2.5 -> 2    */
    frac  = (long)((r - whole) * 10000);    /* 2.5 -> 5000 */
    if (whole == 0 && frac == 0) neg = 0;   /* don't show "-0" */

    if (neg) *out++ = '-';
    out = put_long(out, whole);
    if (frac == 0) return;                  /* whole number: no decimals */

    *out++ = '.';
    for (i = 3; i >= 0; i--) {              /* always 4 digits, so 0.05 gives "0500", not "500" */
        out[i] = '0' + (frac % 10);
        frac /= 10;
    }
    out[4] = '\0';

    i = 3;
    while (out[i] == '0') out[i--] = '\0';  /* remove trailing zeros: "5000" -> "5" */
}

// Callbacks

/* "=" pressed: split the text into A, action and B, then do the maths */
void calc(int v) {
    (void)v;
    char result[EASYUI_TEXT_MAX];
    const char* s = easyui.gettext(number);
    int op = find_op(s);
    float a, b, r;
    char action;

    if (op < 0
        || !parse_num(s, op, &a)                              /* A = everything before the action */
        || !parse_num(s + op + 1, strlen(s) - op - 1, &b)) {  /* B = everything after it */
        easyui.settext(number, "Error");
        return;
    }

    action = s[op];
    switch (action) {
        case '+': r = add(a, b);  break;
        case '-': r = sub(a, b);  break;
        case '*': r = mult(a, b); break;
        case '/':
            if (b == 0) { easyui.settext(number, "Error"); return; }
            r = divide(a, b);
            break;
        default: return;
    }

    format_result(r, result);
    easyui.settext(number, result);
}

/* Any digit or action button: add its label to the end of the text */
void text(int v) {
    (void)v;
    char text[EASYUI_TEXT_MAX];
    const char* old   = easyui.gettext(number);
    const char* label = easyui.gettext(easyui.current());
    int len;

    if (strcmp(old, "Error") == 0) old = "";        /* start fresh after an error */

    if (is_op(label[0])) {
        len = strlen(old);
        if (len == 0 && label[0] != '-') return;    /* only '-' (negative number) can come first */

        if (len > 1 && is_op(old[len - 1])) {
            /* "5+" then "*" pressed -> swap the action: "5*" */
            strlcpy_safe(text, old, sizeof text);
            text[len - 1] = label[0];
            easyui.settext(number, text);
            return;
        }

        if (find_op(old) >= 0) {
            /* "5+3" then "*" pressed -> work out 8 first, then add "*" */
            calc(1);
            old = easyui.gettext(number);
            if (strcmp(old, "Error") == 0) return;
        }
    }

    if (strlen(old) + strlen(label) >= sizeof text) return;   /* full: don't overflow */

    strcpy(text, old);
    strcpy(text + strlen(text), label);
    easyui.settext(number, text);
}

/* CLR: empty the field */
void clear_text(int v) {
    (void)v;
    easyui.settext(number, "");
}

/* <- : remove the last character */
void backspace(int v) {
    (void)v;
    char text[EASYUI_TEXT_MAX];
    const char* old = easyui.gettext(number);
    int len = strlen(old);

    if (len == 0) return;                          /* nothing to remove */
    if (strcmp(old, "Error") == 0) {               /* clear "Error" all at once */
        easyui.settext(number, "");
        return;
    }

    strlcpy_safe(text, old, sizeof text);
    text[len - 1] = '\0';                          /* cut off the last character */
    easyui.settext(number, text);
}

int main() {
    printf("Starting Calculator");
    easyui.init("Calculator", 360, 300);
    easyui.setsmoothtext(1);
    int UIoffsetY = 20;

    //UI
    button1 = easyui.createbutton(10, 75 + UIoffsetY, 75, 40, "1");
    button2 = easyui.createbutton(90, 75 + UIoffsetY, 75, 40, "2");
    button3 = easyui.createbutton(170, 75 + UIoffsetY, 75, 40, "3");
    //Row 2
    button4 = easyui.createbutton(10, 120 + UIoffsetY, 75, 40, "4");
    button5 = easyui.createbutton(90, 120 + UIoffsetY, 75, 40, "5");
    button6 = easyui.createbutton(170, 120 + UIoffsetY, 75, 40, "6");
    //Row 3
    button7 = easyui.createbutton(10, 165 + UIoffsetY, 75, 40, "7");
    button8 = easyui.createbutton(90, 165 + UIoffsetY, 75, 40, "8");
    button9 = easyui.createbutton(170, 165 + UIoffsetY, 75, 40, "9");
    // Row 4
    buttonclr = easyui.createbutton(255, 25 + UIoffsetY, 70, 40, "CLR");
    button0 = easyui.createbutton(90, 210 + UIoffsetY, 75, 40, "0");
    //Text input
    number = easyui.createtextinput(10, 20 + UIoffsetY, 240, 50, "Enter a number...");
    buttonback = easyui.createbutton(10, 210 + UIoffsetY, 75, 40, "<-");
    //Actions
    buttonplus  = easyui.createbutton(250, 75 + UIoffsetY, 75, 40, "+");
    buttonminus = easyui.createbutton(250, 120 + UIoffsetY, 75, 40, "-");
    buttondiv   = easyui.createbutton(250, 165 + UIoffsetY, 75, 40, "/");
    buttonmult  = easyui.createbutton(250, 210 + UIoffsetY, 75, 40, "*");
    buttonequal = easyui.createbutton(170, 210 + UIoffsetY, 75, 40, "=");

    // Digits
    easyui.recallfunc(button1, ONPRESS, text);
    easyui.recallfunc(button2, ONPRESS, text);
    easyui.recallfunc(button3, ONPRESS, text);
    easyui.recallfunc(button4, ONPRESS, text);
    easyui.recallfunc(button5, ONPRESS, text);
    easyui.recallfunc(button6, ONPRESS, text);
    easyui.recallfunc(button7, ONPRESS, text);
    easyui.recallfunc(button8, ONPRESS, text);
    easyui.recallfunc(button9, ONPRESS, text);
    easyui.recallfunc(button0, ONPRESS, text);

    // Actions use the same function, since they also just add their label
    easyui.recallfunc(buttonplus,  ONPRESS, text);
    easyui.recallfunc(buttonminus, ONPRESS, text);
    easyui.recallfunc(buttondiv,   ONPRESS, text);
    easyui.recallfunc(buttonmult,  ONPRESS, text);

    // Equals, clear and backspace
    easyui.recallfunc(buttonequal, ONPRESS, calc);
    easyui.recallfunc(buttonclr,   ONPRESS, clear_text);
    easyui.recallfunc(buttonback,  ONPRESS, backspace);

    // Enter key: use your easyui's event name for "enter pressed in a text input", e.g.
    // easyui.recallfunc(number, ONENTER, calc);

    while (true) {
        easyui.run();
        mos_sleep(16);
    }
}

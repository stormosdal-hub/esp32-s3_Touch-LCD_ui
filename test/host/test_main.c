/* Host unit tests for the hardware-independent firmware pieces. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io_codec.h"
#include "signals.h"
#include "template.h"
#include "util.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b))) { fails++; printf("FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static void fmt(const char *f, double n, const char *t, const char *want)
{
    char out[128];
    util_format_value(f, n, t, out, sizeof(out));
    CHECK_STR(out, want);
}

static void expand(const char *tmpl, double num, const char *text, int is_text, const char *want)
{
    tmpl_ctx_t ctx = { .num = num, .text = text, .is_text = is_text, .source = "btn1" };
    char *s = tmpl_expand(tmpl, &ctx);
    CHECK_STR(s, want);
    free(s);
}

int main(void)
{
    /* util_format_value */
    fmt("%.1f °C", 21.456, NULL, "21.5 °C");
    fmt("Level: %.0f %%", 40, NULL, "Level: 40 %");
    fmt("%d", 3.6, NULL, "4");
    fmt("%05.1f", 3.14159, NULL, "003.1");
    fmt("%x", 255, NULL, "ff");
    fmt("%s", 0, "hello", "hello");
    fmt("Mode: %s", 2, NULL, "Mode: 2");
    fmt("", 12.5, NULL, "12.5");
    fmt(NULL, 7, NULL, "7");
    fmt("%s%s%s%s%n", 1, "x", "xxxx%n");          /* %n is not a conversion we fill */
    fmt("100%", 1, NULL, "100%");
    fmt("%.f", 2.6, NULL, "3");

    /* util_num_to_str */
    char b[32];
    util_num_to_str(40, b, sizeof(b)); CHECK_STR(b, "40");
    util_num_to_str(-3.25, b, sizeof(b)); CHECK_STR(b, "-3.25");
    util_num_to_str(1.0 / 3, b, sizeof(b)); CHECK_STR(b, "0.3333333333");
    util_num_to_str(1234567.8, b, sizeof(b)); CHECK_STR(b, "1234567.8");
    util_num_to_str(0.1 + 0.2, b, sizeof(b)); CHECK_STR(b, "0.3");

    /* util_parse_hex */
    uint8_t hx[8];
    CHECK(util_parse_hex("01 02 ff", hx, 8) == 3 && hx[2] == 0xFF);
    CHECK(util_parse_hex("0x10,0x20", hx, 8) == 2 && hx[1] == 0x20);
    CHECK(util_parse_hex("zz", hx, 8) == -1);
    CHECK(util_parse_hex("100", hx, 8) == -1);
    CHECK(util_parse_hex("", hx, 8) == 0);

    /* util_unescape */
    char esc[] = "A\\r\\n\\x41\\\\";
    size_t n = util_unescape(esc);
    CHECK(n == 5 && !memcmp(esc, "A\r\nA\\", 5));

    /* util_parse_number */
    double v;
    CHECK(util_parse_number(" 12.5 ", &v, false) && v == 12.5);
    CHECK(!util_parse_number("12abc", &v, false));
    CHECK(util_parse_number("ON", &v, true) && v == 1);
    CHECK(!util_parse_number("ON", &v, false));
    CHECK(util_parse_number("0x48", &v, false) && v == 72);

    /* util_parse_color */
    uint32_t rgb;
    CHECK(util_parse_color("#2196F3", &rgb) && rgb == 0x2196F3);
    CHECK(util_parse_color("#f80", &rgb) && rgb == 0xFF8800);
    CHECK(!util_parse_color("red", &rgb));

    /* Modbus CRC: 01 03 00 00 00 01 -> 84 0A */
    const uint8_t req[] = { 1, 3, 0, 0, 0, 1 };
    uint32_t crc = util_crc16_modbus(req, 6);
    CHECK((crc & 0xFF) == 0x84 && (crc >> 8) == 0x0A);

    /* util_json_parse: depth limit */
    char deep[200];
    memset(deep, '[', 100);
    memset(deep + 100, ']', 100);
    CHECK(util_json_parse(deep, sizeof(deep)) == NULL);
    const char *ok_json = "{\"a\":[1,{\"b\":\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"}]}";
    cJSON *pj = util_json_parse(ok_json, strlen(ok_json));
    CHECK(pj != NULL);
    cJSON_Delete(pj);

    /* io_decode */
    const uint8_t be[] = { 0xFF, 0x38, 0x41, 0x48, 0x00, 0x00 };
    CHECK(io_decode(be, 2, "s16", &v) && v == -200);
    CHECK(io_decode(be, 2, "u16", &v) && v == 0xFF38);
    CHECK(io_decode(be, 2, "u16le", &v) && v == 0x38FF);
    CHECK(io_decode(be + 2, 4, "f32", &v) && v == 12.5);
    CHECK(io_decode(be, 1, "bit3", &v) && v == 1);
    CHECK(io_decode(be, 1, "s8", &v) && v == -1);
    CHECK(!io_decode(be, 1, "u16", &v));
    CHECK(io_format_size("u32r") == 4 && io_format_size("hex") == 0);

    /* signals */
    sig_init();
    sig_set_num("temp", 21.5, false);
    sig_set_text("mode", "Auto", false);
    sig_set_text("num_as_text", "42", false);
    sig_set_text("onoff", "ON", false);
    int ti = sig_index("temp", false);
    CHECK(ti >= 0 && sig_get_num(ti, &v) && v == 21.5);
    char txt[64];
    CHECK(sig_get_text(sig_index("mode", false), txt, sizeof(txt)) && !strcmp(txt, "Auto"));
    CHECK(!sig_is_text(sig_index("num_as_text", false)));
    CHECK(sig_get_num(sig_index("onoff", false), &v) && v == 1);
    CHECK(sig_index("missing", false) == -1);
    sig_set_text_remote("remote.x", "5");
    CHECK(sig_index("remote.x", false) >= 0);
    CHECK(sig_count() == 5);

    /* templates */
    sig_set_num("level", 7, false);
    expand("{\"id\":\"{id}\",\"value\":{value}}", 40, NULL, 0, "{\"id\":\"btn1\",\"value\":40}");
    expand("Mode: {text}", 1, "Manual", 0, "Mode: Manual");
    expand("{value}", 1, "typed", 1, "typed");
    expand("01 06 00 01 {value:hex16}", 258, NULL, 0, "01 06 00 01 01 02");
    expand("{value:hex}", 255, NULL, 0, "FF");
    expand("{value:.2}", 3.14159, NULL, 0, "3.14");
    expand("{value:int}", 2.7, NULL, 0, "3");
    expand("lvl={sig:level} t={sig:temp:.1}", 0, NULL, 0, "lvl=7 t=21.5");
    expand("{unknown} {", 0, NULL, 0, "{unknown} {");
    expand("{{value}}", 5, NULL, 0, "{5}");
    expand("", 0, NULL, 0, "");

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

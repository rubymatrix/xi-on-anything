/* host/display_fit.c's sizes for phones and iPads in landscape (docs/ios-design.md), without SDL:
 *   clang -DDISPLAY_FIT_POLICY_ONLY -I host tests/display_fit_test.c host/display_fit.c -o build/display_fit_test */
#include <stdio.h>
#include <stdlib.h>

#include "display_fit.h"

static int failed;

static void expect(const char* what, int got, int want)
{
    if (got != want)
    {
        printf("%s: %d, not %d\n", what, got, want);
        failed = 1;
    }
}

int main(void)
{
    DisplayOut o;
    /* iPhone 18 Pro: 874x402 at 3, the Dynamic Island's inset each side, the home indicator's below */
    DisplayIn phone = { 874, 402, 3.0f, 62, 0, 750, 381, 1.0f, 2.4e6 };
    display_policy(&phone, &o);
    expect("phone rect x", o.rect_x, 186);
    expect("phone rect w", o.rect_w, 2250);
    expect("phone rect h", o.rect_h, 1143);
    expect("phone within budget", o.w * o.h <= 2400000 + 4000, 1);
    expect("phone aspect kept", abs(o.w * 1143 - o.h * 2250) < 2250 * 2, 1);
    expect("phone interface h", o.menu_h, 480);   /* 381 points: forced to 480 lines */
    expect("phone interface w", o.menu_w, 944);
    /* the same phone with no budget: the safe area's own pixels */
    phone.max_pixels = 0;
    display_policy(&phone, &o);
    expect("phone native w", o.w, 2250);
    expect("phone native h", o.h, 1142);           /* even */
    /* iPad Pro 13": 1376x1032 at 2, the home indicator's 20 points */
    DisplayIn ipad = { 1376, 1032, 2.0f, 0, 0, 1376, 1012, 1.0f, 4.0e6 };
    display_policy(&ipad, &o);
    expect("ipad interface w", o.menu_w, 1376);    /* one point an interface pixel */
    expect("ipad interface h", o.menu_h, 1012);
    expect("ipad within budget", o.w * o.h <= 4000000 + 4000, 1);
    /* a bigger UI on the iPad: 1.25 points a pixel */
    ipad.ui_points = 1.25f;
    display_policy(&ipad, &o);
    expect("ipad 1.25 interface h", o.menu_h, 810);
    /* a tall window past the interface's 1200 lines: capped */
    DisplayIn tall = { 2000, 1500, 1.0f, 0, 0, 0, 0, 1.0f, 0 };
    display_policy(&tall, &o);
    expect("tall interface h", o.menu_h, 1200);
    expect("tall rect is the window", o.rect_w, 2000);
    if (!failed)
        printf("display_fit_test: ok\n");
    return failed;
}

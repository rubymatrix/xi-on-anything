package com.rubymatrix.xionanything;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BlurMaskFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LightingColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;

import org.libsdl.app.SDLActivity;

/* The app's activity: SDL runs libmain.so (host/android_main.c) with the arguments from
 * getArguments(). Over the game's surface, Overlay draws the touch controls host/touch.c lays out
 * (through host/android_overlay.c) and the wallpaper beside the game; touches go through it to SDL.
 * The intent's gfx_test extra runs the offscreen graphics fixture instead of the game. */
public final class MainActivity extends SDLActivity {
    private Overlay overlay;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries || mLayout == null)
            return;
        overlay = new Overlay(this);
        mLayout.addView(overlay,
                new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    // host/android_overlay.c, from the game's thread or the UI thread
    public void setTouchShapes(float[] shapes, String labels) {
        if (overlay != null)
            overlay.setShapes(shapes, labels.split("\n", -1));
    }

    public void setWallpaper(int[] argb, int w, int h) {
        if (overlay != null)
            overlay.setWallpaper(argb == null ? null : Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888));
    }

    public void setPresentRect(int x, int y, int w, int h) {
        if (overlay != null)
            overlay.setPresentRect(new Rect(x, y, x + w, y + h));
    }

    /* Back that SDL did not take as a key (SDL_ANDROID_TRAP_BACK_BUTTON): Escape, as touch.c makes of it */
    @Override
    public void onBackPressed() {
        if (mBrokenLibraries) {
            super.onBackPressed();
            return;
        }
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ESCAPE);
        if (overlay != null)
            overlay.postDelayed(new Runnable() {
                @Override
                public void run() {
                    SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ESCAPE);
                }
            }, 80);
    }

    /* What host/touch.c asks for, drawn as the Metal back end draws it on iOS (gfx_metal.m touch_fs):
     * dark glass, lighter at the top, a light rim and a soft shadow, with a symbol or a label. The
     * shapes are in the surface's pixels, which this view's are. Not focusable, and not clickable:
     * every touch is the surface's. */
    static final class Overlay extends View {
        // gfx.h's GFX_TOUCH_* and GFX_SYM_*
        static final int BACK = 0, BUTTON = 1, PILL = 2, RING = 3, KNOB = 4;
        static final int CROSS = 1, CIRCLE = 2, SQUARE = 3, TRIANGLE = 4, UP = 5, DOWN = 6, LEFT = 7, RIGHT = 8,
                PAD = 9, LOCK = 10, UNLOCK = 11;

        private float[] shapes = new float[0];
        private String[] labels = new String[0];
        private Bitmap wallpaper;
        private Rect presentRect;
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                text = new Paint(Paint.ANTI_ALIAS_FLAG), image = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Path path = new Path();
        private final RectF box = new RectF(), arc = new RectF();
        private BlurMaskFilter shadow;
        private float shadowRadius;

        Overlay(Context context) {
            super(context);
            setFocusable(false);
            setClickable(false);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
            setWillNotDraw(false);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeCap(Paint.Cap.ROUND);
            line.setStrokeJoin(Paint.Join.ROUND);
            text.setTypeface(Typeface.DEFAULT_BOLD);
            text.setTextAlign(Paint.Align.CENTER);
            text.setColor(Color.WHITE);
            image.setColorFilter(new LightingColorFilter(0xFFB3B3B3, 0)); // dimmed to 0.7
        }

        synchronized void setShapes(float[] s, String[] l) {
            shapes = s;
            labels = l;
            postInvalidateOnAnimation();
        }

        synchronized void setWallpaper(Bitmap b) {
            wallpaper = b;
            postInvalidateOnAnimation();
        }

        synchronized void setPresentRect(Rect r) {
            presentRect = r;
            postInvalidateOnAnimation();
        }

        private static int rgb(float r, float g, float b, float a) {
            return Color.argb(Math.round(Math.max(0, Math.min(1, a)) * 255), Math.round(r * 255), Math.round(g * 255),
                    Math.round(b * 255));
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float[] s;
            String[] l;
            Bitmap wall;
            Rect rect;
            synchronized (this) {
                s = shapes;
                l = labels;
                wall = wallpaper;
                rect = presentRect;
            }
            if (wall != null && rect != null && !rect.isEmpty())
                drawWallpaper(canvas, wall, rect);
            for (int i = 0; i + 8 <= s.length; i += 8)
                drawShape(canvas, s[i], s[i + 1], s[i + 2], s[i + 3], (int) s[i + 4], (int) s[i + 5], s[i + 6] != 0,
                        s[i + 7], i / 8 < l.length ? l[i / 8] : "");
        }

        /* the picture over the whole view, cropped to fill it, dimmed; not over the game */
        private void drawWallpaper(Canvas canvas, Bitmap wall, Rect rect) {
            int w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0)
                return;
            float view = (float) w / h, pic = (float) wall.getWidth() / wall.getHeight();
            Rect src;
            if (pic > view) { // wider than the screen: its middle across
                int sw = Math.round(wall.getHeight() * view);
                src = new Rect((wall.getWidth() - sw) / 2, 0, (wall.getWidth() + sw) / 2, wall.getHeight());
            } else {
                int sh = Math.round(wall.getWidth() / view);
                src = new Rect(0, (wall.getHeight() - sh) / 2, wall.getWidth(), (wall.getHeight() + sh) / 2);
            }
            canvas.save();
            canvas.clipOutRect(rect);
            canvas.drawBitmap(wall, src, new Rect(0, 0, w, h), image);
            canvas.restore();
        }

        private void body(float x, float y, float hw, float hh, int kind) {
            path.reset();
            if (kind == PILL) {
                box.set(x - hw, y - hh, x + hw, y + hh);
                path.addRoundRect(box, hh, hh, Path.Direction.CW);
            } else
                path.addCircle(x, y, hw, Path.Direction.CW);
        }

        private void drawShape(Canvas canvas, float x, float y, float hw, float hh, int kind, int sym, boolean pressed,
                float alpha, String label) {
            if (kind != PILL)
                hh = hw;
            float r = Math.min(hw, hh);
            body(x, y, hw, hh, kind);
            // the shadow, a little below
            fill.setShader(null);
            fill.setColor(rgb(0, 0, 0, 0.3f * alpha));
            canvas.save();
            canvas.translate(0, r * 0.06f);
            float blur = Math.max(1, Math.round(r * 0.15f));
            if (blur != shadowRadius) {
                shadowRadius = blur;
                shadow = new BlurMaskFilter(blur, BlurMaskFilter.Blur.OUTER);
            }
            fill.setMaskFilter(shadow);
            canvas.drawPath(path, fill);
            fill.setMaskFilter(null);
            canvas.restore();
            // the glass
            float a = 0.6f, top0 = 0.30f, top1 = 0.33f, bot0 = 0.11f, bot1 = 0.13f, rim = 0.35f;
            int light, dark;
            if (kind == RING) {
                light = dark = rgb(0.1f, 0.1f, 0.1f, 0.22f * alpha);
                rim = 0.56f;
            } else if (kind == KNOB) {
                light = rgb(0.85f, 0.85f, 0.85f, 0.85f * alpha);
                dark = rgb(0.6f, 0.6f, 0.6f, 0.85f * alpha);
            } else {
                if (pressed) {
                    a = 0.8f;
                    top0 = top0 + (0.75f - top0) * 0.45f;
                    top1 = top1 + (0.75f - top1) * 0.45f;
                    bot0 = bot0 + (0.75f - bot0) * 0.45f;
                    bot1 = bot1 + (0.75f - bot1) * 0.45f;
                }
                light = rgb(top0, top0, top1, a * alpha);
                dark = rgb(bot0, bot0, bot1, a * alpha);
            }
            fill.setShader(new LinearGradient(0, y - hh, 0, y + hh, light, dark, Shader.TileMode.CLAMP));
            canvas.drawPath(path, fill);
            fill.setShader(null);
            // the rim
            line.setStrokeWidth(Math.max(1.5f, r * 0.04f));
            line.setColor(rgb(1, 1, 1, rim * alpha));
            float inset = line.getStrokeWidth() * 0.5f;
            body(x, y, hw - inset, hh - inset, kind);
            canvas.drawPath(path, line);
            // the symbol or the label
            float ink = 0.97f * alpha;
            if (kind == BACK)
                chevron(canvas, x, y, r, ink);
            else if (sym != 0)
                symbol(canvas, x, y, r, sym, ink);
            else if (label != null && !label.isEmpty()) {
                float lw = kind == PILL ? hw * 1.5f : r * 1.7f, lh = lw * 0.375f;
                text.setTextSize(lh * 0.8f);
                float tw = text.measureText(label);
                if (tw > lw)
                    text.setTextSize(lh * 0.8f * lw / tw);
                text.setColor(rgb(1, 1, 1, ink));
                Paint.FontMetrics m = text.getFontMetrics();
                canvas.drawText(label, x, y - (m.ascent + m.descent) * 0.5f, text);
            }
        }

        private void chevron(Canvas canvas, float x, float y, float r, float ink) {
            line.setStrokeWidth(r * 0.13f);
            line.setColor(rgb(1, 1, 1, ink));
            path.reset();
            path.moveTo(x + 0.08f * r, y - 0.34f * r);
            path.lineTo(x - 0.26f * r, y);
            path.lineTo(x + 0.08f * r, y + 0.34f * r);
            canvas.drawPath(path, line);
        }

        // an equilateral triangle, centre (x, y), half a side h, pointing (dx, dy)
        private void triangle(float x, float y, float h, float dx, float dy) {
            float k = (float) Math.sqrt(3.0);
            float tipx = x + dx * h * 2 / k, tipy = y + dy * h * 2 / k; // the apex, 2/3 of the height out
            float bx = x - dx * h / k, by = y - dy * h / k; // the base's middle
            path.reset();
            path.moveTo(tipx, tipy);
            path.lineTo(bx - dy * h, by + dx * h);
            path.lineTo(bx + dy * h, by - dx * h);
            path.close();
        }

        private void symbol(Canvas canvas, float x, float y, float r, int sym, float ink) {
            line.setStrokeWidth(r * 0.15f);
            fill.setShader(null);
            if (sym == CROSS) {
                line.setColor(rgb(0.49f, 0.70f, 0.95f, ink));
                float d = 0.32f * r;
                canvas.drawLine(x - d, y - d, x + d, y + d, line);
                canvas.drawLine(x - d, y + d, x + d, y - d, line);
            } else if (sym == CIRCLE) {
                line.setColor(rgb(1.0f, 0.45f, 0.45f, ink));
                canvas.drawCircle(x, y, 0.34f * r, line);
            } else if (sym == SQUARE) {
                line.setColor(rgb(0.93f, 0.62f, 0.86f, ink));
                line.setStrokeWidth(r * 0.14f);
                float d = 0.29f * r;
                canvas.drawRect(x - d, y - d, x + d, y + d, line);
            } else if (sym == TRIANGLE) {
                line.setColor(rgb(0.30f, 0.90f, 0.66f, ink));
                line.setStrokeWidth(r * 0.14f);
                triangle(x, y + 0.06f * r, 0.36f * r, 0, -1);
                canvas.drawPath(path, line);
            } else if (sym >= UP && sym <= RIGHT) {
                fill.setColor(rgb(1, 1, 1, ink));
                triangle(x, y, 0.3f * r, sym == LEFT ? -1 : sym == RIGHT ? 1 : 0, sym == UP ? -1 : sym == DOWN ? 1 : 0);
                canvas.drawPath(path, fill);
            } else if (sym == PAD) {
                fill.setColor(rgb(1, 1, 1, ink));
                box.set(x - 0.42f * r, y - 0.24f * r, x + 0.42f * r, y + 0.20f * r);
                canvas.drawRoundRect(box, 0.12f * r, 0.12f * r, fill);
                canvas.drawCircle(x - 0.33f * r, y + 0.12f * r, 0.17f * r, fill);
                canvas.drawCircle(x + 0.33f * r, y + 0.12f * r, 0.17f * r, fill);
                fill.setColor(rgb(0.2f, 0.2f, 0.22f, ink)); // the D-pad and two buttons, cut out
                canvas.drawRect(x - 0.37f * r, y - 0.052f * r, x - 0.17f * r, y + 0.012f * r, fill);
                canvas.drawRect(x - 0.302f * r, y - 0.12f * r, x - 0.238f * r, y + 0.08f * r, fill);
                canvas.drawCircle(x + 0.24f * r, y - 0.07f * r, 0.045f * r, fill);
                canvas.drawCircle(x + 0.33f * r, y + 0.02f * r, 0.045f * r, fill);
            } else if (sym == LOCK || sym == UNLOCK) {
                fill.setColor(rgb(1, 1, 1, ink));
                box.set(x - 0.28f * r, y - 0.06f * r, x + 0.28f * r, y + 0.34f * r);
                canvas.drawRoundRect(box, 0.05f * r, 0.05f * r, fill);
                line.setColor(rgb(1, 1, 1, ink));
                line.setStrokeWidth(0.12f * r);
                // the shackle: an arch over two legs, lifted and to the right when open
                float cx = x + (sym == LOCK ? 0 : 0.12f * r), cy = y + (sym == LOCK ? -0.1f * r : -0.22f * r);
                path.reset();
                path.moveTo(cx - 0.18f * r, cy + 0.14f * r);
                path.lineTo(cx - 0.18f * r, cy);
                arc.set(cx - 0.18f * r, cy - 0.18f * r, cx + 0.18f * r, cy + 0.18f * r);
                path.arcTo(arc, 180, 180);
                path.lineTo(cx + 0.18f * r, cy + 0.14f * r);
                canvas.drawPath(path, line);
            }
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    // run.args (one argument per line) replaces the default command line below.
    @Override
    protected String[] getArguments() {
        File base = getExternalFilesDir(null);
        File data = new File(base, "data");
        data.mkdirs();
        if (getIntent().getBooleanExtra("gfx_test", false))
            return new String[] {"--data-dir", data.getPath(), "--android-gfx-test"};
        File plan = new File(base, "run.args");
        if (plan.isFile()) {
            ArrayList<String> args = new ArrayList<>();
            try (BufferedReader in = new BufferedReader(new FileReader(plan))) {
                String line;
                while ((line = in.readLine()) != null)
                    if (!line.isEmpty())
                        args.add(line);
                return args.toArray(new String[0]);
            } catch (IOException e) {
                throw new IllegalStateException(e);
            }
        }
        return new String[] {"--game", new File(base, "game").getPath(), "--data-dir", data.getPath(), "--reg",
                new File(base, "ffxi.reg").getPath(), "--reg-overlay", new File(data, "settings.reg").getPath(),
                "--user-dir", new File(data, "USER").getPath(), "--server", "127.0.0.1",
                "--fps-divisor", "2", "--aspect", "off"};
    }
}

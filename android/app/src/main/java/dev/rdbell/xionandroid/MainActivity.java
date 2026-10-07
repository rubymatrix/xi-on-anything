package dev.rdbell.xionandroid;

import android.content.res.ColorStateList;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;

import org.libsdl.app.SDLActivity;

/* The test app's activity: SDL runs libmain.so (host/android_main.c) with the arguments from
 * getArguments(). The intent's touch_controls extra adds on-screen keys; gfx_test runs the offscreen
 * graphics fixture instead of the game. */
public final class MainActivity extends SDLActivity {
    private final ArrayList<Button> touchButtons = new ArrayList<>();
    private final TouchKeys touchKeys = new TouchKeys(new KeySink() {
        @Override
        public void down(int key) {
            SDLActivity.onNativeKeyDown(key);
        }

        @Override
        public void up(int key) {
            SDLActivity.onNativeKeyUp(key);
        }
    });

    interface KeySink {
        void down(int key);

        void up(int key);
    }

    // Several fingers may hold one key. Emit one down, then one final up.
    static final class TouchKeys {
        private final KeySink sink;
        private final LinkedHashMap<Integer, Integer> pointers = new LinkedHashMap<>();
        private final LinkedHashMap<Integer, Integer> counts = new LinkedHashMap<>();

        TouchKeys(KeySink sink) {
            this.sink = sink;
        }

        void press(int pointer, int key) {
            Integer previous = pointers.get(pointer);
            if (previous != null && previous == key)
                return;
            release(pointer);
            pointers.put(pointer, key);
            int count = counts.getOrDefault(key, 0);
            counts.put(key, count + 1);
            if (count == 0)
                sink.down(key);
        }

        void release(int pointer) {
            Integer key = pointers.remove(pointer);
            if (key == null)
                return;
            int count = counts.get(key);
            if (count == 1) {
                counts.remove(key);
                sink.up(key);
            } else
                counts.put(key, count - 1);
        }

        boolean held(int key) {
            return counts.containsKey(key);
        }

        void releaseAll() {
            for (Integer key : counts.keySet()) sink.up(key);
            counts.clear();
            pointers.clear();
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (!getIntent().getBooleanExtra("touch_controls", false) || mBrokenLibraries || mLayout == null)
            return;
        FrameLayout controls = new FrameLayout(this);
        controls.setFocusable(false);
        controls.setClickable(false);
        controls.setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
        controls.setMotionEventSplittingEnabled(true);
        mLayout.addView(controls,
                new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        addTouchButton(controls, "8 ↑", "Forward, keypad 8", KeyEvent.KEYCODE_NUMPAD_8, 1, 2, false);
        addTouchButton(controls, "4 ←", "Left, keypad 4", KeyEvent.KEYCODE_NUMPAD_4, 0, 1, false);
        addTouchButton(controls, "2 ↓", "Back, keypad 2", KeyEvent.KEYCODE_NUMPAD_2, 1, 0, false);
        addTouchButton(controls, "6 →", "Right, keypad 6", KeyEvent.KEYCODE_NUMPAD_6, 2, 1, false);
        addTouchButton(controls, "Enter", "Confirm, Enter", KeyEvent.KEYCODE_ENTER, 0, 0, true);
        addTouchButton(controls, "Esc", "Cancel, Escape", KeyEvent.KEYCODE_ESCAPE, 1, 0, true);
        addTouchButton(controls, "Menu", "Menu, keypad minus", KeyEvent.KEYCODE_NUMPAD_SUBTRACT, 2, 0, true);
        addTouchButton(controls, "Tab", "Change target, Tab", KeyEvent.KEYCODE_TAB, 0, 1, true);
        // MOVE_HOME reaches the game; Android HOME would address the launcher.
        addTouchButton(controls, "Home", "Home key", KeyEvent.KEYCODE_MOVE_HOME, 1, 1, true);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void addTouchButton(
            FrameLayout parent, String label, String description, final int key, int column, int row, boolean right) {
        final Button button = new Button(this);
        button.setText(label);
        button.setContentDescription(description);
        button.setAllCaps(false);
        button.setTextSize(14);
        button.setTextColor(Color.WHITE);
        button.setBackgroundTintList(ColorStateList.valueOf(Color.argb(175, 24, 24, 24)));
        button.setPadding(0, 0, 0, 0);
        button.setMinWidth(0);
        button.setMinHeight(0);
        button.setFocusable(false);
        button.setFocusableInTouchMode(false);
        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(dp(54), dp(48));
        layout.gravity = Gravity.BOTTOM | (right ? Gravity.RIGHT : Gravity.LEFT);
        layout.bottomMargin = dp(16 + row * 54);
        if (right)
            layout.rightMargin = dp(16 + (2 - column) * 60);
        else
            layout.leftMargin = dp(16 + column * 60);
        button.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View view, MotionEvent event) {
                int action = event.getActionMasked();
                int index = event.getActionIndex();
                if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                    touchKeys.press(event.getPointerId(index), key);
                } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
                    touchKeys.release(event.getPointerId(index));
                } else if (action == MotionEvent.ACTION_CANCEL) {
                    releaseTouchKeys();
                } else if (action == MotionEvent.ACTION_MOVE) {
                    for (int i = 0; i < event.getPointerCount(); ++i) {
                        float x = event.getX(i), y = event.getY(i);
                        if (x < 0 || y < 0 || x >= view.getWidth() || y >= view.getHeight())
                            touchKeys.release(event.getPointerId(i));
                    }
                }
                button.setPressed(touchKeys.held(key));
                return true;
            }
        });
        touchButtons.add(button);
        parent.addView(button, layout);
    }

    private void releaseTouchKeys() {
        touchKeys.releaseAll();
        for (Button button : touchButtons) button.setPressed(false);
    }

    @Override
    protected void onPause() {
        releaseTouchKeys();
        super.onPause();
    }

    @Override
    protected void onStop() {
        releaseTouchKeys();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        releaseTouchKeys();
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (!hasFocus)
            releaseTouchKeys();
        super.onWindowFocusChanged(hasFocus);
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

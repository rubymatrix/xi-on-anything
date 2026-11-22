package com.rubymatrix.xionanything;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

/* The launcher entry: makes the game's code, libgame.so, on the phone, from the player's own install,
 * then starts MainActivity, which loads it (MainActivity.loadLibraries). Nothing in the APK is game code.
 *
 * 1. The APK's assets go into this app's files once per APK (kit/kit.id says which): kit (the
 *    compiler's flags, headers and builtins), phone (recomp.py, unpack.py, phone_kit.py and this
 *    build's metadata, phone-kit.json) and pyhome (the Python library they use).
 * 2. phone_kit.py checks that the install's FFXiMain.dll and FFXi.dll are the build this APK is for,
 *    unpacks them and translates them into kit/src/<part> (the same unpack.py and recomp.py as a PC's
 *    build), and writes kit/src.id. The interpreter is lib_python.so, with libpython3.14.so beside it.
 * 3. LLVM's multi-call driver, lib_llvm.so, compiles kit/src and links libgame.so against libmain.so.
 *    It runs through links named clang and lld in files/bin: it picks its tool from the name it is
 *    run as, and an APK's libraries must be named lib*.so (llvm-lib, to it).
 *
 * The APK's native library folder is the one place an app may run a program from; the libraries the
 * programs and the game load may be in its files. libgame.id holds kit.id and src.id of the kit it
 * was built from. With no kit in the assets, the APK carries the game in libmain.so (the PC-built
 * APK) and MainActivity starts at once. */
public class BuildActivity extends Activity {
    private static final String TAG = "XIBuild";
    private TextView log;
    private ScrollView scroll;

    static File gameLibrary(File files) {
        return new File(files, "libgame.so");
    }

    private static String read(File f) {
        try {
            return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return null;
        }
    }

    private String assetId() {
        try (InputStream in = getAssets().open("kit/kit.id")) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8).trim() : null;
        } catch (IOException e) {
            return null;
        }
    }

    /* The folder holding the player's FFXiMain.dll, as MainActivity.getArguments finds it. */
    private File gameFolder() {
        File base = getExternalFilesDir(null);
        String chosen = read(new File(base, "game-dir.txt"));
        if (chosen != null && !chosen.isEmpty())
            return new File(chosen, "FINAL FANTASY XI");
        File game = new File(base, "FINAL FANTASY XI");
        return game.isDirectory() ? game : new File(base, "game");
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final File files = getFilesDir(), kit = new File(files, "kit");
        final String asset = assetId();
        String src = read(new File(kit, "src.id"));
        if (asset == null || (asset.equals(read(new File(kit, "kit.id"))) && src != null
                && (asset + "-" + src).equals(read(new File(files, "libgame.id"))))) {
            play();
            return;
        }
        log = new TextView(this);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextSize(11);
        log.setTextColor(Color.rgb(220, 220, 220));
        log.setBackgroundColor(Color.rgb(16, 16, 20));
        log.setPadding(32, 32, 32, 32);
        scroll = new ScrollView(this);
        scroll.addView(log);
        setContentView(scroll);
        // the SDK's android.jar has no LambdaMetafactory for javac: classes, not lambdas
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    long start = SystemClock.elapsedRealtime();
                    say("XI on Anything: making the game for this phone, from your own install");
                    if (!asset.equals(read(new File(kit, "kit.id")))) {
                        say("unpacking the tools");
                        for (String part : new String[] {"kit", "phone", "pyhome", "obj"})
                            delete(new File(files, part));
                        new File(files, "libgame.id").delete();
                        for (String part : new String[] {"phone", "pyhome", "kit"}) // kit.id last
                            copyAssets(part, new File(files, part));
                    }
                    if (read(new File(kit, "src.id")) == null)
                        translate(files, kit);
                    String id = asset + "-" + read(new File(kit, "src.id"));
                    build(files, kit, id);
                    say("ready in " + (SystemClock.elapsedRealtime() - start) / 1000 + " s");
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            play();
                        }
                    });
                } catch (Exception e) {
                    Log.e(TAG, "build failed", e);
                    say("FAILED: " + e.getMessage());
                }
            }
        }, "xi-build").start();
    }

    private static void delete(File f) {
        File[] children = f.isDirectory() && !Files.isSymbolicLink(f.toPath()) ? f.listFiles() : null;
        if (children != null)
            for (File c : children)
                delete(c);
        f.delete();
    }

    private void copyAssets(String path, File to) throws IOException {
        String[] children = getAssets().list(path);
        if (children != null && children.length > 0) {
            to.mkdirs();
            for (String c : children)
                copyAssets(path + "/" + c, new File(to, c));
            return;
        }
        File tmp = new File(to.getPath() + ".tmp");
        try (InputStream in = getAssets().open(path)) {
            Files.copy(in, tmp.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
        if (!tmp.renameTo(to))
            throw new IOException("could not write " + to);
    }

    /* phone_kit.py: the player's DLLs into kit/src, with the phone's Python. */
    private void translate(File files, File kit) throws Exception {
        File game = gameFolder();
        say("translating " + game + "/FFXiMain.dll and FFXi.dll");
        String libs = getApplicationInfo().nativeLibraryDir;
        File phone = new File(files, "phone"), home = new File(files, "pyhome");
        ProcessBuilder pb = new ProcessBuilder(libs + "/lib_python.so", "-u", "-B",
            new File(phone, "tools/phone_kit.py").getPath(), "--game", game.getPath(), "--kit", kit.getPath(),
            "--config", new File(phone, "phone-kit.json").getPath(),
            "--recomp", new File(phone, "recomp/recomp.py").getPath());
        pb.environment().put("PYTHONHOME", home.getPath());
        pb.environment().put("PYTHONPATH", new File(home, "site-packages").getPath());
        pb.environment().put("LD_LIBRARY_PATH", libs);
        pb.environment().put("TMPDIR", getCacheDir().getPath());
        pb.environment().put("HOME", files.getPath());
        Process p = pb.directory(files).redirectErrorStream(true).start();
        StringBuilder tail = new StringBuilder();
        try (BufferedReader in = new BufferedReader(new java.io.InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
            for (String line; (line = in.readLine()) != null;) {
                // phone_kit.py's steps and their times; recomp.py's statistics only go to the tail
                if (line.startsWith(">") || line.matches("  \\S.*: [0-9.]+ s") || line.startsWith("build ")
                        || line.startsWith("sources"))
                    say(line);
                tail.append(line).append('\n');
                if (tail.length() > 8000)
                    tail.delete(0, tail.length() - 4000);
            }
        }
        if (p.waitFor() != 0)
            throw new IOException("translation failed\n" + tail);
    }

    private void play() {
        startActivity(new Intent(this, MainActivity.class).putExtras(getIntent()));
        finish();
    }

    private void say(String line) {
        Log.i(TAG, line);
        if (log == null)
            return;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                log.append(line + "\n");
                scroll.fullScroll(ScrollView.FOCUS_DOWN);
            }
        });
    }

    private static String run(List<String> cmd, File dir) throws IOException, InterruptedException {
        Process p = new ProcessBuilder(cmd).directory(dir).redirectErrorStream(true).start();
        StringBuilder out = new StringBuilder();
        try (InputStream in = p.getInputStream()) {
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0;)
                out.append(new String(buf, 0, n, StandardCharsets.UTF_8));
        }
        int code = p.waitFor();
        if (code != 0)
            throw new IOException(String.join(" ", cmd.subList(0, Math.min(cmd.size(), 3))) + " exit " + code
                + "\n" + out.substring(Math.max(0, out.length() - 4000)));
        return out.toString();
    }

    private void build(File files, File kit, String id) throws Exception {
        long start = SystemClock.elapsedRealtime();
        // the driver picks its tool from argv[0]'s name, and an APK's libraries must be named lib*.so
        // (which reads as llvm-lib): links named for the tools, in the app's files, to the one binary
        String driver = getApplicationInfo().nativeLibraryDir + "/lib_llvm.so";
        File bin = new File(files, "bin");
        bin.mkdirs();
        for (String tool : new String[] {"clang", "lld"}) {
            File link = new File(bin, tool);
            link.delete();
            android.system.Os.symlink(driver, link.getPath());
        }
        String clang = new File(bin, "clang").getPath(), lld = new File(bin, "lld").getPath();
        String main = getApplicationInfo().nativeLibraryDir + "/libmain.so";
        List<String> cflags = new ArrayList<>();
        try (BufferedReader in = new BufferedReader(new FileReader(new File(kit, "cflags.txt")))) {
            for (String line; (line = in.readLine()) != null;)
                if (!line.trim().isEmpty())
                    cflags.add(line.trim().replace("@KIT@", kit.getPath()));
        }
        File src = new File(kit, "src"), objDir = new File(files, "obj");
        objDir.mkdirs();
        List<File> found = new ArrayList<>();
        File[] parts = src.listFiles();
        for (File part : parts == null ? new File[0] : parts) {
            File[] c = part.isDirectory() ? part.listFiles() : null;
            for (File f : c == null ? new File[0] : c)
                if (f.getName().endsWith(".c"))
                    found.add(f);
        }
        if (found.isEmpty())
            throw new IOException("no sources in " + src);
        File[] sources = found.toArray(new File[0]);
        Arrays.sort(sources, new java.util.Comparator<File>() { // biggest first
            @Override
            public int compare(File a, File b) {
                return Long.compare(b.length(), a.length());
            }
        });
        // about 400 MB a compiler at -O2: at most one a core, and one per 600 MB of the device's memory
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(mi);
        int jobs = Math.max(1, Math.min(Runtime.getRuntime().availableProcessors(), (int) (mi.availMem / (600L << 20))));
        say("compiling " + sources.length + " files, " + jobs + " at a time");
        say(run(Arrays.asList(clang, "--version"), files).split("\n")[0]);
        ExecutorService pool = Executors.newFixedThreadPool(jobs);
        List<Future<String>> results = new ArrayList<>();
        List<String> objects = new ArrayList<>();
        final int[] done = {0};
        for (File s : sources) {
            String name = s.getParentFile().getName() + "_" + s.getName();
            File o = new File(objDir, name.replace(".c", ".o"));
            File stamp = new File(objDir, name + ".id");
            objects.add(o.getPath());
            results.add(pool.submit(new java.util.concurrent.Callable<String>() {
                @Override
                public String call() throws Exception {
                    if (o.exists() && id.equals(read(stamp)))
                        return name + " (kept)";
                    long t = SystemClock.elapsedRealtime();
                    List<String> cmd = new ArrayList<>(Arrays.asList(clang));
                    cmd.addAll(cflags);
                    cmd.addAll(Arrays.asList("-c", s.getPath(), "-o", o.getPath()));
                    BuildActivity.run(cmd, s.getParentFile());
                    Files.write(stamp.toPath(), id.getBytes(StandardCharsets.UTF_8));
                    return name + " " + (SystemClock.elapsedRealtime() - t) / 1000.0 + " s";
                }
            }));
        }
        for (Future<String> r : results) {
            String line = r.get();
            synchronized (done) {
                done[0]++;
                say("[" + done[0] + "/" + sources.length + "] " + line);
            }
        }
        pool.shutdown();
        File tmp = new File(files, "libgame.so.tmp");
        List<String> link = new ArrayList<>(Arrays.asList(lld, "-flavor", "gnu", "-shared", "-soname", "libgame.so",
            "-z", "max-page-size=16384", "--hash-style=both", "-o", tmp.getPath()));
        link.addAll(objects);
        link.addAll(Arrays.asList(main, "/system/lib64/libc.so", "/system/lib64/libm.so",
            new File(kit, "lib/libclang_rt.builtins-aarch64-android.a").getPath()));
        say("linking libgame.so");
        run(link, files);
        if (!tmp.renameTo(gameLibrary(files)))
            throw new IOException("could not replace libgame.so");
        Files.write(new File(files, "libgame.id").toPath(), id.getBytes(StandardCharsets.UTF_8));
        say("compiled and linked in " + (SystemClock.elapsedRealtime() - start) / 1000 + " s");
    }
}

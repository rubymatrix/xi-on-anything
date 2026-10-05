"""The browser build (docs/web-port-plan.md): the same translation and runtime, compiled to wasm32
with Emscripten, for the local web server (tools/webserve.py) to serve to the player's own browser.

  python3 tools/build_web.py --game <FINAL FANTASY XI> [--node]

Guest addresses are wasm linear-memory addresses (no RT_GUEST_WINDOW): see guest.h and gwin.c.
SDL3 is not linked; runtime/portable/sdl_web.c stands in for it, fed by the page (tools/web/).
The addon host (LuaJIT) is not built: host/addons_none.c stands in for it.

--node builds for Node.js instead of a page, for running the game headless while the web pieces are
built (the game files read straight from disk through NODERAWFS): build/web-node/host64.js.

Output goes to build/web/ (or build/web-node/), never anywhere served publicly: the wasm is translated
from the player's own FFXiMain.dll (Square Enix code).

Needs Emscripten (emcc on PATH, or EMSDK set: source <emsdk>/emsdk_env.sh).
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
os.environ.setdefault('XI_TP_DIR', os.path.join(ROOT, 'build', 'web', 'third_party'))
os.environ['XI_CC'] = 'emcc'
os.environ['XI_CXX'] = 'em++'
os.environ['XI_AR'] = 'emar'
os.environ['XI_TP_CFLAGS'] = '-pthread'  # every object in a threaded wasm build needs atomics
import build  # noqa: E402
import build_posix as bp  # noqa: E402
import thirdparty  # noqa: E402

CFLAGS = ['-O2', '-std=c11', '-g2', '-pthread', '-fno-strict-aliasing', '-I', 'runtime', '-I', 'runtime/portable',
          '-I', 'generated', '-I', 'third_party/stb', '-I', 'third_party/sdl3/include', '-DXI_WEB=1', '-D_GNU_SOURCE', '-DGFX_QUEUE']
# host64 without the native graphics back ends, SDL3, Discord and the addon host
HOST = [s for s in bp.HOST_SOURCES if s not in bp.GFX_SOURCES and s != 'host/discord.c'] + [
    'runtime/portable/gfx_queue.c', 'runtime/portable/gfx_msl.c', 'runtime/portable/gfx_msl_shaders.c', 'runtime/portable/sdl_web.c', 'runtime/portable/net_web.c', 'runtime/portable/httpfs_web.c',
    'host/addons_none.c']
LINK = ['-pthread', '-sPROXY_TO_PTHREAD', '-sALLOW_MEMORY_GROWTH', '-sMAXIMUM_MEMORY=4GB', '-sINITIAL_MEMORY=64MB',
        '-sSTACK_SIZE=1MB', '-sDEFAULT_PTHREAD_STACK_SIZE=1MB', '-sPTHREAD_POOL_SIZE=24', '-sEXIT_RUNTIME',
        '-Wl,--wrap=sbrk', '-sERROR_ON_UNDEFINED_SYMBOLS=1', '-lm', '--js-library', 'tools/web/net.js',
        '-sEXPORTED_RUNTIME_METHODS=ENV,FS,stringToNewUTF8,HEAPU8', '-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_wn_deliver,_web_key,_web_text,_web_mouse_move,_web_mouse_button,_web_mouse_wheel,_web_focus,_web_close,_web_view_size,_web_gamepad,_web_audio_ring,_web_resize,_web_hud_frames,_web_hud_worst_us']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.expanduser('~/SquareEnix/FINAL FANTASY XI'))
    ap.add_argument('--node', action='store_true', help='build for Node.js (headless, files from disk)')
    ap.add_argument('--profile', action='store_true', help="keep function names (profilers show f_10012345, not wasm-function[812])")
    a = ap.parse_args()
    if not shutil.which('emcc'):
        raise SystemExit('emcc not found: source <emsdk>/emsdk_env.sh first')
    game = os.path.abspath(a.game)
    if not os.path.exists(os.path.join(game, 'FFXiMain.dll')):
        raise SystemExit('no FFXiMain.dll in %s (--game)' % game)
    bp.translate()  # the same translation as host64's (generated/), shared with the native build
    bp.run([sys.executable, 'recomp/recomp.py', '--meta', build.FFXI_META, '--image', bp.GEN_FFXI_IMAGE, '--retail',
            build.FFXI_RETAIL, '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])

    bp.CC, bp.CXX, bp.CFLAGS = ['emcc'], ['em++'], CFLAGS
    tls_flags = thirdparty.flags('mbedtls')
    thirdparty.build('mbedtls')
    out = os.path.join('build', 'web-node' if a.node else 'web')
    objs = bp.compile_stale(bp.generated('all'), 'build/web-obj/all', ['-I', 'generated/all'])
    objs += bp.compile_stale(bp.generated('ffxi'), 'build/web-obj/ffxi', ['-I', 'generated/ffxi'])
    # the graphics back end: none in Node (headless), WebGPU in the page (Dawn's webgpu.h, emdawnwebgpu)
    gfx = ['runtime/portable/gfx_null.c'] if a.node else ['runtime/portable/gfx_webgpu.c', 'runtime/portable/gfx_fx.c',
                                                          'runtime/portable/gfx_scene.c']
    port = [] if a.node else ['--use-port=emdawnwebgpu']
    objdir = 'build/web-obj/' + ('host-node' if a.node else 'host')
    objs += bp.compile_stale(bp.PORTABLE + HOST + gfx, objdir, tls_flags + port)
    env = ['-sENVIRONMENT=node', '-sNODERAWFS', '--pre-js', 'tools/web/node_pre.js', '--js-library', 'tools/web/gfx.js'] if a.node else \
        ['-sENVIRONMENT=web,worker', '--pre-js', 'tools/web/web_pre.js', '--js-library', 'tools/web/gfx.js'] + port
    os.makedirs(os.path.join(ROOT, out), exist_ok=True)
    names = ['--profiling-funcs'] if a.profile else []
    bp.run(['em++', '-O2', '-o', os.path.join(out, 'host64.js')] + objs + [thirdparty.archive('mbedtls')] + LINK + env + names)
    if not a.node:
        for page in ('index.html', 'input.js', 'hud.js', 'audio-worklet.js'):
            shutil.copy(os.path.join(ROOT, 'tools', 'web', page), os.path.join(ROOT, out, page))
    print('built %s/host64.js' % out)


if __name__ == '__main__':
    main()

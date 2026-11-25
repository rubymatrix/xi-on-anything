/* The phone's Python, for the translator (tools/phone_kit.py): CPython for iOS (its Python.xcframework,
 * linked into the launcher's app), run in the app's own process, as iOS starts no program of an app's.
 * Android runs the same script with its own python (BuildActivity); here phone_kit.py runs each step
 * in this interpreter too (its sys.platform is 'ios').
 *
 *   xi_py_run(home, script, argc, argv, log): runs script as __main__ with sys.argv [script, argv...],
 *       its output (stdout and stderr) appended to log. home: the app's python/ (lib/python3.14, as
 *       CPython's own iOS build installs it); site: folders put first on sys.path (pefile, capstone's
 *       binding), ':' between them. Returns the script's exit code (0, or 1 for an exception, its
 *       traceback in the log). One at a time; the interpreter stays for the next run. */

#include <Python.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_started;

static int start(const char* home)
{
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    config.write_bytecode = 0; /* the app's bundle can't be written */
    config.buffered_stdio = 0;
    PyStatus st = PyConfig_SetBytesString(&config, &config.home, home);
    if (!PyStatus_Exception(st))
        st = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(st))
    {
        fprintf(stderr, "[python] didn't start: %s\n", st.err_msg ? st.err_msg : "?");
        return 0;
    }
    PyEval_SaveThread(); /* the GIL is taken per run, on whichever thread calls */
    return 1;
}

static const char RUN[] =
    "import os, runpy, sys, traceback\n"
    "def _xi_run(script, argv, log, site):\n"
    "    out = open(log, 'a', buffering=1, encoding='utf-8', errors='replace')\n"
    "    old = sys.stdout, sys.stderr, sys.argv[:], sys.path[:]\n"
    "    sys.stdout = sys.stderr = out\n"
    "    sys.argv = [script] + list(argv)\n"
    "    sys.path[0:0] = [os.path.dirname(script)] + [p for p in site.split(':') if p]\n"
    "    code = 0\n"
    "    try:\n"
    "        runpy.run_path(script, run_name='__main__')\n"
    "    except SystemExit as e:\n"
    "        code = e.code if isinstance(e.code, int) else (0 if e.code is None else (print(e.code), 1)[1])\n"
    "    except BaseException:\n"
    "        traceback.print_exc()\n"
    "        code = 1\n"
    "    finally:\n"
    "        out.flush()\n"
    "        sys.stdout, sys.stderr, sys.argv, sys.path[:] = old[0], old[1], old[2], old[3]\n"
    "        out.close()\n"
    "    return code\n";

int xi_py_run(const char* home, const char* site, const char* script, int argc, const char** argv, const char* log)
{
    pthread_mutex_lock(&g_lock);
    if (!g_started && !(g_started = start(home)))
    {
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    int code = 1;
    PyObject* main = PyImport_AddModule("__main__"); /* borrowed */
    PyObject* globals = main ? PyModule_GetDict(main) : NULL;
    PyObject* fn = globals ? PyDict_GetItemString(globals, "_xi_run") : NULL;
    if (!fn && globals)
    {
        PyObject* r = PyRun_String(RUN, Py_file_input, globals, globals);
        Py_XDECREF(r);
        fn = PyDict_GetItemString(globals, "_xi_run");
    }
    PyObject* args = PyList_New(0);
    for (int i = 0; i < argc; ++i)
    {
        PyObject* s = PyUnicode_DecodeFSDefault(argv[i]);
        PyList_Append(args, s);
        Py_DECREF(s);
    }
    PyObject* r = fn ? PyObject_CallFunction(fn, "sOss", script, args, log, site ? site : "") : NULL;
    if (r)
    {
        code = (int)PyLong_AsLong(r);
        Py_DECREF(r);
    }
    else
        PyErr_Print();
    Py_DECREF(args);
    PyGILState_Release(gil);
    pthread_mutex_unlock(&g_lock);
    return code;
}

// The phone's compiler and linker: LLVM 18's clang and lld (build/ios-llvm, tools/build_ios.py
// toolchain), called in the app's own process. iOS lets an app start no program of its own, so where
// Android's BuildActivity runs clang and ld.lld (build_android.py --split-game), the launcher's app
// calls these. The game library they make is the player's own: the kit (tools/build_ios.py kit) holds
// no game code, and the translation is made on the phone from the player's install (phone_kit.py).
//
//   xi_cc(argc, argv, err, n): a clang command line (argv[0] "clang"), as the driver would run it: each
//       job it makes (cc1) run here. 0 when it compiled; err gets the diagnostics.
//   xi_ld(argc, argv, err, n): an ld64.lld command line (argv[0] "ld64.lld"). 0 when it linked.
//
// Both may run on several threads at once (a compile each); xi_ld runs alone.

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/FrontendTool/Utils.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

LLD_HAS_DRIVER(macho)

using namespace clang;

namespace {

std::once_flag g_init;

void init()
{
    std::call_once(g_init, [] {
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
    });
}

void put(const std::string& text, char* err, size_t n)
{
    if (!err || !n)
        return;
    size_t k = text.size() < n - 1 ? text.size() : n - 1;
    // the end of it: the last errors are the ones that say what went wrong
    std::memcpy(err, text.data() + (text.size() - k), k);
    err[k] = 0;
}

int cc1(llvm::ArrayRef<const char*> args, const char* argv0, llvm::raw_ostream& out)
{
    auto ci = std::make_unique<CompilerInstance>();
    IntrusiveRefCntPtr<DiagnosticIDs> ids(new DiagnosticIDs());
    IntrusiveRefCntPtr<DiagnosticOptions> opts = new DiagnosticOptions();
    TextDiagnosticPrinter* printer = new TextDiagnosticPrinter(out, &*opts);
    DiagnosticsEngine diags(ids, &*opts, printer);
    if (!CompilerInvocation::CreateFromArgs(ci->getInvocation(), args, diags, argv0))
        return 1;
    ci->createDiagnostics(new TextDiagnosticPrinter(out, &ci->getDiagnosticOpts()), true);
    if (!ci->hasDiagnostics())
        return 1;
    bool ok = ExecuteCompilerInvocation(ci.get());
    ci->getDiagnosticClient().finish();
    return ok ? 0 : 1;
}

}  // namespace

extern "C" int xi_cc(int argc, const char** argv, char* err, size_t n)
{
    init();
    std::string text;
    llvm::raw_string_ostream out(text);
    int rc = 1;
    {
        IntrusiveRefCntPtr<DiagnosticIDs> ids(new DiagnosticIDs());
        IntrusiveRefCntPtr<DiagnosticOptions> opts = new DiagnosticOptions();
        TextDiagnosticPrinter* printer = new TextDiagnosticPrinter(out, &*opts);
        DiagnosticsEngine diags(ids, &*opts, printer);
        // the driver's own path only names its tools, never run here; -resource-dir comes in argv
        driver::Driver driver("/xi/bin/clang", "arm64-apple-ios17.0", diags, "clang");
        driver.CC1Main = nullptr;
        std::vector<const char*> all(argv, argv + argc);
        std::unique_ptr<driver::Compilation> c(driver.BuildCompilation(all));
        if (c && !c->containsError())
        {
            rc = 0;
            for (const driver::Command& job : c->getJobs())
            {
                const llvm::opt::ArgStringList& a = job.getArguments();
                if (a.empty() || std::strcmp(a[0], "-cc1") != 0)
                {
                    out << "xi_cc: a job other than the compiler's: " << job.getExecutable() << "\n";
                    rc = 1;
                    break;
                }
                if ((rc = cc1(llvm::ArrayRef<const char*>(a).drop_front(1), "/xi/bin/clang", out)) != 0)
                    break;
            }
        }
    }
    out.flush();
    put(text, err, n);
    return rc;
}

extern "C" int xi_ld(int argc, const char** argv, char* err, size_t n)
{
    init();
    std::string text;
    llvm::raw_string_ostream out(text);
    std::vector<const char*> all(argv, argv + argc);
    lld::Result r = lld::lldMain(all, out, out, {{lld::Darwin, &lld::macho::link}});
    out.flush();
    put(text, err, n);
    return r.retCode;
}

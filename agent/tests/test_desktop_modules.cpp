/**
 * The desktop module seam (ADR-0021): backends load at runtime from separate packages, so the
 * core carries no X11 or Wayland dependency. What is tested here is the loading and, just as
 * importantly, what a robot is told when nothing loads — "unavailable" on its own is useless to
 * whoever has to fix it.
 */
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "desktop/module_loader.hpp"

using namespace fjarr::desktop;

namespace {
struct Env {
    explicit Env(const char* k, const char* v) : key(k) {
        if (v) ::setenv(k, v, 1);
        else ::unsetenv(k);
    }
    ~Env() { ::unsetenv(key); }
    const char* key;
};

std::filesystem::path temp_dir(const std::string& tag) {
    const auto d = std::filesystem::temp_directory_path() / ("fjarr-desktop-" + tag + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}
} // namespace

TEST(DesktopModules, aRobotWithNoModuleIsToldWhichPackageToInstall) {
    // The normal state of a robot that streams a camera and has no desktop. It is not an error,
    // and the message has to be actionable rather than a bare "unavailable".
    ModuleLoader loader;
    loader.scan((temp_dir("empty")).string());
    EXPECT_TRUE(loader.found().empty());
    EXPECT_EQ(loader.select("auto"), nullptr);
    const std::string why = loader.unavailable_reason("auto");
    EXPECT_NE(why.find("fjarr-desktop-x11"), std::string::npos) << why;
    EXPECT_NE(why.find("fjarr-desktop-wayland"), std::string::npos) << why;
}

TEST(DesktopModules, aMissingDirectoryIsNotAnError) {
    ModuleLoader loader;
    EXPECT_NO_THROW(loader.scan("/definitely/not/here"));
    EXPECT_TRUE(loader.found().empty());
}

TEST(DesktopModules, loadsAModuleFromItsOwnSharedObjectAndMakesABackend) {
    ModuleLoader loader;
    loader.scan(FJARR_STUB_MODULE_DIR);
    ASSERT_EQ(loader.found().size(), 1u) << "the stub module was not discovered";
    const Found& f = loader.found().front();
    EXPECT_EQ(f.display_server, "stub");
    EXPECT_EQ(f.package, "fjarr-desktop-stub");
    EXPECT_TRUE(f.usable) << f.reason;

    const Found* chosen = loader.select("auto");
    ASSERT_NE(chosen, nullptr);
    std::string error;
    auto backend = loader.create(*chosen, &error);
    EXPECT_NE(backend, nullptr) << error;
    EXPECT_TRUE(backend->monitors().empty());
}

TEST(DesktopModules, anInstalledModuleThatCannotRunHereSaysWhy) {
    // "Installed" and "usable" are different questions: a Wayland module on an X11 session is
    // present and useless, and the operator needs the second answer, not the first.
    Env unusable("FJARR_STUB_UNUSABLE", "no DISPLAY in this session");
    ModuleLoader loader;
    loader.scan(FJARR_STUB_MODULE_DIR);
    ASSERT_EQ(loader.found().size(), 1u);
    EXPECT_FALSE(loader.found().front().usable);
    EXPECT_EQ(loader.select("auto"), nullptr);
    EXPECT_NE(loader.unavailable_reason("auto").find("no DISPLAY in this session"), std::string::npos)
        << loader.unavailable_reason("auto");
}

TEST(DesktopModules, aConfiguredBackendThatIsNotInstalledIsNotSubstituted) {
    // `backend = "wayland"` with only an X11 module installed must fail, not quietly use X11:
    // the operator asked for something specific, probably for a reason.
    ModuleLoader loader;
    loader.scan(FJARR_STUB_MODULE_DIR);
    ASSERT_FALSE(loader.found().empty());
    EXPECT_NE(loader.select("stub"), nullptr);
    EXPECT_EQ(loader.select("wayland"), nullptr);
    EXPECT_NE(loader.unavailable_reason("wayland").find("backend = \"wayland\""), std::string::npos)
        << loader.unavailable_reason("wayland");
}

TEST(DesktopModules, somethingThatIsNotOurModuleIsIgnoredWithAReasonRatherThanLoaded) {
    // A stray .so in the directory — another project's, or one built against an older seam. The
    // version is in the symbol name, so the wrong one is simply not found.
    const auto dir = temp_dir("stray");
    std::ofstream(dir / "libnot-a-module.so") << "this is not an ELF file";
    ModuleLoader loader;
    loader.scan(dir.string());
    ASSERT_EQ(loader.found().size(), 1u);
    EXPECT_FALSE(loader.found().front().usable);
    EXPECT_FALSE(loader.found().front().reason.empty());
    EXPECT_EQ(loader.select("auto"), nullptr);
}

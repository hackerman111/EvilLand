#include "../../Log.hpp"
#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <format>
#include <optional>
#include <string>

#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Hyprutils::OS;
using namespace Hyprutils::Utils;

static bool runToplevelCapture(const std::string& appID) {
    CProcess process(std::format("{}/toplevel-capture", binaryDir), {appID});
    process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);

    if (!process.runSync()) {
        NLog::log("{}Failed to run toplevel-capture helper", Colors::RED);
        return false;
    }

    if (process.exitCode() == 0)
        return true;

    NLog::log("{}toplevel-capture helper failed with exit code {}", Colors::RED, process.exitCode());
    return false;
}

static std::optional<std::string> monitorChecksum() {
    CProcess process(std::format("{}/toplevel-capture", binaryDir), {"--monitor", "HEADLESS-1"});
    process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    if (!process.runSync() || process.exitCode() != 0 || process.stdOut().empty())
        return std::nullopt;
    return process.stdOut();
}

SUBTEST(privateMonitorWindow, bool fullscreen) {
    CScopeGuard cleanup([&] {
        Tests::killAllWindows();
        Tests::killAllLayers();
        getFromSocket("/reload");
    });
    ASSERT(Tests::killAllWindows(), true);
    ASSERT(Tests::killAllLayers(), true);
    OK(getFromSocket("/eval hl.config({ animations = { enabled = false }, ecosystem = { enforce_permissions = false } })"));
    OK(getFromSocket("/dispatch hl.dsp.focus({ monitor = 'HEADLESS-1' })"));
    OK(getFromSocket("/eval hl.window_rule({ name = 'private-monitor-window', match = { class = '^private-monitor-window$' }, "
                     "float = true, hide_from_screen_share = true })"));

    auto background = Tests::spawnLayerKitty("capture-background", {"--edge", "background", "--output-name", "HEADLESS-1", "--override", "background=#0044cc", "sleep", "60"});
    ASSERT(sc<bool>(background), true);
    const auto BASELINE = monitorChecksum();
    ASSERT(BASELINE.has_value(), true);
    auto kitty = Tests::spawnKitty("private-monitor-window", {"--override", "background=#ff0000"});
    ASSERT(sc<bool>(kitty), true);
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "class: private-monitor-window");
    if (fullscreen)
        OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'set' })"));

    EXPECT(monitorChecksum() == BASELINE, true);
    OK(getFromSocket("/dispatch hl.dsp.window.set_prop({ window = 'class:private-monitor-window', prop = 'hide_from_screen_share', value = 'off' })"));
    const auto VISIBLE = monitorChecksum();
    ASSERT(VISIBLE.has_value(), true);
    ASSERT(VISIBLE != BASELINE, true);
    OK(getFromSocket("/dispatch hl.dsp.window.set_prop({ window = 'class:private-monitor-window', prop = 'hide_from_screen_share', value = 'on' })"));
    EXPECT(monitorChecksum() == BASELINE, true);
    OK(getFromSocket("/dispatch hl.dsp.window.set_prop({ window = 'class:private-monitor-window', prop = 'no_screen_share', value = 'on' })"));
    EXPECT(monitorChecksum() == BASELINE, true);
    OK(getFromSocket("/dispatch hl.dsp.window.set_prop({ window = 'class:private-monitor-window', prop = 'hide_from_screen_share', value = 'off' })"));
    const auto MASKED = monitorChecksum();
    ASSERT(MASKED.has_value(), true);
    EXPECT(MASKED != BASELINE, true);
    EXPECT(MASKED != VISIBLE, true);
}

TEST_CASE(monitorCaptureOmitsPrivateWindow) {
    CALL_SUBTEST(privateMonitorWindow, false);
}

TEST_CASE(monitorCaptureOmitsPrivateFullscreenWindow) {
    CALL_SUBTEST(privateMonitorWindow, true);
}

TEST_CASE(monitorCaptureOmitsPrivateLayer) {
    CScopeGuard cleanup([&] {
        Tests::killAllLayers();
        getFromSocket("/reload");
    });
    ASSERT(Tests::killAllWindows(), true);
    ASSERT(Tests::killAllLayers(), true);
    OK(getFromSocket("/eval hl.config({ animations = { enabled = false }, ecosystem = { enforce_permissions = false } })"));
    OK(getFromSocket("/dispatch hl.dsp.focus({ monitor = 'HEADLESS-1' })"));
    OK(getFromSocket("/eval hl.layer_rule({ name = 'private-monitor-layer', match = { namespace = '^private-monitor-layer$' }, hide_from_screen_share = true })"));

    auto background = Tests::spawnLayerKitty("capture-background", {"--edge", "background", "--output-name", "HEADLESS-1", "--override", "background=#0044cc", "sleep", "60"});
    ASSERT(sc<bool>(background), true);
    const auto BASELINE = monitorChecksum();
    ASSERT(BASELINE.has_value(), true);
    auto kitty = Tests::spawnLayerKitty("private-monitor-layer",
                                        {"--edge", "center", "--layer", "overlay", "--exclusive-zone", "0", "--output-name", "HEADLESS-1", "--override", "background=#ff0000"});
    ASSERT(sc<bool>(kitty), true);
    EXPECT(monitorChecksum() == BASELINE, true);

    OK(getFromSocket("/eval hl.layer_rule({ name = 'private-monitor-layer', match = { namespace = '^private-monitor-layer$' }, hide_from_screen_share = false })"));
    const auto VISIBLE = monitorChecksum();
    ASSERT(VISIBLE.has_value(), true);
    ASSERT(VISIBLE != BASELINE, true);
}

TEST_CASE(windowShareCaptureProducesFrame) {
    constexpr auto APP_ID = "hyprtester-toplevel-capture";

    CScopeGuard    guard = {[&]() { Tests::killAllWindows(); }};

    auto           kitty = Tests::spawnKitty(APP_ID);
    if (!kitty)
        FAIL_TEST("Could not spawn kitty with class: {}", APP_ID);

    ASSERT(Tests::windowCount(), 1);

    ASSERT(runToplevelCapture(APP_ID), true);
}

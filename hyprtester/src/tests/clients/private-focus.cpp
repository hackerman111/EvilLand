#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "tests.hpp"
#include "build.hpp"

#include <hyprutils/os/FileDescriptor.hpp>
#include <hyprutils/os/Process.hpp>
#include <csignal>
#include <chrono>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>

struct SFocusStats {
    bool     focused = false, active = false;
    uint32_t enters = 0, leaves = 0, presses = 0, releases = 0;
};

class CFocusClient {
  public:
    CFocusClient(const std::string& appID) {
        int input[2]{}, output[2]{};
        if (pipe(input) != 0 || pipe(output) != 0)
            throw std::runtime_error("focus client pipes failed");

        m_input   = Hyprutils::OS::CFileDescriptor(input[1]);
        m_output  = Hyprutils::OS::CFileDescriptor(output[0]);
        m_process = Hyprutils::Memory::makeShared<Hyprutils::OS::CProcess>(binaryDir + "/keyboard-modifiers", std::vector<std::string>{appID});
        m_process->addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        m_process->setStdinFD(input[0]);
        m_process->setStdoutFD(output[1]);
        const bool STARTED = m_process->runAsync();
        close(input[0]);
        close(output[1]);
        if (!STARTED || receive() != "started\n")
            throw std::runtime_error("focus client failed to start");
    }

    ~CFocusClient() {
        kill(m_process->pid(), SIGTERM);
    }

    std::string selector() {
        return std::format("pid:{}", m_process->pid());
    }

    void focus() {
        if (getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = '{}' }})", selector())) != "ok")
            throw std::runtime_error("focus dispatcher failed");
    }

    void setProp(const std::string& prop, bool value) {
        if (getFromSocket(std::format("/dispatch hl.dsp.window.set_prop({{ window = '{}', prop = '{}', value = '{}' }})", selector(), prop, value ? "on" : "off")) != "ok")
            throw std::runtime_error("focus property failed");
    }

    void moveToWorkspace(const std::string& workspace) {
        if (getFromSocket(std::format("/dispatch hl.dsp.window.move({{ window = '{}', workspace = '{}', follow = false }})", selector(), workspace)) != "ok")
            throw std::runtime_error("focus client workspace move failed");
    }

    void floating() {
        if (getFromSocket(std::format("/dispatch hl.dsp.window.float({{ window = '{}', action = 'enable' }})", selector())) != "ok")
            throw std::runtime_error("focus client floating failed");
    }

    bool activeWindow() {
        return Tests::getAttribute(getFromSocket("/activewindow"), "pid") == selector().substr(4);
    }

    std::string attribute(const std::string& name) {
        return Tests::getAttribute(getFromSocket("/activewindow"), name);
    }

    bool suspended() {
        if (write(m_input.get(), "suspended\n", 10) != 10)
            throw std::runtime_error("suspend query failed");
        return receive() == "true\n";
    }

    void unmap() {
        if (write(m_input.get(), "unmap\n", 6) != 6 || receive() != "unmapped\n")
            throw std::runtime_error("focus client unmap failed");
    }

    void setTitle(const std::string& title) {
        const auto COMMAND = "title " + title + "\n";
        if (write(m_input.get(), COMMAND.data(), COMMAND.size()) != static_cast<ssize_t>(COMMAND.size()) || receive() != "titled\n")
            throw std::runtime_error("focus client title update failed");
    }

    SFocusStats stats() {
        if (write(m_input.get(), "stats\n", 6) != 6)
            throw std::runtime_error("focus query failed");
        SFocusStats        stats;
        std::istringstream input(receive());
        if (!(input >> std::boolalpha >> stats.focused >> stats.active >> stats.enters >> stats.leaves >> stats.presses >> stats.releases))
            throw std::runtime_error("invalid focus statistics");
        return stats;
    }

  private:
    std::string receive() {
        pollfd fd{.fd = m_output.get(), .events = POLLIN};
        if (poll(&fd, 1, 3000) != 1 || !(fd.revents & POLLIN))
            throw std::runtime_error("focus client timed out");
        std::array<char, 256> buffer{};
        const auto            COUNT = read(fd.fd, buffer.data(), buffer.size());
        if (COUNT <= 0)
            throw std::runtime_error("focus client disconnected");
        return {buffer.data(), static_cast<size_t>(COUNT)};
    }

    Hyprutils::Memory::CSharedPointer<Hyprutils::OS::CProcess> m_process;
    Hyprutils::OS::CFileDescriptor                             m_input, m_output;
};

static void sendFocusTestKey(uint32_t key, bool pressed) {
    if (getFromSocket(std::format("/eval hl.plugin.test.keybind({}, 0, {})", pressed ? 1 : 0, key)) != "ok")
        throw std::runtime_error("focus test key failed");
}

class CFocusEvents {
  public:
    CFocusEvents() : m_socket(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        const auto  RUNTIME = getenv("XDG_RUNTIME_DIR");
        const auto  PATH    = std::format("{}/hypr/{}/.socket2.sock", RUNTIME ? RUNTIME : std::format("/run/user/{}", getuid()), HIS);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (PATH.size() >= sizeof(address.sun_path))
            throw std::runtime_error("focus event socket path too long");

        std::ranges::copy(PATH, address.sun_path);
        if (connect(m_socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("focus event socket connection failed");
    }

    std::string receive() {
        std::string            events;
        std::array<char, 4096> buffer{};
        pollfd                 fd{.fd = m_socket.get(), .events = POLLIN};
        while (poll(&fd, 1, 100) == 1) {
            const auto COUNT = read(fd.fd, buffer.data(), buffer.size());
            if (COUNT <= 0)
                throw std::runtime_error("focus event socket disconnected");

            events.append(buffer.data(), COUNT);
        }
        return events;
    }

  private:
    Hyprutils::OS::CFileDescriptor m_socket;
};

TEST_CASE(captureHiddenFocusPreservesActiveWindow) {
    CFocusClient browser("public-browser"), hidden("public-private"), other("public-other");
    hidden.setProp("hide_from_screen_share", true);
    browser.focus();
    const auto   HISTORY = browser.attribute("focusHistoryID");
    CFocusEvents events;
    hidden.focus();
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.attribute("focusHistoryID"), HISTORY);
    EXPECT(hidden.stats().focused, true);
    EXPECT(browser.stats().focused, false);
    sendFocusTestKey(38, true);
    sendFocusTestKey(38, false);
    EXPECT(hidden.stats().presses, 1U);
    EXPECT(events.receive().contains("activewindow"), false);
    hidden.setTitle("private title");
    EXPECT(browser.attribute("title"), std::string("keyboard-modifiers test client"));
    EXPECT(events.receive().contains("activewindow"), false);
    browser.setTitle("public title");
    EXPECT(browser.attribute("title"), std::string("public title"));
    EXPECT_CONTAINS(events.receive(), "activewindow>>public-browser,public title");
    browser.focus();
    EXPECT(browser.activeWindow(), true);
    EXPECT(events.receive().contains("activewindow"), false);
    other.focus();
    EXPECT(other.activeWindow(), true);
    EXPECT_CONTAINS(events.receive(), "activewindow>>public-other,");
}

TEST_CASE(captureHiddenFocusSurvivesPrivateChainAndUnmap) {
    CFocusClient browser("public-browser"), first("public-first"), second("public-second");
    first.setProp("hide_from_screen_share", true);
    second.setProp("hide_from_screen_share", true);
    browser.focus();
    CFocusEvents events;
    first.focus();
    second.focus();
    EXPECT(browser.activeWindow(), true);
    EXPECT(second.stats().focused, true);
    second.unmap();
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().focused, true);
    EXPECT(events.receive().contains("activewindow"), false);
}

TEST_CASE(captureHiddenFocusReleasedWhenDisabled) {
    CFocusClient browser("public-browser"), hidden("public-private");
    hidden.setProp("hide_from_screen_share", true);
    browser.focus();
    hidden.focus();
    CFocusEvents events;
    hidden.setProp("hide_from_screen_share", false);
    EXPECT(hidden.activeWindow(), true);
    EXPECT_CONTAINS(events.receive(), "activewindow>>public-private,");
}

TEST_CASE(captureHiddenFocusReleasesAfterOriginalCloses) {
    CFocusClient browser("public-browser"), hidden("public-private"), other("public-other");
    hidden.setProp("hide_from_screen_share", true);
    browser.focus();
    hidden.focus();
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.close({{ window = '{}' }})", browser.selector())));
    Tests::waitUntilWindowsN(2);
    other.focus();
    EXPECT(other.activeWindow(), true);
    EXPECT(other.stats().focused, true);
}

TEST_CASE(captureHiddenFocusKeepsActiveWindowOnKeyboardLeave) {
    CFocusClient browser("public-browser"), hidden("public-private");
    hidden.setProp("hide_from_screen_share", true);
    browser.focus();
    hidden.focus();
    CFocusEvents events;
    OK(getFromSocket("/eval hl.plugin.test.nullfocus()"));
    EXPECT(browser.activeWindow(), true);
    EXPECT(hidden.stats().focused, false);
    EXPECT(events.receive().contains("activewindow"), false);
}

TEST_CASE(captureHiddenFocusReleasedOnEmptyWorkspace) {
    CFocusClient browser("public-browser"), hidden("public-private");
    hidden.setProp("hide_from_screen_share", true);
    browser.focus();
    hidden.focus();
    CFocusEvents events;
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '3' })"));
    EXPECT(browser.activeWindow() || hidden.activeWindow(), false);
    EXPECT(hidden.stats().focused, false);
    EXPECT_CONTAINS(events.receive(), "activewindow>>,\n");
}

TEST_CASE(captureHiddenFocusReturnsInputOnOriginalClick) {
    CFocusClient browser("public-browser"), hidden("public-private");
    OK(getFromSocket("/eval hl.config({ input = { follow_mouse = 0 } })"));
    browser.focus();
    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'set' })"));
    hidden.floating();
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ window = '{}', x = 200, y = 200 }})", hidden.selector())));
    hidden.setProp("hide_from_screen_share", true);
    hidden.focus();
    CFocusEvents events;
    EXPECT(hidden.stats().focused, true);
    OK(getFromSocket("/dispatch hl.dsp.cursor.move({ x = 10, y = 10 })"));
    OK(getFromSocket("/eval hl.plugin.test.click(272, 1)"));
    OK(getFromSocket("/eval hl.plugin.test.click(272, 0)"));
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().focused, true);
    EXPECT(hidden.stats().focused, false);
    EXPECT(events.receive().contains("activewindow"), false);
}

TEST_CASE(captureHiddenFocusUsesNormalFocusAcrossWorkspaces) {
    CFocusClient browser("public-browser"), hidden("public-private");
    hidden.setProp("hide_from_screen_share", true);
    hidden.moveToWorkspace("2");
    browser.focus();
    hidden.focus();
    EXPECT(hidden.activeWindow(), true);
    EXPECT(hidden.stats().focused, true);
    EXPECT(browser.stats().focused, false);
}

TEST_CASE(privateWindowPreservesPreviousFocus) {
    CFocusClient browser("focus-browser"), hidden("focus-private"), other("focus-other");
    OK(getFromSocket(
        "/eval hl.window_rule({ name = 'focus-private-rule', match = { class = '^focus-private$' }, hide_from_screen_share = true, preserve_previous_focus = true })"));
    browser.focus();
    sendFocusTestKey(38, true);
    const auto BEFORE = browser.stats();
    hidden.focus();
    const auto PRESERVED = browser.stats();
    EXPECT(PRESERVED.focused && PRESERVED.active, true);
    EXPECT(PRESERVED.leaves, BEFORE.leaves);
    EXPECT(PRESERVED.releases, BEFORE.releases + 1);
    EXPECT(hidden.stats().focused, true);
    sendFocusTestKey(38, false);
    sendFocusTestKey(56, true);
    sendFocusTestKey(56, false);
    EXPECT(hidden.stats().presses, 1U);
    EXPECT(browser.stats().presses, BEFORE.presses);
    browser.focus();
    const auto RETURNED = browser.stats();
    EXPECT(RETURNED.focused && RETURNED.active, true);
    EXPECT(RETURNED.enters, BEFORE.enters);
    EXPECT(RETURNED.leaves, BEFORE.leaves);
    EXPECT(hidden.stats().focused, false);
    other.focus();
    EXPECT(browser.stats().focused, false);
    EXPECT(browser.stats().active, false);
}

TEST_CASE(privateFocusReleasedForThirdWindow) {
    CFocusClient browser("focus-browser"), hidden("focus-private"), other("focus-other");
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    browser.focus();
    hidden.focus();
    EXPECT(browser.stats().focused, true);
    other.focus();
    EXPECT(browser.stats().focused || browser.stats().active, false);
    EXPECT(hidden.stats().focused || hidden.stats().active, false);
    EXPECT(other.stats().focused && other.stats().active, true);
}

TEST_CASE(privateFocusReleasedWhenDisabled) {
    CFocusClient browser("focus-browser"), hidden("focus-private");
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    browser.focus();
    hidden.focus();
    EXPECT(browser.stats().focused, true);
    hidden.setProp("preserve_previous_focus", false);
    EXPECT(browser.stats().focused || browser.stats().active, false);
    EXPECT(hidden.stats().focused && hidden.stats().active, true);
    browser.focus();
    hidden.focus();
    EXPECT(browser.stats().focused, false);
}

TEST_CASE(privateFocusRequiresCaptureHiding) {
    CFocusClient browser("focus-browser"), hidden("focus-private");
    hidden.setProp("preserve_previous_focus", true);
    browser.focus();
    hidden.focus();
    EXPECT(browser.stats().focused, false);
    hidden.setProp("hide_from_screen_share", true);
    browser.focus();
    hidden.focus();
    EXPECT(browser.stats().focused, true);
    hidden.setProp("hide_from_screen_share", false);
    EXPECT(browser.stats().focused || browser.stats().active, false);
}

TEST_CASE(privateFocusReleasedOnNullFocus) {
    CFocusClient browser("focus-browser"), hidden("focus-private");
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    browser.focus();
    hidden.focus();
    EXPECT(browser.stats().focused, true);
    OK(getFromSocket("/eval hl.plugin.test.nullfocus()"));
    EXPECT(browser.stats().focused || browser.stats().active, false);
    EXPECT(hidden.stats().focused, false);
    browser.focus();
    EXPECT(browser.stats().focused && browser.stats().active, true);
}

TEST_CASE(privateFocusSurvivesPrivateWindowChain) {
    CFocusClient browser("focus-browser"), first("focus-first"), second("focus-second");
    for (auto client : {std::ref(first), std::ref(second)}) {
        client.get().setProp("hide_from_screen_share", true);
        client.get().setProp("preserve_previous_focus", true);
    }
    browser.focus();
    const auto BEFORE = browser.stats();
    first.focus();
    second.focus();
    EXPECT(first.stats().focused || first.stats().active, false);
    EXPECT(browser.stats().focused && browser.stats().active, true);
    browser.focus();
    EXPECT(browser.stats().enters, BEFORE.enters);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
}

TEST_CASE(privateFocusSurvivesClosingPrivateWindow) {
    CFocusClient browser("focus-browser"), hidden("focus-private");
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    browser.focus();
    const auto BEFORE = browser.stats();
    hidden.focus();
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.close({{ window = '{}' }})", hidden.selector())));
    for (int i = 0; i < 50 && Tests::windowCount() > 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    browser.focus();
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
}

TEST_CASE(privateFocusSurvivesWorkspaceSwitch) {
    CFocusClient browser("focus-browser"), hidden("focus-private"), other("focus-other");
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    hidden.moveToWorkspace("2");
    other.moveToWorkspace("3");
    browser.focus();
    const auto BEFORE = browser.stats();
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '2' })"));
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(browser.suspended(), false);
    EXPECT(hidden.stats().focused, true);
    sendFocusTestKey(56, true);
    sendFocusTestKey(56, false);
    EXPECT(hidden.stats().presses, 1U);
    EXPECT(browser.stats().presses, BEFORE.presses);
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '1' })"));
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().enters, BEFORE.enters);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(browser.suspended(), false);
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '2' })"));
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '3' })"));
    EXPECT(browser.stats().focused || browser.stats().active, false);
    EXPECT(browser.suspended(), true);
    EXPECT(other.stats().focused && other.stats().active, true);
}

TEST_CASE(privateFocusReleasedOnInvisibleWorkspaceWhenDisabled) {
    CFocusClient browser("focus-browser"), hidden("focus-private");
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    hidden.moveToWorkspace("2");
    browser.focus();
    const auto BEFORE = browser.stats();
    hidden.focus();
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(browser.suspended(), false);
    hidden.setProp("preserve_previous_focus", false);
    EXPECT(browser.stats().focused || browser.stats().active, false);
    EXPECT(browser.stats().leaves, BEFORE.leaves + 1);
    EXPECT(browser.suspended(), true);
}

TEST_CASE(privateFocusReleasedOnEmptyWorkspace) {
    CFocusClient browser("focus-browser"), hidden("focus-private");
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    hidden.moveToWorkspace("2");
    browser.focus();
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '2' })"));
    EXPECT(browser.stats().focused, true);
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '3' })"));
    EXPECT(browser.stats().focused || browser.stats().active, false);
    EXPECT(browser.suspended(), true);
    EXPECT(hidden.stats().focused, false);
}

static void prepareGuardedFocus(CFocusClient& browser, CFocusClient& hidden) {
    hidden.setProp("hide_from_screen_share", true);
    hidden.setProp("preserve_previous_focus", true);
    hidden.floating();
    browser.focus();
    browser.setProp("focus_guard", true);
}

TEST_CASE(guardedPrivateFocusKeepsActiveWindow) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    prepareGuardedFocus(browser, hidden);
    const auto BEFORE = browser.stats();
    hidden.focus();
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(hidden.stats().focused, true);
    sendFocusTestKey(56, true);
    sendFocusTestKey(56, false);
    EXPECT(hidden.stats().presses, 1U);
    EXPECT(browser.stats().presses, BEFORE.presses);
    browser.focus();
    EXPECT(hidden.stats().focused, false);
    EXPECT(browser.stats().enters, BEFORE.enters);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    sendFocusTestKey(38, true);
    sendFocusTestKey(38, false);
    EXPECT(browser.stats().presses, BEFORE.presses + 1);
    browser.setProp("focus_guard", false);
}

TEST_CASE(guardedFocusReturnsInputOnBrowserClick) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    browser.focus();
    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'set' })"));
    hidden.floating();
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ window = '{}', x = 200, y = 200 }})", hidden.selector())));
    prepareGuardedFocus(browser, hidden);
    const auto BEFORE = browser.stats();
    hidden.focus();
    EXPECT(hidden.stats().focused, true);
    OK(getFromSocket("/dispatch hl.dsp.cursor.move({ x = 10, y = 10 })"));
    OK(getFromSocket("/eval hl.plugin.test.click(272, 1)"));
    OK(getFromSocket("/eval hl.plugin.test.click(272, 0)"));
    EXPECT(hidden.stats().focused, false);
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().enters, BEFORE.enters);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    sendFocusTestKey(38, true);
    sendFocusTestKey(38, false);
    EXPECT(browser.stats().presses, BEFORE.presses + 1);
    browser.setProp("focus_guard", false);
}

TEST_CASE(guardedFocusRejectsOrdinaryAndTiledWindows) {
    CFocusClient browser("guard-browser"), hidden("guard-private"), ordinary("guard-ordinary"), tiled("guard-tiled");
    tiled.setProp("hide_from_screen_share", true);
    tiled.setProp("preserve_previous_focus", true);
    prepareGuardedFocus(browser, hidden);
    const auto BEFORE = browser.stats();
    ordinary.focus();
    tiled.focus();
    OK(getFromSocket("/eval hl.plugin.test.nullfocus()"));
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(ordinary.stats().focused || tiled.stats().focused, false);
    hidden.focus();
    ordinary.focus();
    EXPECT(browser.activeWindow(), true);
    EXPECT(hidden.stats().focused, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    browser.setProp("focus_guard", false);
}

TEST_CASE(guardedFocusLocksWorkspace) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    prepareGuardedFocus(browser, hidden);
    const auto WORKSPACE = browser.attribute("workspace");
    const auto BEFORE    = browser.stats();
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '2' })"));
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = 'special:guard' })"));
    browser.moveToWorkspace("2");
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.attribute("workspace"), WORKSPACE);
    EXPECT(browser.suspended(), false);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    hidden.moveToWorkspace("2");
    hidden.focus();
    EXPECT(hidden.stats().focused, false);
    EXPECT(browser.activeWindow(), true);
    browser.setProp("focus_guard", false);
    OK(getFromSocket("/dispatch hl.dsp.focus({ workspace = '2' })"));
    EXPECT(hidden.stats().focused, true);
}

TEST_CASE(guardedFocusLocksFullscreen) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    browser.focus();
    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'set' })"));
    prepareGuardedFocus(browser, hidden);
    const auto FULLSCREEN = browser.attribute("fullscreen");
    const auto SIZE       = browser.attribute("size");
    EXPECT(FULLSCREEN == "0", false);
    hidden.focus();
    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'unset' })"));
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.fullscreen({{ mode = 'fullscreen', action = 'set', window = '{}' }})", hidden.selector())));
    browser.floating();
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.attribute("fullscreen"), FULLSCREEN);
    EXPECT(browser.attribute("size"), SIZE);
    EXPECT(browser.stats().focused && browser.stats().active, true);
    browser.setProp("focus_guard", false);
    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'fullscreen', action = 'unset' })"));
    EXPECT(browser.attribute("fullscreen"), std::string("0"));
}

TEST_CASE(guardedFocusLocksFloatingGeometry) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    browser.floating();
    prepareGuardedFocus(browser, hidden);
    const auto SIZE = browser.attribute("size");
    const auto POS  = browser.attribute("at");
    OK(getFromSocket("/dispatch hl.dsp.window.resize({ x = 400, y = 300 })"));
    OK(getFromSocket("/dispatch hl.dsp.window.move({ x = 100, y = 100 })"));
    EXPECT(browser.attribute("size"), SIZE);
    EXPECT(browser.attribute("at"), POS);
    browser.setProp("focus_guard", false);
    OK(getFromSocket("/dispatch hl.dsp.window.resize({ x = 400, y = 300 })"));
    EXPECT(browser.attribute("size"), std::string("400,300"));
}

TEST_CASE(guardedFocusRestoresInputWhenPrivateDisabled) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    prepareGuardedFocus(browser, hidden);
    const auto BEFORE = browser.stats();
    hidden.focus();
    hidden.setProp("hide_from_screen_share", false);
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(hidden.stats().focused, false);
    sendFocusTestKey(38, true);
    sendFocusTestKey(38, false);
    EXPECT(browser.stats().presses, BEFORE.presses + 1);
    browser.setProp("focus_guard", false);
}

TEST_CASE(guardedFocusDisablesWithoutLosingBrowserFocus) {
    CFocusClient browser("guard-browser"), hidden("guard-private"), other("guard-other");
    prepareGuardedFocus(browser, hidden);
    const auto BEFORE = browser.stats();
    hidden.focus();
    browser.setProp("focus_guard", false);
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().enters, BEFORE.enters);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(hidden.stats().focused, false);
    other.focus();
    EXPECT(other.activeWindow(), true);
    EXPECT(browser.stats().focused || browser.stats().active, false);
}

TEST_CASE(guardedFocusSurvivesClosingPrivateWindow) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    prepareGuardedFocus(browser, hidden);
    const auto BEFORE = browser.stats();
    hidden.focus();
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.close({{ window = '{}' }})", hidden.selector())));
    Tests::waitUntilWindowsN(1);
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    sendFocusTestKey(38, true);
    sendFocusTestKey(38, false);
    EXPECT(browser.stats().presses, BEFORE.presses + 1);
    browser.setProp("focus_guard", false);
}

TEST_CASE(guardedFocusReleasesAfterBrowserCloses) {
    CFocusClient browser("guard-browser"), hidden("guard-private"), other("guard-other");
    prepareGuardedFocus(browser, hidden);
    hidden.focus();
    OK(getFromSocket(std::format("/dispatch hl.dsp.window.close({{ window = '{}' }})", browser.selector())));
    Tests::waitUntilWindowsN(2);
    other.focus();
    EXPECT(other.activeWindow(), true);
    EXPECT(other.stats().focused && other.stats().active, true);
}

TEST_CASE(guardedFocusSurvivesUnmappingPrivateWindow) {
    CFocusClient browser("guard-browser"), hidden("guard-private");
    prepareGuardedFocus(browser, hidden);
    const auto BEFORE = browser.stats();
    hidden.focus();
    hidden.unmap();
    EXPECT(browser.activeWindow(), true);
    EXPECT(browser.stats().focused && browser.stats().active, true);
    EXPECT(browser.stats().leaves, BEFORE.leaves);
    EXPECT(hidden.stats().focused, false);
    sendFocusTestKey(38, true);
    sendFocusTestKey(38, false);
    EXPECT(browser.stats().presses, BEFORE.presses + 1);
    browser.setProp("focus_guard", false);
}

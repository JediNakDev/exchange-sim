#include "server/admin.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <system_error>
#include <vector>

namespace exsim::server {

namespace {

std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
        const std::size_t start = i;
        while (i < s.size() && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

template <typename T>
bool to_uint(std::string_view v, T& out) noexcept {
    std::uint64_t x = 0;
    const auto r = std::from_chars(v.data(), v.data() + v.size(), x);
    if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || x > std::numeric_limits<T>::max()) return false;
    out = static_cast<T>(x);
    return true;
}

bool to_phase(std::string_view v, Phase& p) noexcept {
    if (iequals(v, "closed")) p = Phase::Closed;
    else if (iequals(v, "pre_open")) p = Phase::PreOpen;
    else if (iequals(v, "continuous")) p = Phase::Continuous;
    else if (iequals(v, "halted")) p = Phase::Halted;
    else if (iequals(v, "pre_close")) p = Phase::PreClose;
    else return false;
    return true;
}

}  // namespace

std::string_view admin_help() noexcept {
    return "help\n"
           "status\n"
           "phase <SYMBOL|*> <closed|pre_open|continuous|halted|pre_close>\n"
           "book <SYMBOL> [levels]\n"
           "fault md drop <n>\n"
           "fault md dup <n>\n"
           "fault md reorder <n> [delay]\n"
           "fault md rate [drop=<ppm>] [dup=<ppm>] [reorder=<ppm>] [delay=<n>] [seed=<s>]\n"
           "fault md off\n"
           "fault user <USER> reject <n>\n"
           "fault user <USER> delay <us>\n"
           "fault user <USER> drop_out <n>\n"
           "fault user <USER> disconnect\n"
           "fault user <USER> off\n"
           "bots <on|off>\n"
           "shutdown\n";
}

bool parse_admin(std::string_view line, AdminCommand& out, std::string& error) {
    const auto t = split(line);
    AdminCommand c{};
    auto fail = [&](std::string msg) {
        error = std::move(msg);
        return false;
    };
    auto expect_args = [&](std::size_t n) { return t.size() == n; };
    if (t.empty()) return fail("empty command");
    const std::string_view verb = t[0];

    if (iequals(verb, "help")) {
        if (!expect_args(1)) return fail("usage: help");
        c.op = AdminOp::Help;
    } else if (iequals(verb, "status")) {
        if (!expect_args(1)) return fail("usage: status");
        c.op = AdminOp::Status;
    } else if (iequals(verb, "shutdown")) {
        if (!expect_args(1)) return fail("usage: shutdown");
        c.op = AdminOp::Shutdown;
    } else if (iequals(verb, "phase")) {
        if (!expect_args(3) || !to_phase(t[2], c.phase)) {
            return fail("usage: phase <SYMBOL|*> <closed|pre_open|continuous|halted|pre_close>");
        }
        c.op = AdminOp::Phase;
        c.symbol = std::string(t[1]);
    } else if (iequals(verb, "book")) {
        c.op = AdminOp::Book;
        c.count = 5;
        if ((t.size() != 2 && t.size() != 3) || (t.size() == 3 && (!to_uint(t[2], c.count) || c.count == 0))) {
            return fail("usage: book <SYMBOL> [levels]");
        }
        c.symbol = std::string(t[1]);
    } else if (iequals(verb, "bots")) {
        if (!expect_args(2) || !(iequals(t[1], "on") || iequals(t[1], "off"))) return fail("usage: bots <on|off>");
        c.op = AdminOp::Bots;
        c.on = iequals(t[1], "on");
    } else if (iequals(verb, "fault")) {
        if (t.size() < 3) return fail("usage: fault md ... | fault user <USER> ...");
        if (iequals(t[1], "md")) {
            const std::string_view what = t[2];
            if (iequals(what, "off")) {
                if (!expect_args(3)) return fail("usage: fault md off");
                c.op = AdminOp::FaultMdOff;
            } else if (iequals(what, "drop") || iequals(what, "dup")) {
                if (!expect_args(4) || !to_uint(t[3], c.count) || c.count == 0) {
                    return fail("usage: fault md " + std::string(what) + " <n>");
                }
                c.op = iequals(what, "drop") ? AdminOp::FaultMdDrop : AdminOp::FaultMdDup;
            } else if (iequals(what, "reorder")) {
                c.delay = 1;
                if ((t.size() != 4 && t.size() != 5) || !to_uint(t[3], c.count) || c.count == 0 ||
                    (t.size() == 5 && (!to_uint(t[4], c.delay) || c.delay == 0))) {
                    return fail("usage: fault md reorder <n> [delay]");
                }
                c.op = AdminOp::FaultMdReorder;
            } else if (iequals(what, "rate")) {
                c.op = AdminOp::FaultMdRate;
                std::array<bool, 5> supplied_keys{};
                for (std::size_t i = 3; i < t.size(); ++i) {
                    const std::size_t eq = t[i].find('=');
                    if (eq == std::string_view::npos) return fail("expected key=value, got '" + std::string(t[i]) + "'");
                    const std::string_view k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
                    std::size_t key_index = 0;
                    if (iequals(k, "drop")) key_index = 0;
                    else if (iequals(k, "dup")) key_index = 1;
                    else if (iequals(k, "reorder")) key_index = 2;
                    else if (iequals(k, "delay")) key_index = 3;
                    else if (iequals(k, "seed")) key_index = 4;
                    else return fail("unknown rate key '" + std::string(k) + "'");
                    if (supplied_keys[key_index]) return fail("duplicate rate key '" + std::string(k) + "'");
                    supplied_keys[key_index] = true;
                    bool ok = false;
                    if (iequals(k, "drop")) ok = to_uint(v, c.rate.drop_ppm) && c.rate.drop_ppm <= 1'000'000;
                    else if (iequals(k, "dup")) ok = to_uint(v, c.rate.duplicate_ppm) && c.rate.duplicate_ppm <= 1'000'000;
                    else if (iequals(k, "reorder")) ok = to_uint(v, c.rate.reorder_ppm) && c.rate.reorder_ppm <= 1'000'000;
                    else if (iequals(k, "delay")) ok = to_uint(v, c.rate.reorder_delay) && c.rate.reorder_delay > 0;
                    else if (iequals(k, "seed")) ok = to_uint(v, c.rate.seed);
                    else return fail("unknown rate key '" + std::string(k) + "'");
                    if (!ok) return fail("invalid value for '" + std::string(k) + "' (ppm must be 0..1000000)");
                }
            } else {
                return fail("unknown md fault '" + std::string(what) + "'");
            }
        } else if (iequals(t[1], "user")) {
            if (t.size() < 4) return fail("usage: fault user <USER> <reject|delay|drop_out|disconnect|off> [n]");
            c.user = std::string(t[2]);
            const std::string_view what = t[3];
            if (iequals(what, "disconnect") || iequals(what, "off")) {
                if (!expect_args(4)) return fail("usage: fault user <USER> " + std::string(what));
                c.op = iequals(what, "off") ? AdminOp::FaultUserOff : AdminOp::FaultUserDisconnect;
            } else if (iequals(what, "reject") || iequals(what, "drop_out")) {
                if (!expect_args(5) || !to_uint(t[4], c.count) || c.count == 0) {
                    return fail("usage: fault user <USER> " + std::string(what) + " <n>");
                }
                c.op = iequals(what, "reject") ? AdminOp::FaultUserReject : AdminOp::FaultUserDropOut;
            } else if (iequals(what, "delay")) {
                if (!expect_args(5) || !to_uint(t[4], c.delay)) return fail("usage: fault user <USER> delay <us>");
                c.op = AdminOp::FaultUserDelay;
            } else {
                return fail("unknown user fault '" + std::string(what) + "'");
            }
        } else {
            return fail("usage: fault md ... | fault user <USER> ...");
        }
    } else {
        return fail("unknown command '" + std::string(verb) + "' (try: help)");
    }
    out = std::move(c);
    error.clear();
    return true;
}

}  // namespace exsim::server

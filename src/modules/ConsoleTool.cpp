#include "modules/ConsoleTool.h"
#include "core/XbdmClient.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace x360::modules {

namespace {

std::string g_bindIp;
std::string joinFrom(const std::vector<std::string>& a, size_t start) {
    std::string s;
    for (size_t i = start; i < a.size(); ++i) {
        if (!s.empty()) s.push_back(' ');
        s += a[i];
    }
    return s;
}

void printLines(const std::vector<std::string>& lines) {
    for (const auto& l : lines) std::cout << l << '\n';
}

} // namespace

void setConsoleBindIp(const std::string& ip) { g_bindIp = ip; }

void ConsoleTool::printHelp() const {
    std::cout << "  console probe <ip>                     connect and show XBDM banner\n"
                 "  console info <ip>                      SYSTEMINFO\n"
                 "  console drives <ip>                    DRIVELIST\n"
                 "  console dir <ip> <path>                DIRLIST\n"
                 "  console get <ip> <remote> <local>      download a file via GETFILE\n"
                 "  console cmd <ip> <raw command...>      send any XBDM command\n"
                 "  console reboot <ip>                    REBOOT (actually reboots!)\n";
}

int ConsoleTool::run(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        printHelp();
        return 0;
    }
    const std::string& sub = ctx.args[0];
    if (ctx.args.size() < 2) {
        std::cerr << "usage: x360tool console <command> <ip> ...\n";
        printHelp();
        return 1;
    }
    const std::string& ip = ctx.args[1];

    XbdmClient x;
    std::string error;
    if (!x.connect(ip, 730, error, g_bindIp)) {
        std::cerr << "connect: " << error << '\n';
        return 1;
    }

    if (sub == "probe") {
        printLines(x.banner(error));
        std::cout << "connected to " << ip << ":730\n";
        return 0;
    }
    if (sub == "info") {
        printLines(x.command("SYSTEMINFO", error));
    } else if (sub == "drives") {
        printLines(x.command("DRIVELIST", error));
    } else if (sub == "dir") {
        if (ctx.args.size() < 3) {
            std::cerr << "usage: x360tool console dir <ip> <path>\n";
            return 1;
        }
        std::string path = joinFrom(ctx.args, 2);
        if (!path.empty() && path.back() != '\\') path.push_back('\\');
        printLines(x.command("DIRLIST NAME=\"" + path + "\"", error));
    } else if (sub == "get") {
        if (ctx.args.size() < 4) {
            std::cerr << "usage: x360tool console get <ip> <remote> <local>\n";
            return 1;
        }
        uint64_t bytes = 0;
        if (!x.download(ctx.args[2], ctx.args[3], error, &bytes)) {
            std::cerr << "download failed: " << error << '\n';
            return 1;
        }
        std::cout << "downloaded " << bytes << " bytes: " << ctx.args[2]
                  << " -> " << ctx.args[3] << '\n';
        return 0;
    } else if (sub == "cmd") {
        if (ctx.args.size() < 3) {
            std::cerr << "usage: x360tool console cmd <ip> <raw command...>\n";
            return 1;
        }
        printLines(x.command(joinFrom(ctx.args, 2), error));
    } else if (sub == "reboot") {
        std::cerr << "warning: rebooting console at " << ip << '\n';
        printLines(x.command("REBOOT", error));
    } else {
        std::cerr << "unknown console command: " << sub << '\n';
        printHelp();
        return 1;
    }

    if (!error.empty()) {
        std::cerr << "command error: " << error << '\n';
        return 1;
    }
    return 0;
}

} // namespace x360::modules

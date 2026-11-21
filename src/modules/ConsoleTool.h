#pragma once

#include "core/Tool.h"

namespace x360::modules {

void setConsoleBindIp(const std::string& ip);

class ConsoleTool final : public ITool {
public:
    std::string_view name() const override { return "console"; }
    std::string_view description() const override { return "XBDM connection to the console"; }
    void printHelp() const override;
    int run(const CommandContext& ctx) override;
};

} // namespace x360::modules

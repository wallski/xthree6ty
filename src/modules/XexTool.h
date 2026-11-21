#pragma once

#include "core/Tool.h"

namespace x360::modules {

class XexTool final : public ITool {
public:
    std::string_view name() const override { return "xex"; }
    std::string_view description() const override { return "XEX2 executable header inspector"; }
    void printHelp() const override;
    int run(const CommandContext& ctx) override;
};

} // namespace x360::modules

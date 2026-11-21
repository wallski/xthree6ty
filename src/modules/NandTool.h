#pragma once

#include "core/Tool.h"

namespace x360::modules {

class NandTool final : public ITool {
public:
    std::string_view name() const override { return "nand"; }
    std::string_view description() const override { return "read-only NAND dump inspector"; }
    void printHelp() const override;
    int run(const CommandContext& ctx) override;
};

} // namespace x360::modules

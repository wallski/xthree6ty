#pragma once

#include "core/Tool.h"

namespace x360::modules {

class XuizTool final : public ITool {
public:
    std::string_view name() const override { return "xuiz"; }
    std::string_view description() const override { return "XUIZ / XZP skin package inspector"; }
    void printHelp() const override;
    int run(const CommandContext& ctx) override;
};

} // namespace x360::modules

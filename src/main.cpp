#include "core/Tool.h"
#include "modules/ConsoleTool.h"
#include "modules/NandTool.h"
#include "modules/XexTool.h"
#include "modules/XuizTool.h"
#include "ui/GuiApp.h"

int main(int, char**) {
    auto& registry = x360::Registry::instance();
    registry.add(std::make_unique<x360::modules::XuizTool>());
    registry.add(std::make_unique<x360::modules::XexTool>());
    registry.add(std::make_unique<x360::modules::NandTool>());
    registry.add(std::make_unique<x360::modules::ConsoleTool>());

    return x360::ui::runGui();
}

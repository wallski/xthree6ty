#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x360 {

struct CommandContext {
    std::vector<std::string> args;
};

class ITool {
public:
    virtual ~ITool() = default;
    virtual std::string_view name() const = 0;
    virtual std::string_view description() const = 0;
    virtual void printHelp() const = 0;
    virtual int run(const CommandContext& ctx) = 0;
};

class Registry {
public:
    static Registry& instance() {
        static Registry r;
        return r;
    }

    void add(std::unique_ptr<ITool> tool) {
        tools_.push_back(std::move(tool));
    }

    ITool* find(std::string_view name) const {
        for (const auto& t : tools_) {
            if (t->name() == name) return t.get();
        }
        return nullptr;
    }

    const std::vector<std::unique_ptr<ITool>>& all() const { return tools_; }

private:
    std::vector<std::unique_ptr<ITool>> tools_;
};

} // namespace x360

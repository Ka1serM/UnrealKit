#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "UnrealKit/Export/SceneExport.h"

namespace ue5parse::runtime {
class UWorld;
}
namespace ue = ue5parse::runtime;

namespace UnrealKit::Export {

struct MapActor {
    std::string category;
    std::vector<ExportMesh> parts;
    float progress = 0;
};

struct MapRules {
    std::vector<std::string> hiddenClasses;
    std::function<std::expected<std::vector<std::string>, Core::Error>(const ue::UWorld& world, std::stop_token stop)> sublevels;
};

class MapReader {
public:
    explicit MapReader(MapRules rules);
    ~MapReader();
    MapReader(MapReader&&) noexcept;
    MapReader& operator=(MapReader&&) noexcept;

    void open(std::string_view path);
    std::expected<std::optional<MapActor>, Core::Error> next(std::stop_token stop);

private:
    struct State;

    std::unique_ptr<State> state;
};

std::string worldPath(std::string_view path);

}

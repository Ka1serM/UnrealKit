#include "UnrealKit/Export/MapExport.h"

#include <string>
#include <thread>
#include <utility>

#include "UnrealKit/Core/WorkQueue.h"

namespace UnrealKit::Export {

std::expected<ExportResult, Core::Error> sendMap(const Assets::Provider& provider, Transport::RecordSink& sink, MapReader& source, const std::string_view exportId,
                                                 const std::string_view title, std::stop_token stop, const ExportProgress& progress) {
    SceneSender sender(provider, {}, sink, stop, {});
    if (auto opened = sender.open(exportId, title); !opened)
        return std::unexpected(opened.error());
    Core::WorkQueue<MapActor> queue;
    std::jthread producer([&](std::stop_token producerStop) {
        while (!producerStop.stop_requested() && !stop.stop_requested()) {
            auto actor = source.next(stop);
            if (!actor)
                return queue.finish(std::unexpected(actor.error()));
            if (!*actor)
                break;
            if ((*actor)->category.empty())
                return queue.finish(Core::fail("map.category", "The map has an actor without a category."));
            queue.push(std::move(**actor));
        }
        queue.finish();
    });
    float shown = 0;
    while (true) {
        auto actor = queue.pop(stop);
        if (!actor)
            return std::unexpected(actor.error());
        if (!*actor)
            break;
        const auto collection = sender.collection((*actor)->category, 0);
        if (!collection)
            return std::unexpected(collection.error());
        if (auto sent = sender.send({.meshes = std::move((*actor)->parts), .images = {}, .collection = *collection}); !sent)
            return std::unexpected(sent.error());
        if (!progress || (*actor)->progress < shown + 0.005f)
            continue;
        shown = (*actor)->progress;
        progress(ExportStage::Sending, shown);
    }
    auto diagnostics = sender.complete({});
    if (!diagnostics)
        return std::unexpected(diagnostics.error());
    if (progress)
        progress(ExportStage::Sending, 1);
    return ExportResult{.title = std::string(title), .diagnostics = std::move(*diagnostics)};
}

}

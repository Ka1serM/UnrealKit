#pragma once

#include <stop_token>
#include <string_view>

#include "UnrealKit/Assets/Provider.h"
#include "UnrealKit/Export/MapReader.h"
#include "UnrealKit/Export/SceneExport.h"
#include "UnrealKit/Transport/RecordSink.h"

namespace UnrealKit::Export {

std::expected<ExportResult, Core::Error> sendMap(const Assets::Provider& provider, Transport::RecordSink& sink, MapReader& source, std::string_view exportId,
                                                 std::string_view title, std::stop_token stop, const ExportProgress& progress);

}

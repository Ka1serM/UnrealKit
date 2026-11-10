#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "CppUE5Parse/Provider/FileProvider.h"

namespace UnrealKit::Assets {

//! Maps an archive path such as `Game/Content/Maps/A.umap` to its object path `/Game/Maps/A.A`. Empty when the file is in no content folder.
std::string objectPathOf(std::string_view filePath);

//! The object paths of every `.umap` in the mounted archives, sorted.
std::vector<std::string> listMaps(const ue5parse::FileProvider& files);

}

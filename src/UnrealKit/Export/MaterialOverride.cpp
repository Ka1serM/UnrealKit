#include "UnrealKit/Export/MaterialOverride.h"

#include "UnrealKit/Assets/Reading.h"
#include "Engine/Source/Runtime/Engine/Materials/MaterialInterface.h"

namespace UnrealKit::Export {

MaterialOverride overrideOf(const ue::UMaterialInterface* material) {
    return {.path = Assets::Reading::pathOf(material), .material = material};
}

}

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Source/Runtime/CoreUObject/UObject/Class.h"
#include "Engine/Source/Runtime/CoreUObject/UObject/UnrealType.h"

namespace UnrealKit::Assets {

class FunctionParameters {
public:
    explicit FunctionParameters(const ue5parse::runtime::UFunction& function)
        : function(function), memory((static_cast<std::size_t>(function.ParmsSize) + sizeof(std::max_align_t)) / sizeof(std::max_align_t)) {
        for (ue5parse::runtime::FField* field = function.ChildProperties; field != nullptr; field = field->Next)
            if (const auto* property = parameter(field))
                property->InitializeValue_InContainer(get());
    }
    ~FunctionParameters() {
        for (ue5parse::runtime::FField* field = function.ChildProperties; field != nullptr; field = field->Next)
            if (const auto* property = parameter(field))
                property->DestroyValue_InContainer(get());
    }
    FunctionParameters(const FunctionParameters&) = delete;
    FunctionParameters& operator=(const FunctionParameters&) = delete;

    std::uint8_t* get() { return reinterpret_cast<std::uint8_t*>(memory.data()); }

private:
    static const ue5parse::runtime::FProperty* parameter(ue5parse::runtime::FField* field) {
        const auto* property = ue5parse::runtime::CastField<ue5parse::runtime::FProperty>(field);
        return property != nullptr && property->HasAnyPropertyFlags(ue5parse::runtime::CPF_Parm) ? property : nullptr;
    }

    const ue5parse::runtime::UFunction& function;
    std::vector<std::max_align_t> memory;
};

}

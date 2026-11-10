#pragma once

#include <array>

#include "Engine/Source/Runtime/Core/Math/Transform.h"

namespace ue = ue5parse::runtime;

namespace UnrealKit::Scene {

struct Vector3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

class Matrix {
public:
    static Matrix identity();
    static Matrix translation(Vector3 offset);

    Matrix operator*(const Matrix& right) const;
    Vector3 translation() const;

    const std::array<float, 16>& rows() const { return values; }

    float& at(int row, int column) { return values[row * 4 + column]; }
    float at(int row, int column) const { return values[row * 4 + column]; }

private:
    std::array<float, 16> values{};
};

Matrix matrixOf(const ue::FTransform& transform);
Matrix matrixOf(const ue::FVector& location, const ue::FRotator& rotation, const ue::FVector& scale);
ue::FTransform transformOf(const Matrix& matrix);

}

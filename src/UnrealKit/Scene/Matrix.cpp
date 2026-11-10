#include "UnrealKit/Scene/Matrix.h"

#include <cmath>
#include <numbers>

namespace UnrealKit::Scene {

namespace {

Matrix rotation(float x, float y, float z, float w) {
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    x /= length;
    y /= length;
    z /= length;
    w /= length;
    Matrix result = Matrix::identity();
    result.at(0, 0) = 1 - 2 * (y * y + z * z);
    result.at(0, 1) = 2 * (x * y - z * w);
    result.at(0, 2) = 2 * (x * z + y * w);
    result.at(1, 0) = 2 * (x * y + z * w);
    result.at(1, 1) = 1 - 2 * (x * x + z * z);
    result.at(1, 2) = 2 * (y * z - x * w);
    result.at(2, 0) = 2 * (x * z - y * w);
    result.at(2, 1) = 2 * (y * z + x * w);
    result.at(2, 2) = 1 - 2 * (x * x + y * y);
    return result;
}

}

Matrix Matrix::identity() {
    Matrix result;
    for (int i = 0; i < 4; ++i)
        result.at(i, i) = 1;
    return result;
}

Matrix Matrix::translation(const Vector3 offset) {
    Matrix result = identity();
    result.at(0, 3) = offset.x;
    result.at(1, 3) = offset.y;
    result.at(2, 3) = offset.z;
    return result;
}

Matrix Matrix::operator*(const Matrix& right) const {
    Matrix result;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += at(row, k) * right.at(k, column);
            result.at(row, column) = sum;
        }
    return result;
}

Vector3 Matrix::translation() const {
    return {at(0, 3), at(1, 3), at(2, 3)};
}

Matrix matrixOf(const ue::FTransform& transform) {
    Matrix scale = Matrix::identity();
    scale.at(0, 0) = static_cast<float>(transform.GetScale3D().X);
    scale.at(1, 1) = static_cast<float>(transform.GetScale3D().Y);
    scale.at(2, 2) = static_cast<float>(transform.GetScale3D().Z);
    const ue::FVector& t = transform.GetTranslation();
    const ue::FQuat& q = transform.GetRotation();
    return Matrix::translation({static_cast<float>(t.X), static_cast<float>(t.Y), static_cast<float>(t.Z)})
           * rotation(static_cast<float>(q.X), static_cast<float>(q.Y), static_cast<float>(q.Z), static_cast<float>(q.W)) * scale;
}

Matrix matrixOf(const ue::FVector& location, const ue::FRotator& rotator, const ue::FVector& scale) {
    constexpr float halfRadians = std::numbers::pi_v<float> / 360.0f;
    const float pitch = static_cast<float>(rotator.Pitch), yaw = static_cast<float>(rotator.Yaw), roll = static_cast<float>(rotator.Roll);
    const float sp = std::sin(pitch * halfRadians), cp = std::cos(pitch * halfRadians);
    const float sy = std::sin(yaw * halfRadians), cy = std::cos(yaw * halfRadians);
    const float sr = std::sin(roll * halfRadians), cr = std::cos(roll * halfRadians);
    const ue::FQuat quaternion(cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy);
    const ue::FVector floatLocation{ue::FVector3f(location)}, floatScale{ue::FVector3f(scale)};
    return matrixOf(ue::FTransform(quaternion, floatLocation, floatScale));
}

ue::FTransform transformOf(const Matrix& matrix) {
    ue::FMatrix unreal;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            unreal.M[row][column] = matrix.at(column, row);
    return ue::FTransform(unreal);
}

}

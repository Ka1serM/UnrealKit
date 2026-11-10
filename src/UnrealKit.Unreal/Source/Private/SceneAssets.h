#pragma once

#include "CoreMinimal.h"
#include "ReferenceSkeleton.h"
#include "SceneStream.h"

class UMaterialInstanceConstant;
class UMaterialInterface;
class USkeletalMesh;
class UStaticMesh;
class UTexture2D;

namespace UnrealKit
{
FString PackageOf(const FString& Key);

UObject* FindAsset(const FString& PackageName, const UClass* Class);
UObject* CreateAsset(const FString& PackageName, UClass* Class);

template <class T>
T* FindAsset(const FString& PackageName)
{
	return static_cast<T*>(FindAsset(PackageName, T::StaticClass()));
}

template <class T>
T* CreateAsset(const FString& PackageName)
{
	return static_cast<T*>(CreateAsset(PackageName, T::StaticClass()));
}

UTexture2D* CreateTexture(const FString& PackageName, const scene::stream::Image& Record, FString& Error);

FReferenceSkeleton ReferenceSkeletonOf(const scene::stream::Skeleton& Record);

UStaticMesh* CreateStaticMesh(const FString& PackageName, const scene::stream::Mesh& Record, FString& Error);
USkeletalMesh* CreateSkeletalMesh(const FString& PackageName, const scene::stream::Mesh& Record, const FReferenceSkeleton& Reference, FString& Error);

FString ToString(const flatbuffers::String* Text);
FTransform ToTransform(const flatbuffers::Vector<float>& Location, const flatbuffers::Vector<float>& Rotation, const flatbuffers::Vector<float>& Scale);
}

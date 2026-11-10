#pragma once

#include "CoreMinimal.h"
#include "ReferenceSkeleton.h"
#include "SceneSequence.h"
#include "SceneStream.h"
#include "UObject/GCObject.h"

class AActor;
class UMaterialInterface;
class UStaticMesh;
class USkeletalMesh;
class UTexture2D;

struct FSceneDiagnostic
{
	scene::stream::Severity Severity = scene::stream::Severity::Error;
	FString Code;
	FString Message;
	FString Subject;
};

struct FSceneLight
{
	scene::stream::LightType Type = scene::stream::LightType::Point;
	FColor Color;
	float Intensity = 0.0f;
	float SourceRadius = 0.0f;
	float InnerConeAngle = 0.0f;
	float OuterConeAngle = 0.0f;
	float SourceWidth = 0.0f;
	float SourceHeight = 0.0f;
};

struct FSceneCamera
{
	float FieldOfView = 90.0f;
	float NearClip = 0.0f;
};

class FSceneImport final : public FGCObject
{
public:
	bool Accept(const scene::stream::Envelope& Envelope, FString& Error);
	bool IsComplete() const { return bComplete; }
	TArray<FSceneDiagnostic> Finish();

	void AddReferencedObjects(FReferenceCollector& Collector) override;
	FString GetReferencerName() const override { return TEXT("FSceneImport"); }

private:
	bool Build(const scene::stream::Envelope& Envelope, FString& Error);
	bool BuildImage(const scene::stream::Image& Record, FString& Error);
	bool BuildMaterial(const scene::stream::Material& Record, FString& Error);
	bool BuildSkeleton(const scene::stream::Skeleton& Record, FString& Error);
	bool BuildMesh(const scene::stream::Mesh& Record, FString& Error);
	void BuildScene(const scene::stream::Scene& Record);
	bool BuildCollection(const scene::stream::Collection& Record, FString& Error);
	bool BuildObject(const scene::stream::Object& Record, FString& Error);
	AActor* SpawnObject(const scene::stream::Object& Record, FString& Error);

	bool bBegun = false;
	bool bComplete = false;
	bool bNearClipReported = false;
	TArray<FSceneDiagnostic> AppDiagnostics;
	TArray<FSceneDiagnostic> Diagnostics;

	TMap<FString, TObjectPtr<UTexture2D>> Textures;
	TMap<FString, TObjectPtr<UMaterialInterface>> Materials;
	TMap<FString, FReferenceSkeleton> Skeletons;
	TMap<FString, TObjectPtr<UStaticMesh>> StaticMeshes;
	TMap<FString, TObjectPtr<USkeletalMesh>> SkeletalMeshes;
	TMap<FString, FSceneLight> Lights;
	TMap<FString, FSceneCamera> Cameras;
	TMap<uint32, FName> Folders;
	TMap<uint32, TObjectPtr<AActor>> Actors;
	FSceneSequences Sequences;
};

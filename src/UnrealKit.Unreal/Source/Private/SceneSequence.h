#pragma once

#include "CoreMinimal.h"
#include "SceneStream.h"
#include "UObject/ObjectPtr.h"
#include "UObject/UObjectGlobals.h"

class AActor;
class UCameraComponent;
class ULevelSequence;
class USkeletalMesh;

class FSceneSequences
{
public:
	void Begin(const FString& ExportId);
	void SetScene(const scene::stream::Scene& Record);
	TArray<FString> AddAnimation(const scene::stream::Animation& Record);
	bool Animate(const FString& AnimationKey, const FString& TrackName, AActor& Actor, USkeletalMesh* Mesh, UCameraComponent* Camera, FString& Error);

	void AddReferencedObjects(FReferenceCollector& Collector);

private:
	struct FKeys
	{
		int32 Components = 0;
		TArray<float> Frames;
		TArray<float> Values;
	};

	struct FBoneTrack
	{
		FName Bone;
		FKeys Location;
		FKeys Rotation;
		FKeys Scale;
	};

	struct FTrack
	{
		FKeys Location;
		FKeys Rotation;
		FKeys Scale;
		FKeys Visibility;
		FKeys FieldOfView;
		TArray<FBoneTrack> Bones;
	};

	struct FAnimation
	{
		FString Name;
		TMap<FString, FTrack> Tracks;
	};

	static FKeys* KeysFor(FTrack& Track, const FString& Property);
	static FKeys KeysOf(const scene::stream::Keys* Record);
	ULevelSequence* FindOrCreateSequence(FString& Error);
	FFrameNumber Tick(float Frame) const;
	bool AnimateBones(const FString& AnimationKey, const FString& TrackName, const FTrack& Track, USkeletalMesh& Mesh, const FGuid& Binding,
	                  FString& Error) const;

	uint32 FramesPerSecond = 30;
	int32 FrameStart = 1;
	int32 FrameEnd = 1;
	FString SequenceKey;
	TMap<FString, FAnimation> Animations;
	TObjectPtr<ULevelSequence> Sequence;
};

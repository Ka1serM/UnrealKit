#include "SceneSequence.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "SceneAssets.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneVisibilitySection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"

namespace
{
namespace stream = scene::stream;

constexpr int32 TicksPerFrame = 1000;

template <class T, class TRead, class TBlend>
T Sample(const TArray<float>& Frames, const float Frame, int32& Cursor, TRead Read, TBlend Blend)
{
	while (Cursor + 1 < Frames.Num() && Frames[Cursor + 1] <= Frame)
		++Cursor;
	if (Cursor + 1 >= Frames.Num() || Frame <= Frames[Cursor])
		return Read(Cursor);
	const float Alpha = (Frame - Frames[Cursor]) / (Frames[Cursor + 1] - Frames[Cursor]);
	return Blend(Read(Cursor), Read(Cursor + 1), Alpha);
}

FVector3f VectorAt(const TArray<float>& Values, const int32 Key)
{
	return FVector3f(Values[Key * 3], Values[Key * 3 + 1], Values[Key * 3 + 2]);
}

FQuat4f QuatAt(const TArray<float>& Values, const int32 Key)
{
	return FQuat4f(Values[Key * 4], Values[Key * 4 + 1], Values[Key * 4 + 2], Values[Key * 4 + 3]);
}

FGuid FindBinding(UMovieScene& MovieScene, const FString& Name)
{
	for (int32 Index = 0; Index < MovieScene.GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = MovieScene.GetPossessable(Index);
		if (!Possessable.GetParent().IsValid() && Possessable.GetName() == Name)
			return Possessable.GetGuid();
	}
	return {};
}

FGuid FindChildBinding(UMovieScene& MovieScene, const FGuid& Parent)
{
	for (int32 Index = 0; Index < MovieScene.GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = MovieScene.GetPossessable(Index);
		if (Possessable.GetParent() == Parent)
			return Possessable.GetGuid();
	}
	return {};
}

template <class TSection, class TTrack>
TSection& AddSection(UMovieScene& MovieScene, const FGuid& Binding)
{
	TTrack* Track = MovieScene.AddTrack<TTrack>(Binding);
	TSection* Section = CastChecked<TSection>(Track->CreateNewSection());
	Section->SetRange(TRange<FFrameNumber>::All());
	Track->AddSection(*Section);
	return *Section;
}
}

FSceneSequences::FKeys FSceneSequences::KeysOf(const stream::Keys* Record)
{
	FKeys Keys;
	if (!Record || Record->frames()->size() == 0 || Record->values()->size() != Record->frames()->size() * Record->components())
		return Keys;
	Keys.Components = Record->components();
	Keys.Frames = TArray<float>(Record->frames()->data(), static_cast<int32>(Record->frames()->size()));
	Keys.Values = TArray<float>(Record->values()->data(), static_cast<int32>(Record->values()->size()));
	return Keys;
}

void FSceneSequences::Begin(const FString& ExportId)
{
	SequenceKey = ExportId;
}

void FSceneSequences::SetScene(const stream::Scene& Record)
{
	FramesPerSecond = FMath::Max(Record.frames_per_second(), 1u);
	FrameStart = Record.frame_start();
	FrameEnd = FMath::Max(Record.frame_end(), Record.frame_start());
}

FSceneSequences::FKeys* FSceneSequences::KeysFor(FTrack& Track, const FString& Property)
{
	if (Property == TEXT("location"))
		return &Track.Location;
	if (Property == TEXT("rotation"))
		return &Track.Rotation;
	if (Property == TEXT("scale"))
		return &Track.Scale;
	if (Property == TEXT("visibility"))
		return &Track.Visibility;
	if (Property == TEXT("field_of_view"))
		return &Track.FieldOfView;
	FString Bone, Attribute;
	if (!Property.StartsWith(TEXT("bones/")) || !Property.RightChop(6).Split(TEXT("/"), &Bone, &Attribute, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		return nullptr;
	FKeys FBoneTrack::* Member = Attribute == TEXT("location")   ? &FBoneTrack::Location
	                             : Attribute == TEXT("rotation") ? &FBoneTrack::Rotation
	                             : Attribute == TEXT("scale")    ? &FBoneTrack::Scale
	                                                             : nullptr;
	if (!Member)
		return nullptr;
	FBoneTrack* Found = Track.Bones.FindByPredicate([&Bone](const FBoneTrack& Each) { return Each.Bone == FName(Bone); });
	if (!Found)
		Found = &Track.Bones.Add_GetRef({FName(Bone)});
	return &(Found->*Member);
}

TArray<FString> FSceneSequences::AddAnimation(const stream::Animation& Record)
{
	TArray<FString> Unanimated;
	FAnimation& Animation = Animations.Add(UnrealKit::ToString(Record.key()));
	Animation.Name = UnrealKit::ToString(Record.name());
	for (const stream::Track* TrackRecord : *Record.tracks())
	{
		const FString TrackName = UnrealKit::ToString(TrackRecord->name());
		FTrack& Track = Animation.Tracks.Add(TrackName);
		for (const stream::Channel* Channel : *TrackRecord->channels())
		{
			const FString Property = UnrealKit::ToString(Channel->property());
			if (FKeys* Keys = KeysFor(Track, Property))
				*Keys = KeysOf(Channel->keys());
			else
				Unanimated.Add(TrackName + TEXT(": ") + Property);
		}
	}
	return Unanimated;
}

void FSceneSequences::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Sequence);
}

FFrameNumber FSceneSequences::Tick(const float Frame) const
{
	return FFrameNumber(FMath::RoundToInt32(Frame * TicksPerFrame));
}

ULevelSequence* FSceneSequences::FindOrCreateSequence(FString& Error)
{
	if (Sequence)
		return Sequence;
	const FString PackageName = UnrealKit::PackageOf(SequenceKey);
	Sequence = UnrealKit::FindAsset<ULevelSequence>(PackageName);
	if (!Sequence)
	{
		Sequence = UnrealKit::CreateAsset<ULevelSequence>(PackageName);
		if (!Sequence)
		{
			Error = FString::Printf(TEXT("%s holds another kind of asset"), *PackageName);
			return nullptr;
		}
		Sequence->Initialize();
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		MovieScene->SetDisplayRate(FFrameRate(FramesPerSecond, 1));
		MovieScene->SetTickResolutionDirectly(FFrameRate(FramesPerSecond * TicksPerFrame, 1));
		MovieScene->SetPlaybackRange(Tick(FrameStart), (Tick(FrameEnd) - Tick(FrameStart)).Value);
	}
	return Sequence;
}

bool FSceneSequences::Animate(const FString& AnimationKey, const FString& TrackName, AActor& Actor, USkeletalMesh* Mesh, UCameraComponent* Camera,
                              FString& Error)
{
	const FAnimation* Animation = Animations.Find(AnimationKey);
	const FTrack* Track = Animation ? Animation->Tracks.Find(TrackName) : nullptr;
	if (!Track)
	{
		Error = FString::Printf(TEXT("the animation %s has no track %s"), *AnimationKey, *TrackName);
		return false;
	}
	if (!FindOrCreateSequence(Error))
		return false;
	UMovieScene& MovieScene = *Sequence->GetMovieScene();

	if (const FGuid Known = FindBinding(MovieScene, TrackName); Known.IsValid())
	{
		Sequence->UnbindPossessableObjects(Known);
		Sequence->BindPossessableObject(Known, Actor, Actor.GetWorld());
		if (const FGuid Child = FindChildBinding(MovieScene, Known); Child.IsValid() && Camera)
		{
			Sequence->UnbindPossessableObjects(Child);
			Sequence->BindPossessableObject(Child, *Camera, &Actor);
		}
		Sequence->MarkPackageDirty();
		return true;
	}

	const FGuid Binding = MovieScene.AddPossessable(TrackName, Actor.GetClass());
	Sequence->BindPossessableObject(Binding, Actor, Actor.GetWorld());

	if (Track->Location.Components == 3 || Track->Rotation.Components == 4 || Track->Scale.Components == 3)
	{
		auto& Section = AddSection<UMovieScene3DTransformSection, UMovieScene3DTransformTrack>(MovieScene, Binding);
		const TArrayView<FMovieSceneDoubleChannel*> Channels = Section.GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		const auto AddVectors = [this, &Channels](const FKeys& Keys, const int32 First) {
			for (int32 Key = 0; Key < Keys.Frames.Num(); ++Key)
				for (int32 Axis = 0; Axis < 3; ++Axis)
					Channels[First + Axis]->AddLinearKey(Tick(Keys.Frames[Key]), Keys.Values[Key * 3 + Axis]);
		};
		if (Track->Location.Components == 3)
			AddVectors(Track->Location, 0);
		if (Track->Scale.Components == 3)
			AddVectors(Track->Scale, 6);
		if (Track->Rotation.Components == 4)
		{
			Section.SetUseQuaternionInterpolation(true);
			for (int32 Key = 0; Key < Track->Rotation.Frames.Num(); ++Key)
			{
				const FRotator Rotator = FQuat(QuatAt(Track->Rotation.Values, Key)).Rotator();
				const FFrameNumber Time = Tick(Track->Rotation.Frames[Key]);
				Channels[3]->AddLinearKey(Time, Rotator.Roll);
				Channels[4]->AddLinearKey(Time, Rotator.Pitch);
				Channels[5]->AddLinearKey(Time, Rotator.Yaw);
			}
		}
	}

	if (Track->Visibility.Components == 1)
	{
		UMovieSceneVisibilityTrack* VisibilityTrack = MovieScene.AddTrack<UMovieSceneVisibilityTrack>(Binding);
		VisibilityTrack->SetPropertyNameAndPath(TEXT("bHiddenInGame"), TEXT("bHiddenInGame"));
		auto* Section = CastChecked<UMovieSceneVisibilitySection>(VisibilityTrack->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>::All());
		VisibilityTrack->AddSection(*Section);
		TMovieSceneChannelData<bool> Data = Section->GetChannel().GetData();
		for (int32 Key = 0; Key < Track->Visibility.Frames.Num(); ++Key)
			Data.AddKey(Tick(Track->Visibility.Frames[Key]), Track->Visibility.Values[Key] > 0.5f);
	}

	if (Track->FieldOfView.Components == 1 && Camera)
	{
		const FGuid CameraBinding = MovieScene.AddPossessable(Camera->GetName(), Camera->GetClass());
		MovieScene.FindPossessable(CameraBinding)->SetParent(Binding, &MovieScene);
		Sequence->BindPossessableObject(CameraBinding, *Camera, &Actor);
		UMovieSceneFloatTrack* FieldOfViewTrack = MovieScene.AddTrack<UMovieSceneFloatTrack>(CameraBinding);
		FieldOfViewTrack->SetPropertyNameAndPath(TEXT("FieldOfView"), TEXT("FieldOfView"));
		auto* Section = CastChecked<UMovieSceneFloatSection>(FieldOfViewTrack->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>::All());
		FieldOfViewTrack->AddSection(*Section);
		for (int32 Key = 0; Key < Track->FieldOfView.Frames.Num(); ++Key)
			Section->GetChannel().AddLinearKey(Tick(Track->FieldOfView.Frames[Key]), Track->FieldOfView.Values[Key]);
	}

	if (!Track->Bones.IsEmpty() && Mesh && !AnimateBones(AnimationKey, TrackName, *Track, *Mesh, Binding, Error))
		return false;
	Sequence->MarkPackageDirty();
	return true;
}

bool FSceneSequences::AnimateBones(const FString& AnimationKey, const FString& TrackName, const FTrack& Track, USkeletalMesh& Mesh, const FGuid& Binding,
                                   FString& Error) const
{
	float First = TNumericLimits<float>::Max();
	float Last = TNumericLimits<float>::Lowest();
	for (const FBoneTrack& Bone : Track.Bones)
	{
		for (const FKeys* Keys : {&Bone.Location, &Bone.Rotation, &Bone.Scale})
		{
			if (Keys->Frames.IsEmpty())
				continue;
			First = FMath::Min(First, Keys->Frames[0]);
			Last = FMath::Max(Last, Keys->Frames.Last());
		}
	}
	if (First > Last)
		return true;
	const FString PackageName = UnrealKit::PackageOf(AnimationKey + TEXT(":") + TrackName);
	UAnimSequence* Anim = UnrealKit::FindAsset<UAnimSequence>(PackageName);
	if (!Anim)
	{
		Anim = UnrealKit::CreateAsset<UAnimSequence>(PackageName);
		if (!Anim)
		{
			Error = FString::Printf(TEXT("%s holds another kind of asset"), *PackageName);
			return false;
		}
		Anim->SetSkeleton(Mesh.GetSkeleton());
		Anim->SetPreviewMesh(&Mesh);

		const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
		const TArray<FTransform>& RefPose = Reference.GetRefBonePose();
		const int32 FrameCount = FMath::Max(FMath::RoundToInt32(Last - First) + 1, 2);
		TArray<FVector3f> Locations;
		TArray<FQuat4f> Rotations;
		TArray<FVector3f> Scales;
		IAnimationDataController& Controller = Anim->GetController();
		Controller.OpenBracket(FText::FromString(TrackName), false);
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(FramesPerSecond, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(FrameCount - 1), false);
		for (const FBoneTrack& Bone : Track.Bones)
		{
			const int32 Index = Reference.FindBoneIndex(Bone.Bone);
			if (Index == INDEX_NONE)
				continue;
			const FTransform& Bind = RefPose[Index];
			Locations.Reset(FrameCount);
			Rotations.Reset(FrameCount);
			Scales.Reset(FrameCount);
			int32 LocationCursor = 0, RotationCursor = 0, ScaleCursor = 0;
			for (int32 Frame = 0; Frame < FrameCount; ++Frame)
			{
				const float At = First + Frame;
				const auto Vector = [At](const FKeys& Keys, int32& Cursor, const FVector3f Default) {
					if (Keys.Components != 3)
						return Default;
					return Sample<FVector3f>(
					    Keys.Frames, At, Cursor, [&Keys](const int32 Key) { return VectorAt(Keys.Values, Key); },
					    [](const FVector3f A, const FVector3f B, const float Alpha) { return FMath::Lerp(A, B, Alpha); });
				};
				Locations.Add(Vector(Bone.Location, LocationCursor, FVector3f(Bind.GetLocation())));
				Scales.Add(Vector(Bone.Scale, ScaleCursor, FVector3f(Bind.GetScale3D())));
				Rotations.Add(Bone.Rotation.Components != 4
				                  ? FQuat4f(Bind.GetRotation())
				                  : Sample<FQuat4f>(
				                        Bone.Rotation.Frames, At, RotationCursor, [&Bone](const int32 Key) { return QuatAt(Bone.Rotation.Values, Key); },
				                        [](const FQuat4f A, const FQuat4f B, const float Alpha) { return FQuat4f::Slerp(A, B, Alpha); }));
			}
			Controller.AddBoneCurve(Bone.Bone, false);
			Controller.SetBoneTrackKeys(Bone.Bone, Locations, Rotations, Scales, false);
		}
		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
	}
	UMovieSceneSkeletalAnimationTrack* AnimTrack = Sequence->GetMovieScene()->AddTrack<UMovieSceneSkeletalAnimationTrack>(Binding);
	AnimTrack->AddNewAnimation(Tick(First), Anim);
	return true;
}

#include "SceneImport.h"

#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "SceneAssets.h"
#include "SceneServer.h"

namespace
{
namespace stream = scene::stream;

FTransform TransformOf(const flatbuffers::Vector<float>& Matrix)
{
	FMatrix Result;
	for (int32 Row = 0; Row < 4; ++Row)
		for (int32 Column = 0; Column < 4; ++Column)
			Result.M[Column][Row] = Matrix[Row * 4 + Column];
	return FTransform(Result);
}

template <class TKey, class T>
T* Known(const TMap<TKey, TObjectPtr<T>>& Map, const TKey& Key)
{
	const TObjectPtr<T>* Found = Map.Find(Key);
	return Found ? Found->Get() : nullptr;
}

const char* NameOf(const stream::Record Type)
{
	return stream::EnumNameRecord(Type);
}
}

bool FSceneImport::Accept(const stream::Envelope& Envelope, FString& Error)
{
	const stream::Record Type = Envelope.record_type();
	if (bComplete)
	{
		Error = FString::Printf(TEXT("%s arrived after Complete"), UTF8_TO_TCHAR(NameOf(Type)));
		return false;
	}
	if (Type == stream::Record::Begin)
	{
		if (bBegun)
		{
			Error = TEXT("Begin arrived twice");
			return false;
		}
		bBegun = true;
		Sequences.Begin(UnrealKit::ToString(Envelope.record_as_Begin()->export_id()));
		UE_LOG(LogSceneImport, Display, TEXT("Receiving %s"), *UnrealKit::ToString(Envelope.record_as_Begin()->title()));
		return true;
	}
	if (!bBegun)
	{
		Error = FString::Printf(TEXT("%s arrived before Begin"), UTF8_TO_TCHAR(NameOf(Type)));
		return false;
	}
	if (Type == stream::Record::Complete)
	{
		bComplete = true;
		if (const auto* Records = Envelope.record_as_Complete()->diagnostics())
			for (const stream::Diagnostic* Diagnostic : *Records)
				AppDiagnostics.Add({Diagnostic->severity(), UnrealKit::ToString(Diagnostic->code()), UnrealKit::ToString(Diagnostic->message()),
				                    UnrealKit::ToString(Diagnostic->subject())});
		return true;
	}
	FString BuildError;
	if (!Build(Envelope, BuildError))
	{
		UE_LOG(LogSceneImport, Error, TEXT("%s: %s"), UTF8_TO_TCHAR(NameOf(Type)), *BuildError);
		Diagnostics.Add({stream::Severity::Error, TEXT("import.record"), BuildError, UTF8_TO_TCHAR(NameOf(Type))});
	}
	return true;
}

bool FSceneImport::Build(const stream::Envelope& Envelope, FString& Error)
{
	switch (Envelope.record_type())
	{
	case stream::Record::Image:
		return BuildImage(*Envelope.record_as_Image(), Error);
	case stream::Record::Material:
		return BuildMaterial(*Envelope.record_as_Material(), Error);
	case stream::Record::Skeleton:
		return BuildSkeleton(*Envelope.record_as_Skeleton(), Error);
	case stream::Record::Mesh:
		return BuildMesh(*Envelope.record_as_Mesh(), Error);
	case stream::Record::Light:
	{
		const stream::Light& Record = *Envelope.record_as_Light();
		if (Record.type() == stream::LightType::Sky)
		{
			Error = FString::Printf(TEXT("%s: sky lights are not built in Unreal yet"), *UnrealKit::ToString(Record.name()));
			return false;
		}
		const auto* Color = Record.color();
		if (Color->size() != 3)
		{
			Error = TEXT("the light's color does not have three components");
			return false;
		}
		Lights.Add(
		    UnrealKit::ToString(Record.key()),
		    {Record.type(), FColor(FMath::RoundToInt32((*Color)[0] * 255), FMath::RoundToInt32((*Color)[1] * 255), FMath::RoundToInt32((*Color)[2] * 255)),
		     Record.intensity(), Record.source_radius(), Record.inner_cone_angle(), Record.outer_cone_angle(), Record.source_width(), Record.source_height()});
		return true;
	}
	case stream::Record::Camera:
	{
		const stream::Camera& Record = *Envelope.record_as_Camera();
		Cameras.Add(UnrealKit::ToString(Record.key()), {Record.field_of_view(), Record.near_clip()});
		if (Record.near_clip() > 0.0f && !bNearClipReported)
		{
			bNearClipReported = true;
			Diagnostics.Add({stream::Severity::Info, TEXT("import.camera_near_clip"),
			                 FString::Printf(TEXT("Unreal has no near clip per camera. The camera's is %g cm."), Record.near_clip()),
			                 UnrealKit::ToString(Record.name())});
		}
		return true;
	}
	case stream::Record::Animation:
		for (const FString& Unanimated : Sequences.AddAnimation(*Envelope.record_as_Animation()))
			Diagnostics.Add({stream::Severity::Warning, TEXT("import.animation_property"),
			                 FString::Printf(TEXT("The importer does not animate the property %s."), *Unanimated),
			                 UnrealKit::ToString(Envelope.record_as_Animation()->name())});
		return true;
	case stream::Record::Scene:
		Sequences.SetScene(*Envelope.record_as_Scene());
		return true;
	case stream::Record::Collection:
		return BuildCollection(*Envelope.record_as_Collection(), Error);
	case stream::Record::Object:
		return BuildObject(*Envelope.record_as_Object(), Error);
	default:
		Error = TEXT("the record is not a resource");
		return false;
	}
}

bool FSceneImport::BuildImage(const stream::Image& Record, FString& Error)
{
	const FString Key = UnrealKit::ToString(Record.key());
	const FString PackageName = UnrealKit::PackageOf(Key);
	UTexture2D* Texture = UnrealKit::FindAsset<UTexture2D>(PackageName);
	if (!Texture)
		Texture = UnrealKit::CreateTexture(PackageName, Record, Error);
	if (!Texture)
		return false;
	Textures.Add(Key, Texture);
	return true;
}

bool FSceneImport::BuildMaterial(const stream::Material& Record, FString& Error)
{
	Error = FString::Printf(TEXT("%s: material programs are not built in Unreal yet"), *UnrealKit::ToString(Record.key()));
	return false;
}

bool FSceneImport::BuildSkeleton(const stream::Skeleton& Record, FString& Error)
{
	if (Record.bones()->size() == 0)
	{
		Error = TEXT("the skeleton has no bones");
		return false;
	}
	Skeletons.Add(UnrealKit::ToString(Record.key()), UnrealKit::ReferenceSkeletonOf(Record));
	return true;
}

bool FSceneImport::BuildMesh(const stream::Mesh& Record, FString& Error)
{
	const FString Key = UnrealKit::ToString(Record.key());
	const FString PackageName = UnrealKit::PackageOf(Key);
	const FString SkeletonKey = UnrealKit::ToString(Record.skeleton());
	if (SkeletonKey.IsEmpty())
	{
		UStaticMesh* Mesh = UnrealKit::FindAsset<UStaticMesh>(PackageName);
		if (!Mesh)
			Mesh = UnrealKit::CreateStaticMesh(PackageName, Record, Error);
		if (!Mesh)
			return false;
		StaticMeshes.Add(Key, Mesh);
		return true;
	}
	USkeletalMesh* Mesh = UnrealKit::FindAsset<USkeletalMesh>(PackageName);
	if (!Mesh)
	{
		const FReferenceSkeleton* Reference = Skeletons.Find(SkeletonKey);
		if (!Reference)
		{
			Error = FString::Printf(TEXT("the skeleton %s was not sent"), *SkeletonKey);
			return false;
		}
		Mesh = UnrealKit::CreateSkeletalMesh(PackageName, Record, *Reference, Error);
	}
	if (!Mesh)
		return false;
	SkeletalMeshes.Add(Key, Mesh);
	return true;
}

bool FSceneImport::BuildCollection(const stream::Collection& Record, FString& Error)
{
	const FName Name(UnrealKit::ToString(Record.name()).Replace(TEXT("/"), TEXT("∕")));
	if (Record.parent() < 0)
	{
		Folders.Add(Record.id(), Name);
		return true;
	}
	const FName* Parent = Folders.Find(static_cast<uint32>(Record.parent()));
	if (!Parent)
	{
		Error = FString::Printf(TEXT("the parent collection %d was not sent"), Record.parent());
		return false;
	}
	Folders.Add(Record.id(), FName(Parent->ToString() / Name.ToString()));
	return true;
}

AActor* FSceneImport::SpawnObject(const stream::Object& Record, FString& Error)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	const FString Data = UnrealKit::ToString(Record.data());
	switch (Record.kind())
	{
	case stream::ObjectKind::Mesh:
	{
		if (USkeletalMesh* Mesh = Known(SkeletalMeshes, Data))
		{
			ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>();
			Actor->GetSkeletalMeshComponent()->SetSkeletalMesh(Mesh);
			return Actor;
		}
		UStaticMesh* Mesh = Known(StaticMeshes, Data);
		if (!Mesh)
			break;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		return Actor;
	}
	case stream::ObjectKind::Light:
	{
		const FSceneLight* Light = Lights.Find(Data);
		if (!Light)
			break;
		ULightComponent* Component = nullptr;
		AActor* Actor = nullptr;
		switch (Light->Type)
		{
		case stream::LightType::Directional:
		{
			ADirectionalLight* Directional = World->SpawnActor<ADirectionalLight>();
			Component = Directional->GetLightComponent();
			Actor = Directional;
			break;
		}
		case stream::LightType::Rect:
		{
			ARectLight* Rect = World->SpawnActor<ARectLight>();
			Rect->RectLightComponent->SetSourceWidth(Light->SourceWidth);
			Rect->RectLightComponent->SetSourceHeight(Light->SourceHeight);
			Component = Rect->RectLightComponent;
			Actor = Rect;
			break;
		}
		case stream::LightType::Spot:
		{
			ASpotLight* Spot = World->SpawnActor<ASpotLight>();
			Spot->SpotLightComponent->SetInnerConeAngle(Light->InnerConeAngle);
			Spot->SpotLightComponent->SetOuterConeAngle(Light->OuterConeAngle);
			Spot->SpotLightComponent->SetSourceRadius(Light->SourceRadius);
			Component = Spot->SpotLightComponent;
			Actor = Spot;
			break;
		}
		case stream::LightType::Point:
		{
			APointLight* Point = World->SpawnActor<APointLight>();
			Point->PointLightComponent->SetSourceRadius(Light->SourceRadius);
			Component = Point->PointLightComponent;
			Actor = Point;
			break;
		}
		}
		if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(Component))
			Local->SetIntensityUnits(ELightUnits::Candelas);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetIntensity(Light->Intensity);
		Component->SetLightColor(FLinearColor(Light->Color));
		return Actor;
	}
	case stream::ObjectKind::Camera:
	{
		const FSceneCamera* Camera = Cameras.Find(Data);
		if (!Camera)
			break;
		ACameraActor* Actor = World->SpawnActor<ACameraActor>();
		Actor->GetCameraComponent()->SetFieldOfView(Camera->FieldOfView);
		Actor->GetCameraComponent()->SetConstraintAspectRatio(false);
		return Actor;
	}
	case stream::ObjectKind::Empty:
	{
		AActor* Actor = World->SpawnActor<AActor>();
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"), RF_Transactional);
		Root->SetMobility(EComponentMobility::Movable);
		Actor->SetRootComponent(Root);
		Actor->AddInstanceComponent(Root);
		Root->RegisterComponent();
		return Actor;
	}
	}
	Error = FString::Printf(TEXT("the data %s was not built"), *Data);
	return nullptr;
}

bool FSceneImport::BuildObject(const stream::Object& Record, FString& Error)
{
	if (Record.matrix()->size() != 16)
	{
		Error = TEXT("the object's matrix does not have 16 values");
		return false;
	}
	const FName* Folder = Folders.Find(Record.collection());
	AActor* Parent = Record.parent() < 0 ? nullptr : Known(Actors, static_cast<uint32>(Record.parent()));
	if (!Folder || (Record.parent() >= 0 && !Parent))
	{
		Error = TEXT("the object's collection or parent was not built");
		return false;
	}
	const stream::ViewerVisibility Visibility = Record.viewer_visibility();
	AActor* Viewer = Record.viewer() < 0 ? nullptr : Known(Actors, static_cast<uint32>(Record.viewer()));
	if (Record.viewer() >= 0 && !Viewer)
	{
		Error = TEXT("the object's viewer was not built");
		return false;
	}
	AActor* Actor = SpawnObject(Record, Error);
	if (!Actor)
		return false;
	const FString Name = UnrealKit::ToString(Record.name());
	Actor->SetActorLabel(Name);
	Actor->SetFolderPath(*Folder);
	if (Parent)
	{
		const FName Bone(UnrealKit::ToString(Record.parent_bone()));
		Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepRelativeTransform, Bone);
		Actor->SetActorRelativeTransform(TransformOf(*Record.matrix()));
	}
	else
	{
		Actor->SetActorTransform(TransformOf(*Record.matrix()));
	}

	if (Record.materials())
	{
		UMeshComponent* Mesh = Actor->FindComponentByClass<UMeshComponent>();
		for (uint32 Slot = 0; Mesh && Slot < Record.materials()->size(); ++Slot)
		{
			const FString Key = UnrealKit::ToString(Record.materials()->Get(Slot));
			if (Key.IsEmpty())
				continue;
			UMaterialInterface* Material = Known(Materials, Key);
			if (!Material)
			{
				Diagnostics.Add(
				    {stream::Severity::Warning, TEXT("import.material_missing"), FString::Printf(TEXT("The material %s was not built."), *Key), Name});
				continue;
			}
			Mesh->SetMaterial(static_cast<int32>(Slot), Material);
		}
	}
	if (Record.properties())
		for (const stream::Property* Property : *Record.properties())
			Actor->Tags.Add(FName(UnrealKit::ToString(Property->name()) + TEXT("=") + UnrealKit::ToString(Property->value())));
	if (!Record.visible())
	{
		Actor->SetActorHiddenInGame(true);
		Actor->SetIsTemporarilyHiddenInEditor(true);
	}
	if (Viewer && Visibility != stream::ViewerVisibility::Everyone)
	{
		Actor->SetOwner(Viewer);
		TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			Primitive->SetOnlyOwnerSee(Visibility == stream::ViewerVisibility::OnlyViewer);
			Primitive->SetOwnerNoSee(Visibility == stream::ViewerVisibility::NotViewer);
		}
	}
	Actors.Add(Record.id(), Actor);

	const FString Animation = UnrealKit::ToString(Record.animation());
	if (Animation.IsEmpty())
		return true;
	const ASkeletalMeshActor* Skinned = Record.posed() ? Cast<ASkeletalMeshActor>(Actor) : nullptr;
	const ACameraActor* Camera = Cast<ACameraActor>(Actor);
	return Sequences.Animate(Animation, UnrealKit::ToString(Record.track()), *Actor,
	                         Skinned ? Skinned->GetSkeletalMeshComponent()->GetSkeletalMeshAsset() : nullptr, Camera ? Camera->GetCameraComponent() : nullptr,
	                         Error);
}

TArray<FSceneDiagnostic> FSceneImport::Finish()
{
	TArray<FSceneDiagnostic> Result = MoveTemp(AppDiagnostics);
	Result.Append(MoveTemp(Diagnostics));
	return Result;
}

void FSceneImport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(Textures);
	Collector.AddReferencedObjects(Materials);
	Collector.AddReferencedObjects(StaticMeshes);
	Collector.AddReferencedObjects(SkeletalMeshes);
	Collector.AddReferencedObjects(Actors);
	Sequences.AddReferencedObjects(Collector);
}

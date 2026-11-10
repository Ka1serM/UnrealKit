#include "SceneAssets.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "DDSFile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "ImporterProduct.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "StaticToSkeletalMeshConverter.h"
#include "UObject/Package.h"

THIRD_PARTY_INCLUDES_START
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
THIRD_PARTY_INCLUDES_END

namespace UnrealKit
{
namespace
{
namespace stream = scene::stream;


const FName BaseColorName = TEXT("BaseColor");
const FName NormalName = TEXT("Normal");
const FName PackedName = TEXT("Packed");
const FName EmissiveName = TEXT("Emissive");
const FName MetallicMaskName = TEXT("MetallicMask");
const FName RoughnessMaskName = TEXT("RoughnessMask");
const FName OcclusionMaskName = TEXT("OcclusionMask");
const FName MetallicName = TEXT("Metallic");
const FName RoughnessName = TEXT("Roughness");
const FName OcclusionName = TEXT("Occlusion");
const FName EmissiveStrengthName = TEXT("EmissiveStrength");
const FName OpacityName = TEXT("Opacity");

FString ObjectPathOf(const FString& PackageName)
{
	return PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
}

UObject* LoadQuietly(const FString& PackageName)
{
	const FString ObjectPath = ObjectPathOf(PackageName);
	if (UObject* Loaded = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
		return Loaded;
	if (!FPackageName::DoesPackageExist(PackageName))
		return nullptr;
	return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

template <class T>
T* EngineAsset(const TCHAR* ObjectPath)
{
	return LoadObject<T>(nullptr, ObjectPath);
}

struct FDecodedImage
{
	int32 Width = 0;
	int32 Height = 0;
	ETextureSourceFormat Format = TSF_BGRA8;
	TextureCompressionSettings Compression = TC_Default;
	TArray64<uint8> Texels;
};

template <class TStore>
void DecodeBlocks(const uint8* Source, const int32 Width, const int32 Height, const int32 BlockBytes, const int32 TexelBytes,
                  void (*DecodeBlock)(const void*, void*, int), TStore Store)
{
	uint8 Decoded[16 * 4];
	const int32 BlocksX = FMath::DivideAndRoundUp(Width, 4);
	const int32 BlocksY = FMath::DivideAndRoundUp(Height, 4);
	for (int32 BlockY = 0; BlockY < BlocksY; ++BlockY)
	{
		for (int32 BlockX = 0; BlockX < BlocksX; ++BlockX)
		{
			DecodeBlock(Source, Decoded, 4 * TexelBytes);
			Source += BlockBytes;
			const int32 Columns = FMath::Min(4, Width - BlockX * 4);
			const int32 Rows = FMath::Min(4, Height - BlockY * 4);
			for (int32 Y = 0; Y < Rows; ++Y)
				for (int32 X = 0; X < Columns; ++X)
					Store(int64(BlockY * 4 + Y) * Width + BlockX * 4 + X, Decoded + (Y * 4 + X) * TexelBytes);
		}
	}
}

void DecodeBc1(const void* Block, void* Texels, const int Pitch)
{
	bcdec_bc1(Block, Texels, Pitch);
}

void DecodeBc3(const void* Block, void* Texels, const int Pitch)
{
	bcdec_bc3(Block, Texels, Pitch);
}

void DecodeBc4(const void* Block, void* Texels, const int Pitch)
{
	bcdec_bc4(Block, Texels, Pitch);
}

void DecodeBc5(const void* Block, void* Texels, const int Pitch)
{
	bcdec_bc5(Block, Texels, Pitch);
}

void DecodeBc7(const void* Block, void* Texels, const int Pitch)
{
	bcdec_bc7(Block, Texels, Pitch);
}

bool Decode(const stream::Image& Record, FDecodedImage& Image, FString& Error)
{
	const TUniquePtr<UE::DDS::FDDSFile> File(UE::DDS::FDDSFile::CreateFromDDSInMemory(Record.data()->data(), Record.data()->size()));
	if (!File || File->Dimension != 2 || File->Mips.IsEmpty())
	{
		Error = TEXT("the image is not a two-dimensional DDS file");
		return false;
	}
	const UE::DDS::FDDSMip& Mip = File->Mips[0];
	Image.Width = static_cast<int32>(Mip.Width);
	Image.Height = static_cast<int32>(Mip.Height);
	const int64 Texels = int64(Image.Width) * Image.Height;
	const auto Rgba = [&Image](void (*DecodeBlock)(const void*, void*, int), const uint8* Source) {
		Image.Texels.SetNumUninitialized(int64(Image.Width) * Image.Height * 4);
		uint8* Target = Image.Texels.GetData();
		DecodeBlocks(Source, Image.Width, Image.Height, DecodeBlock == DecodeBc1 ? 8 : 16, 4, DecodeBlock, [Target](const int64 Texel, const uint8* Color) {
			uint8* Out = Target + Texel * 4;
			Out[0] = Color[2];
			Out[1] = Color[1];
			Out[2] = Color[0];
			Out[3] = Color[3];
		});
	};
	using UE::DDS::EDXGIFormat;
	switch (File->DXGIFormat)
	{
	case EDXGIFormat::BC1_UNORM:
	case EDXGIFormat::BC1_UNORM_SRGB:
		Rgba(DecodeBc1, Mip.Data);
		return true;
	case EDXGIFormat::BC3_UNORM:
	case EDXGIFormat::BC3_UNORM_SRGB:
		Rgba(DecodeBc3, Mip.Data);
		return true;
	case EDXGIFormat::BC7_UNORM:
	case EDXGIFormat::BC7_UNORM_SRGB:
		Rgba(DecodeBc7, Mip.Data);
		return true;
	case EDXGIFormat::BC4_UNORM:
	{
		Image.Format = TSF_G8;
		Image.Compression = TC_Grayscale;
		Image.Texels.SetNumUninitialized(Texels);
		uint8* Target = Image.Texels.GetData();
		DecodeBlocks(Mip.Data, Image.Width, Image.Height, 8, 1, DecodeBc4, [Target](const int64 Texel, const uint8* Value) { Target[Texel] = *Value; });
		return true;
	}
	case EDXGIFormat::BC5_UNORM:
	{
		Image.Compression = TC_Normalmap;
		Image.Texels.SetNumUninitialized(Texels * 4);
		uint8* Target = Image.Texels.GetData();
		DecodeBlocks(Mip.Data, Image.Width, Image.Height, 16, 2, DecodeBc5, [Target](const int64 Texel, const uint8* Value) {
			uint8* Out = Target + Texel * 4;
			Out[0] = 255;
			Out[1] = Value[1];
			Out[2] = Value[0];
			Out[3] = 255;
		});
		return true;
	}
	case EDXGIFormat::B8G8R8A8_UNORM:
	case EDXGIFormat::B8G8R8A8_UNORM_SRGB:
		Image.Texels = TArray64<uint8>(Mip.Data, Texels * 4);
		return true;
	case EDXGIFormat::R8_UNORM:
		Image.Format = TSF_G8;
		Image.Compression = TC_Grayscale;
		Image.Texels = TArray64<uint8>(Mip.Data, Texels);
		return true;
	default:
		Error = FString::Printf(TEXT("the DDS format %d is not one that the scene stream carries"), static_cast<int32>(File->DXGIFormat));
		return false;
	}
}

UMaterialExpression* Expression(UMaterial& Material, const TSubclassOf<UMaterialExpression> Class, const int32 X, const int32 Y)
{
	return UMaterialEditingLibrary::CreateMaterialExpression(&Material, Class, X, Y);
}

UMaterialExpressionTextureSampleParameter2D* TextureParameter(UMaterial& Material, const FName Name, const EMaterialSamplerType Sampler, UTexture* Default,
                                                              const int32 Y)
{
	auto* Parameter =
	    Cast<UMaterialExpressionTextureSampleParameter2D>(Expression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -900, Y));
	Parameter->ParameterName = Name;
	Parameter->SamplerType = Sampler;
	Parameter->Texture = Default;
	return Parameter;
}

UMaterialExpression* ScalarParameter(UMaterial& Material, const FName Name, const float Default, const int32 Y)
{
	auto* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression(Material, UMaterialExpressionScalarParameter::StaticClass(), -600, Y));
	Parameter->ParameterName = Name;
	Parameter->DefaultValue = Default;
	return Parameter;
}

UMaterialExpression* VectorParameter(UMaterial& Material, const FName Name, const FLinearColor Default, const int32 Y)
{
	auto* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression(Material, UMaterialExpressionVectorParameter::StaticClass(), -900, Y));
	Parameter->ParameterName = Name;
	Parameter->DefaultValue = Default;
	return Parameter;
}

UMaterialExpression* Binary(UMaterial& Material, const TSubclassOf<UMaterialExpression> Class, UMaterialExpression* A, const FString& AOutput,
                            UMaterialExpression* B, const FString& BOutput, const int32 X, const int32 Y)
{
	UMaterialExpression* Result = Expression(Material, Class, X, Y);
	UMaterialEditingLibrary::ConnectMaterialExpressions(A, AOutput, Result, TEXT("A"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(B, BOutput, Result, TEXT("B"));
	return Result;
}

UMaterialExpression* PackedChannel(UMaterial& Material, UMaterialExpression* Packed, const FName Mask, const FName Value, const float Default, const int32 Y)
{
	UMaterialExpression* MaskParameter = VectorParameter(Material, Mask, FLinearColor::Black, Y);
	UMaterialExpression* Dot = Binary(Material, UMaterialExpressionDotProduct::StaticClass(), Packed, TEXT("RGB"), MaskParameter, TEXT("RGB"), -400, Y);
	return Binary(Material, UMaterialExpressionAdd::StaticClass(), Dot, TEXT(""), ScalarParameter(Material, Value, Default, Y + 120), TEXT(""), -200, Y);
}

UTexture2D* LinearWhite()
{
	if (UTexture2D* Existing = FindAsset<UTexture2D>(ImporterLinearWhitePackage))
		return Existing;
	UTexture2D* Texture = CreateAsset<UTexture2D>(ImporterLinearWhitePackage);
	if (!Texture)
		return nullptr;
	const uint8 Texel[] = {255, 255, 255, 255};
	Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, Texel);
	Texture->SRGB = false;
	Texture->PostEditChange();
	return Texture;
}

UMaterial* MasterMaterial(FString& Error)
{
	if (UMaterial* Existing = FindAsset<UMaterial>(ImporterMasterPackage))
		return Existing;
	UTexture2D* PackedDefault = LinearWhite();
	if (!PackedDefault)
	{
		Error = FString::Printf(TEXT("%s holds another kind of asset"), ImporterLinearWhitePackage);
		return nullptr;
	}
	UMaterial* Material = CreateAsset<UMaterial>(ImporterMasterPackage);
	if (!Material)
	{
		Error = FString::Printf(TEXT("%s holds another kind of asset"), ImporterMasterPackage);
		return nullptr;
	}
	UTexture* White = EngineAsset<UTexture>(TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* Black = EngineAsset<UTexture>(TEXT("/Engine/EngineResources/Black.Black"));
	UTexture* FlatNormal = EngineAsset<UTexture>(TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
	Material->bUsedWithSkeletalMesh = true;
	Material->TwoSided = false;

	UMaterialExpression* BaseColor = TextureParameter(*Material, BaseColorName, SAMPLERTYPE_Color, White, -600);
	UMaterialEditingLibrary::ConnectMaterialProperty(BaseColor, TEXT("RGB"), MP_BaseColor);
	UMaterialExpression* Opacity = Binary(*Material, UMaterialExpressionMultiply::StaticClass(), BaseColor, TEXT("A"),
	                                      ScalarParameter(*Material, OpacityName, 1.0f, -450), TEXT(""), -200, -450);
	UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
	UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_OpacityMask);

	UMaterialExpression* Normal = TextureParameter(*Material, NormalName, SAMPLERTYPE_Normal, FlatNormal, -300);
	UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT("RGB"), MP_Normal);

	UMaterialExpression* Packed = TextureParameter(*Material, PackedName, SAMPLERTYPE_LinearColor, PackedDefault, 0);
	UMaterialEditingLibrary::ConnectMaterialProperty(PackedChannel(*Material, Packed, MetallicMaskName, MetallicName, 0.0f, 0), TEXT(""), MP_Metallic);
	UMaterialEditingLibrary::ConnectMaterialProperty(PackedChannel(*Material, Packed, RoughnessMaskName, RoughnessName, 0.5f, 300), TEXT(""), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(PackedChannel(*Material, Packed, OcclusionMaskName, OcclusionName, 1.0f, 600), TEXT(""),
	                                                 MP_AmbientOcclusion);

	UMaterialExpression* Emissive = TextureParameter(*Material, EmissiveName, SAMPLERTYPE_Color, Black, 900);
	UMaterialExpression* Strength = ScalarParameter(*Material, EmissiveStrengthName, 0.0f, 1050);
	UMaterialEditingLibrary::ConnectMaterialProperty(
	    Binary(*Material, UMaterialExpressionMultiply::StaticClass(), Emissive, TEXT("RGB"), Strength, TEXT(""), -200, 900), TEXT(""), MP_EmissiveColor);

	UMaterialEditingLibrary::RecompileMaterial(Material);
	return Material;
}

void Conform(UTexture2D& Texture, const EMaterialSamplerType Sampler)
{
	const bool bSrgb = Sampler == SAMPLERTYPE_Color;
	const TextureCompressionSettings Compression = Sampler == SAMPLERTYPE_Normal                 ? TC_Normalmap
	                                               : Texture.CompressionSettings == TC_Normalmap ? TC_Default
	                                                                                             : Texture.CompressionSettings.GetValue();
	if (Texture.SRGB == bSrgb && Texture.CompressionSettings == Compression)
		return;
	Texture.SRGB = bSrgb;
	Texture.CompressionSettings = Compression;
	Texture.PostEditChange();
	Texture.MarkPackageDirty();
}

template <class TAttributes>
bool FillMeshDescription(const stream::Mesh& Record, FMeshDescription& Description, TAttributes& Attributes, FString& Error)
{
	const int32 VertexCount = static_cast<int32>(Record.positions()->size() / 3);
	const int32 UvCount = Record.uv_count();
	if (Record.normals()->size() != Record.positions()->size() || Record.uvs()->size() != static_cast<uint32>(VertexCount * UvCount * 2)
	    || Record.colors()->size() != static_cast<uint32>(VertexCount * 4) || Record.indices()->size() % 3 != 0
	    || Record.triangle_materials()->size() != Record.indices()->size() / 3)
	{
		Error = TEXT("the vertex streams of the mesh differ in length");
		return false;
	}
	if (UvCount > MAX_MESH_TEXTURE_COORDS_MD)
	{
		Error = FString::Printf(TEXT("the mesh has %d UV channels"), UvCount);
		return false;
	}
	const float* Positions = Record.positions()->data();
	const float* Normals = Record.normals()->data();
	const float* Uvs = Record.uvs()->data();
	const uint8* Colors = Record.colors()->data();

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstanceUvs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	InstanceUvs.SetNumChannels(FMath::Max(UvCount, 1));

	Description.ReserveNewVertices(VertexCount);
	Description.ReserveNewVertexInstances(VertexCount);
	for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		const FVertexID VertexId = Description.CreateVertex();
		VertexPositions[VertexId] = FVector3f(Positions[Vertex * 3], Positions[Vertex * 3 + 1], Positions[Vertex * 3 + 2]);
		const FVertexInstanceID Instance = Description.CreateVertexInstance(VertexId);
		InstanceNormals[Instance] = FVector3f(Normals[Vertex * 3], Normals[Vertex * 3 + 1], Normals[Vertex * 3 + 2]);
		for (int32 Channel = 0; Channel < UvCount; ++Channel)
		{
			const int64 Uv = (int64(Channel) * VertexCount + Vertex) * 2;
			InstanceUvs.Set(Instance, Channel, FVector2f(Uvs[Uv], Uvs[Uv + 1]));
		}
		const uint8* Color = Colors + Vertex * 4;
		InstanceColors[Instance] = FVector4f(FLinearColor(FColor(Color[0], Color[1], Color[2], Color[3])));
	}

	TArray<FPolygonGroupID> Groups;
	for (const flatbuffers::String* Slot : *Record.material_slots())
	{
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		SlotNames[Group] = FName(ToString(Slot));
		Groups.Add(Group);
	}
	const uint32* Indices = Record.indices()->data();
	const uint16* TriangleMaterials = Record.triangle_materials()->data();
	const int32 TriangleCount = static_cast<int32>(Record.triangle_materials()->size());
	Description.ReserveNewTriangles(TriangleCount);
	Description.ReserveNewPolygons(TriangleCount);
	for (int32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
	{
		const uint32 A = Indices[Triangle * 3], B = Indices[Triangle * 3 + 1], C = Indices[Triangle * 3 + 2];
		if (FMath::Max3(A, B, C) >= static_cast<uint32>(VertexCount) || !Groups.IsValidIndex(TriangleMaterials[Triangle]))
		{
			Error = TEXT("a triangle names a vertex or a material slot that the mesh does not have");
			return false;
		}
		if (A == B || B == C || A == C)
			continue;
		const FVertexInstanceID Corners[3] = {FVertexInstanceID(A), FVertexInstanceID(B), FVertexInstanceID(C)};
		Description.CreateTriangle(Groups[TriangleMaterials[Triangle]], Corners);
	}
	return true;
}
}

FString ToString(const flatbuffers::String* Text)
{
	if (!Text)
		return {};
	return FString(FUTF8ToTCHAR(Text->c_str(), static_cast<int32>(Text->size())));
}

FTransform ToTransform(const flatbuffers::Vector<float>& Location, const flatbuffers::Vector<float>& Rotation, const flatbuffers::Vector<float>& Scale)
{
	if (Location.size() != 3 || Rotation.size() != 4 || Scale.size() != 3)
		return FTransform::Identity;
	return FTransform(FQuat(Rotation[0], Rotation[1], Rotation[2], Rotation[3]), FVector(Location[0], Location[1], Location[2]),
	                  FVector(Scale[0], Scale[1], Scale[2]));
}

FString PackageOf(const FString& Key)
{
	TArray<FString> Parts;
	Key.ParseIntoArray(Parts, TEXT(":"));
	FString Path = Parts.IsEmpty() ? FString() : Parts[0];
	Path.Split(TEXT("."), &Path, nullptr);
	Path.RemoveFromStart(TEXT("/Game"));
	for (int32 Part = 1; Part < Parts.Num(); ++Part)
		Path += TEXT("/") + Parts[Part].Replace(TEXT("/"), TEXT("_"));
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("/"));
	FString Result = ImporterRoot;
	for (const FString& Segment : Segments)
		Result +=
		    TEXT("/")
		    + ObjectTools::SanitizeInvalidChars(ObjectTools::SanitizeInvalidChars(Segment, INVALID_LONGPACKAGE_CHARACTERS), INVALID_OBJECTNAME_CHARACTERS);
	return Result;
}

UObject* FindAsset(const FString& PackageName, const UClass* Class)
{
	UObject* Found = LoadQuietly(PackageName);
	return Found && Found->IsA(Class) ? Found : nullptr;
}

UObject* CreateAsset(const FString& PackageName, UClass* Class)
{
	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	const FString Name = FPackageName::GetShortName(PackageName);
	if (const UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *Name); Existing && !Existing->IsA(Class))
		return nullptr;
	UObject* Asset = NewObject<UObject>(Package, Class, FName(Name), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	return Asset;
}

UTexture2D* CreateTexture(const FString& PackageName, const stream::Image& Record, FString& Error)
{
	FDecodedImage Image;
	if (!Decode(Record, Image, Error))
		return nullptr;
	UTexture2D* Texture = CreateAsset<UTexture2D>(PackageName);
	if (!Texture)
	{
		Error = FString::Printf(TEXT("%s holds another kind of asset"), *PackageName);
		return nullptr;
	}
	Texture->Source.Init(Image.Width, Image.Height, 1, 1, Image.Format, Image.Texels.GetData());
	Texture->SRGB = Record.srgb();
	Texture->CompressionSettings = Image.Compression;
	Texture->PostEditChange();
	return Texture;
}

FReferenceSkeleton ReferenceSkeletonOf(const stream::Skeleton& Record)
{
	FReferenceSkeleton Reference;
	FReferenceSkeletonModifier Modifier(Reference, nullptr);
	for (const stream::Bone* Bone : *Record.bones())
	{
		const FString Name = ToString(Bone->name());
		Modifier.Add(FMeshBoneInfo(FName(Name), Name, Bone->parent()), ToTransform(*Bone->location(), *Bone->rotation(), *Bone->scale()));
	}
	return Reference;
}

UStaticMesh* CreateStaticMesh(const FString& PackageName, const stream::Mesh& Record, FString& Error)
{
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	if (!FillMeshDescription(Record, Description, Attributes, Error))
		return nullptr;
	UStaticMesh* Mesh = CreateAsset<UStaticMesh>(PackageName);
	if (!Mesh)
	{
		Error = FString::Printf(TEXT("%s holds another kind of asset"), *PackageName);
		return nullptr;
	}
	TArray<FStaticMaterial> Slots;
	for (const flatbuffers::String* Slot : *Record.material_slots())
		Slots.Emplace(nullptr, FName(ToString(Slot)), FName(ToString(Slot)));
	Mesh->SetStaticMaterials(Slots);
	FMeshBuildSettings& Settings = Mesh->AddSourceModel().BuildSettings;
	Settings.bRecomputeNormals = false;
	Settings.bRecomputeTangents = true;
	Settings.bRemoveDegenerates = false;
	Settings.bUseFullPrecisionUVs = true;
	Settings.bGenerateLightmapUVs = false;
	Mesh->CreateMeshDescription(0, MoveTemp(Description));
	Mesh->CommitMeshDescription(0);
	for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		Mesh->GetSectionInfoMap().Set(0, Slot, FMeshSectionInfo(Slot));
	Mesh->GetOriginalSectionInfoMap().CopyFrom(Mesh->GetSectionInfoMap());
	Mesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Mesh->PostEditChange();
	return Mesh;
}

USkeletalMesh* CreateSkeletalMesh(const FString& PackageName, const stream::Mesh& Record, const FReferenceSkeleton& Reference, FString& Error)
{
	FMeshDescription Description;
	FSkeletalMeshAttributes Attributes(Description);
	Attributes.Register();
	if (!FillMeshDescription(Record, Description, Attributes, Error))
		return nullptr;
	const int32 VertexCount = Description.Vertices().Num();
	const int32 Influences = Record.influences();
	if (Influences == 0 || !Record.bone_indices() || !Record.bone_weights() || Record.bone_indices()->size() != static_cast<uint32>(VertexCount * Influences)
	    || Record.bone_weights()->size() != Record.bone_indices()->size())
	{
		Error = TEXT("the skin weights of the mesh do not match its vertices");
		return nullptr;
	}
	const uint16* Bones = Record.bone_indices()->data();
	const uint16* Weights = Record.bone_weights()->data();
	const int32 BoneCount = Reference.GetRawBoneNum();
	FSkinWeightsVertexAttributesRef SkinWeights = Attributes.GetVertexSkinWeights();
	TArray<UE::AnimationCore::FBoneWeight, TInlineAllocator<16>> VertexWeights;
	for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		VertexWeights.Reset();
		for (int32 Influence = Vertex * Influences; Influence < (Vertex + 1) * Influences; ++Influence)
		{
			if (Weights[Influence] == 0)
				continue;
			if (Bones[Influence] >= BoneCount)
			{
				Error = TEXT("a vertex is bound to a bone that the skeleton does not have");
				return nullptr;
			}
			VertexWeights.Emplace(Bones[Influence], Weights[Influence] / 65535.0f);
		}
		SkinWeights.Set(FVertexID(Vertex), VertexWeights);
	}

	const FString SkeletonPackage = PackageName + TEXT("_Skeleton");
	USkeleton* Skeleton = FindAsset<USkeleton>(SkeletonPackage);
	if (!Skeleton)
		Skeleton = CreateAsset<USkeleton>(SkeletonPackage);
	USkeletalMesh* Mesh = CreateAsset<USkeletalMesh>(PackageName);
	if (!Skeleton || !Mesh)
	{
		Error = FString::Printf(TEXT("%s or its skeleton holds another kind of asset"), *PackageName);
		return nullptr;
	}
	TArray<FSkeletalMaterial> Slots;
	for (const flatbuffers::String* Slot : *Record.material_slots())
		Slots.Emplace(nullptr, FName(ToString(Slot)), FName(ToString(Slot)));
	Mesh->InvalidateDeriveDataCacheGUID();
	const FMeshDescription* Descriptions[] = {&Description};
	if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(Mesh, Descriptions, Slots, Reference, false, true))
	{
		Error = TEXT("Unreal could not build the skeletal mesh");
		return nullptr;
	}
	if (!Skeleton->MergeAllBonesToBoneTree(Mesh, false))
	{
		Error = FString::Printf(TEXT("the bones do not fit the hierarchy of %s"), *Skeleton->GetPathName());
		return nullptr;
	}
	Mesh->SetSkeleton(Skeleton);
	Skeleton->SetPreviewMesh(Mesh);
	Skeleton->MarkPackageDirty();
	Mesh->PostEditChange();
	return Mesh;
}
}

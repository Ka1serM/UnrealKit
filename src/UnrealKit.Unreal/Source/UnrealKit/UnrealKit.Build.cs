using System.IO;
using UnrealBuildTool;

public class UnrealKit : ModuleRules
{
	public UnrealKit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "ThirdParty"));
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core", "CoreUObject", "Engine", "UnrealEd", "AssetRegistry", "ImageCore", "MeshDescription",
			"StaticMeshDescription", "SkeletalMeshDescription", "SkeletalMeshUtilitiesCommon", "MaterialEditor",
			"LevelSequence", "MovieScene", "MovieSceneTracks", "Sockets", "Networking", "Projects",
		});
	}
}

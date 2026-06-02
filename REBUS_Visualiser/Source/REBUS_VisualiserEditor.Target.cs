// Copyright REBUS Industries. Minimal Editor target so Build.bat can compile the
// project (and its in-tree plugins) headlessly. Non-destructive addition.
using UnrealBuildTool;
using System.Collections.Generic;

public class REBUS_VisualiserEditorTarget : TargetRules
{
	public REBUS_VisualiserEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("REBUS_Visualiser");
	}
}

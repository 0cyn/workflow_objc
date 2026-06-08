#include "Metadata.h"
#include "Workflow.h"

#include <binaryninjaapi.h>

using namespace BinaryNinja;

extern "C"
{
	BN_DECLARE_CORE_ABI_VERSION

#ifndef DEMO_EDITION
	BINARYNINJAPLUGIN void CorePluginDependencies()
	{
		AddOptionalPluginDependency("arch_x86");
		AddOptionalPluginDependency("arch_armv7");
		AddOptionalPluginDependency("arch_arm64");
	}
#endif

#ifdef DEMO_EDITION
	bool WorkflowObjcPluginInit()
#else
	BINARYNINJAPLUGIN bool CorePluginInit()
#endif
	{
		if (Settings::Instance()->Get<bool>("corePlugins.workflows.objc"))
		{
			LogAlert("The 0cyn/workflow_objc plugin conflicts with the default workflow_objc plugin and you must disable it"
			" via the 'corePlugins.workflows.objc' setting in BinaryNinja preferences and restart before you can use this plugin.");
			return false;
		}

		if (!WorkflowObjC::RegisterActivities())
		{
			LogWarn("Failed to register Objective-C workflow");
			return false;
		}
		WorkflowObjC::RegisterRenderLayers();

		return true;
	}
}

#pragma once

#include <binaryninjaapi.h>

#include <cstdint>

namespace WorkflowObjC
{
	/// TODO: Fill in documentation.
	enum class ConfidenceLevel : uint8_t
	{
		ObjCMsgSend = 96,
		AllocInit = 98,
		IvarGetter = 255,
		Retain = 100,
		SuperInit = 100,
	};

	/// TODO: Fill in documentation.
	void RegisterSettings();
	/// TODO: Fill in documentation.
	bool RegisterActivities();
	/// TODO: Fill in documentation.
	void RegisterRenderLayers();
}

namespace WorkflowObjC::Activities
{
	/// TODO: Fill in documentation.
	void ProcessObjCMsgSendCalls(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessDiscoverTypedObjCExterns(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessObjCExterns(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessInlineStubs(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessAllocInit(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessIvarGetterTypes(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessPropagateRetainTypes(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessSuperInit(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessRemoveMemoryManagement(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
	/// TODO: Fill in documentation.
	void ProcessVariableNames(BinaryNinja::Ref<BinaryNinja::AnalysisContext> ac);
}

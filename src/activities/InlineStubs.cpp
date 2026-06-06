#include "../Metadata.h"
#include "../Workflow.h"

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	void ProcessInlineStubs(Ref<AnalysisContext> ac)
	{
		auto view = ac->GetBinaryView();
		if (GlobalState::ShouldIgnoreView(view))
			return;

		auto info = GlobalState::GetAnalysisInfo(view);
		if (!info || !info->objcStubs)
			return;

		auto func = ac->GetFunction();
		if (!info->objcStubs->Contains(func->GetStart()))
			return;

		auto existing = func->GetInlinedDuringAnalysis();
		if (!existing.IsUnknown() && existing.GetValue() == InlineUsingCallAddress)
			return;

		func->SetAutoInlinedDuringAnalysis(InlineUsingCallAddress);
	}
}

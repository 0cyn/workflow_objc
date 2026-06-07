#include "../Metadata.h"
#include "../Workflow.h"

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	/**!
	 *
	 * This activity is in charge of inlining objc_msgSend$blah outlines from newer ObjC binaries
	 *
	 * Does so by just checking if the function it is running on is in that section and editing its inline setting
	 *		if so.
	 *
	 */
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

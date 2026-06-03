#include "Workflow.h"

#include "Metadata.h"

#include <exception>

using namespace BinaryNinja;

namespace WorkflowObjC
{
	namespace
	{
		using ActivityAction = void (*)(Ref<AnalysisContext>);

		class ActivityCallback
		{
			ActivityAction m_action;

		public:
			explicit ActivityCallback(ActivityAction action) : m_action(action) {}

			void operator()(Ref<AnalysisContext> ac) const
			{
				try
				{
					m_action(ac);
				}
				catch (const std::exception& e)
				{
					LogDebugForException(e, "Error occurred while running Objective-C workflow activity: %s", e.what());
				}
				catch (...)
				{
					LogDebug("Unknown error occurred while running Objective-C workflow activity");
				}
			}
		};
	}

	bool RegisterActivities()
	{
		GlobalState::RegisterCleanup();

		Ref<Workflow> baseWorkflow = Workflow::Get("core.function.metaAnalysis");
		if (!baseWorkflow)
			return false;

		Ref<Workflow> workflow = baseWorkflow->Clone("core.function.metaAnalysis");
		if (!workflow)
			return false;

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.analyzeMessageSends",
			"role": "action",
			"title": "Obj-C: Analyze Message Sends",
			"description": "Analyze inline objc_msgSend calls, including applying call type adjustments and resolving to direct calls (if enabled)",
			"eligibility": {
				"auto": {},
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessObjCMsgSendCalls)));

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.inlineStubs",
			"role": "action",
			"title": "Obj-C: Inline Message Send Stubs",
			"description": "Inline Objective-C selector stubs, such as _objc_msgSend$foo, into their callers",
			"eligibility": {
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessInlineStubs)));

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.types.allocInit",
			"role": "action",
			"title": "Obj-C: Adjust return types of objc_alloc_init calls",
			"description": "Adjust the return type of calls to objc_alloc / objc_alloc_init when a fixed type is passed as an argument.",
			"eligibility": {
				"auto": {},
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessAllocInit)));

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.types.superInit",
			"role": "action",
			"title": "Obj-C: Adjust return types of [super init...] calls",
			"description": "Adjust the return type of calls to objc_msgSendSuper2 where the selector is in the init family.",
			"eligibility": {
				"auto": {},
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessSuperInit)));

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.types.retain",
			"role": "action",
			"title": "Obj-C: Propagate types through retain calls",
			"description": "Propagate the retained argument type to the return type of objc_retain-family calls.",
			"eligibility": {
				"auto": {},
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessPropagateRetainTypes)));

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.types.ivarGetter",
			"role": "action",
			"title": "Obj-C: Refine simple ivar getter return types",
			"description": "Refine Objective-C getter implementation return types when the body simply returns a typed ivar.",
			"eligibility": {
				"auto": {},
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessIvarGetterTypes)));

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.discoverTypedExterns",
			"role": "action",
			"title": "Obj-C: Discover typed external methods",
			"description": "Discover Objective-C external method calls whose receiver class is only available after MLIL type propagation.",
			"eligibility": {
				"auto": {},
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessDiscoverTypedObjCExterns)));

		workflow->RegisterActivity(new Activity(R"json({
			"name": "core.function.objectiveC.removeMemoryManagement",
			"role": "action",
			"title": "Obj-C: Remove reference counting calls",
			"description": "Remove calls to objc_retain / objc_release / objc_autorelease to simplify the resulting higher-level ILs",
			"eligibility": {
				"auto": { "default": false },
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					},
					{
						"type": "platform",
						"operator": "in",
						"value": ["mac-aarch64", "ios-aarch64"]
					}
				]
			}
		})json", ActivityCallback(&Activities::ProcessRemoveMemoryManagement)));

		if (!workflow->InsertAfter("core.function.translateTailCalls", "core.function.objectiveC.inlineStubs"))
			return false;
		if (!workflow->InsertAfter("core.function.objectiveC.inlineStubs", "core.function.objectiveC.analyzeMessageSends"))
			return false;
		if (!workflow->Insert("core.function.generateMediumLevelIL", "core.function.objectiveC.removeMemoryManagement"))
			return false;
		if (!workflow->InsertAfter("core.function.generateMediumLevelIL", "core.function.objectiveC.types.allocInit"))
			return false;
		if (!workflow->InsertAfter("core.function.objectiveC.types.allocInit", "core.function.objectiveC.types.superInit"))
			return false;
		if (!workflow->InsertAfter("core.function.objectiveC.types.superInit", "core.function.objectiveC.types.retain"))
			return false;
		if (!workflow->InsertAfter("core.function.objectiveC.types.retain", "core.function.objectiveC.types.ivarGetter"))
			return false;
		if (!workflow->InsertAfter("core.function.objectiveC.types.ivarGetter", "core.function.objectiveC.discoverTypedExterns"))
			return false;

		if (!Workflow::RegisterWorkflow(workflow))
			return false;

		Ref<Workflow> baseModuleWorkflow = Workflow::Get("core.module.metaAnalysis");
		if (!baseModuleWorkflow)
			return false;

		Ref<Workflow> moduleWorkflow = baseModuleWorkflow->Clone("core.module.metaAnalysis");
		if (!moduleWorkflow)
			return false;

		moduleWorkflow->RegisterActivity(new Activity(R"json({
			"name": "core.module.objectiveC.materializeExterns",
			"role": "action",
			"title": "Obj-C: Materialize External Methods",
			"description": "Create Objective-C external method symbols for typed message sends whose receiver class is not present in the current image",
			"eligibility": {
				"runOnce": true,
				"auto": {},
				"predicates": [
					{
						"type": "viewType",
						"operator": "in",
						"value": ["Mach-O", "DSCView"]
					}
				]
			},
			"dependencies": {
				"downstream": ["core.module.update"]
			}
		})json", ActivityCallback(&Activities::ProcessObjCExterns)));

		if (!moduleWorkflow->InsertAfter("core.module.extendedAnalysis", "core.module.objectiveC.materializeExterns"))
			return false;

		return Workflow::RegisterWorkflow(moduleWorkflow);
	}
}

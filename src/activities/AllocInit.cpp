#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	namespace
	{
		const std::vector<std::string_view> kAllocInitFunctions = {
			"_objc_alloc_init",
			"_objc_alloc_initWithZone",
			"_objc_alloc",
			"_objc_allocWithZone",
			"_objc_opt_new",
			"j__objc_alloc_init",
			"j__objc_alloc_initWithZone",
			"j__objc_alloc",
			"j__objc_allocWithZone",
			"j__objc_opt_new",
		};

		Ref<Type> ReturnTypeForAllocCall(const Call& call, BinaryView* view)
		{
			if (call.params.empty())
				return nullptr;

			auto classAddr = MatchConstantPointerOrLoadOfConstantPointer(call.params[0]);
			if (!classAddr)
				return nullptr;

			auto classSymbol = view->GetSymbolByAddress(*classAddr);
			if (!classSymbol)
				return nullptr;

			std::string classSymbolName = classSymbol->GetFullName();
			auto className = ClassNameFromSymbolName(classSymbolName);
			if (!className)
				return nullptr;

			auto classType = NamedType(view, *className);
			if (!classType)
				return nullptr;

			auto function = call.instr.function ? call.instr.function->GetFunction() : nullptr;
			auto arch = function ? function->GetArchitecture() : nullptr;
			if (!arch)
				return nullptr;

			return Type::PointerType(arch, classType);
		}

		void ProcessInstruction(const MediumLevelILInstruction& instr, BinaryView* view)
		{
			auto call = MatchCallToFunctionNamed(instr, view, kAllocInitFunctions);
			if (!call)
				return;

			auto returnType = ReturnTypeForAllocCall(*call, view);
			if (!returnType)
				return;

			AdjustReturnTypeOfCall(*call, returnType, static_cast<uint8_t>(ConfidenceLevel::AllocInit));
		}
	}

	void ProcessAllocInit(Ref<AnalysisContext> ac)
	{
		auto view = ac->GetBinaryView();
		if (GlobalState::ShouldIgnoreView(view))
			return;

		auto mlil = ac->GetMediumLevelILFunction();
		if (!mlil)
			return;

		auto mlilSSA = mlil->GetSSAForm();
		if (!mlilSSA)
			return;

		for (const auto& block : mlilSSA->GetBasicBlocks())
		{
			for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
				ProcessInstruction(mlilSSA->GetInstruction(i), view);
		}
	}
}

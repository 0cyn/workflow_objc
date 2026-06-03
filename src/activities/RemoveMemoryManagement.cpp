#include "../Metadata.h"
#include "../Workflow.h"

#include <lowlevelilinstruction.h>

#include <optional>
#include <string_view>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	namespace
	{
		const std::vector<std::string_view> kIgnorableMemoryManagementFunctions = {
			"_objc_autorelease",
			"_objc_autoreleaseReturnValue",
			"_objc_release",
			"_objc_retain",
			"_objc_retainAutorelease",
			"_objc_retainAutoreleaseReturnValue",
			"_objc_retainAutoreleasedReturnValue",
			"_objc_retainBlock",
			"_objc_unsafeClaimAutoreleasedReturnValue",
		};

		bool IsIgnorableName(std::string_view name)
		{
			if (name.starts_with("j_"))
				name.remove_prefix(2);

			return std::find(kIgnorableMemoryManagementFunctions.begin(), kIgnorableMemoryManagementFunctions.end(), name)
			    != kIgnorableMemoryManagementFunctions.end();
		}

		std::optional<uint64_t> ConstantCallTarget(const PossibleValueSet& value)
		{
			switch (value.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				return static_cast<uint64_t>(value.value);
			default:
				return std::nullopt;
			}
		}

		std::optional<uint64_t> InstructionTargetAddress(LowLevelILFunction* llil, const LowLevelILInstruction& instr)
		{
			if (instr.operation == LLIL_CALL || instr.operation == LLIL_TAILCALL)
				return ConstantCallTarget(instr.GetDestExpr().GetPossibleValues());

			if (instr.operation == LLIL_GOTO)
			{
				size_t targetIndex = instr.GetTarget<LLIL_GOTO>();
				if (targetIndex == BN_INVALID_EXPR || targetIndex >= llil->GetInstructionCount())
					return std::nullopt;
				return llil->GetInstruction(targetIndex).address;
			}

			return std::nullopt;
		}

		bool IsCallToIgnorableMemoryManagementFunction(
		    BinaryView* view, LowLevelILFunction* llil, const LowLevelILInstruction& instr)
		{
			auto target = InstructionTargetAddress(llil, instr);
			if (!target)
				return false;

			auto symbol = view->GetSymbolByAddress(*target);
			return symbol && IsIgnorableName(symbol->GetFullName());
		}

		bool ProcessInstruction(
		    BinaryView* view, LowLevelILFunction* llil, const LowLevelILInstruction& instr,
		    uint32_t linkRegister, size_t linkRegisterSize)
		{
			if (!IsCallToIgnorableMemoryManagementFunction(view, llil, instr))
				return false;

			auto arch = llil->GetArchitecture();
			ILSourceLocation loc(instr);

			if (instr.operation == LLIL_TAILCALL)
			{
				llil->SetCurrentAddress(arch, instr.address);
				llil->ReplaceExpr(instr.exprIndex,
				    llil->Return(llil->Register(linkRegisterSize, linkRegister, loc), loc));
				return true;
			}

			if (instr.operation == LLIL_CALL)
			{
				llil->SetCurrentAddress(arch, instr.address);
				llil->ReplaceExpr(instr.exprIndex, llil->Nop(loc));
				return true;
			}

			if (instr.operation != LLIL_GOTO)
				return false;

			if (instr.instructionIndex == 0)
			{
				llil->SetCurrentAddress(arch, instr.address);
				llil->ReplaceExpr(instr.exprIndex,
				    llil->Return(llil->Register(linkRegisterSize, linkRegister, loc), loc));
				return true;
			}

			auto prev = llil->GetInstruction(instr.instructionIndex - 1);
			if (prev.operation != LLIL_SET_REG || prev.GetDestRegister<LLIL_SET_REG>() != linkRegister)
				return false;

			auto src = prev.GetSourceExpr<LLIL_SET_REG>();
			if (src.operation != LLIL_CONST_PTR)
				return false;

			uint64_t targetAddr = static_cast<uint64_t>(src.GetConstant<LLIL_CONST_PTR>());
			size_t targetIndex = llil->GetInstructionStart(arch, targetAddr);
			if (targetIndex == BN_INVALID_EXPR)
				return false;

			LowLevelILLabel label;
			label.operand = targetIndex;

			llil->SetCurrentAddress(arch, prev.address);
			llil->ReplaceExpr(prev.exprIndex, llil->Nop(ILSourceLocation(prev)));
			llil->SetCurrentAddress(arch, instr.address);
			llil->ReplaceExpr(instr.exprIndex, llil->Goto(label, loc));
			return true;
		}
	}

	void ProcessRemoveMemoryManagement(Ref<AnalysisContext> ac)
	{
		auto view = ac->GetBinaryView();
		if (GlobalState::ShouldIgnoreView(view))
			return;

		auto func = ac->GetFunction();
		if (!func)
			return;

		auto arch = func->GetArchitecture();
		uint32_t linkRegister = arch->GetLinkRegister();
		if (linkRegister == BN_INVALID_REGISTER)
			return;

		auto llil = ac->GetLowLevelILFunction();
		if (!llil)
			return;

		size_t linkRegisterSize = arch->GetRegisterInfo(linkRegister).size;
		bool functionChanged = false;
		for (const auto& block : llil->GetBasicBlocks())
		{
			for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
				functionChanged |= ProcessInstruction(view, llil, llil->GetInstruction(i), linkRegister, linkRegisterSize);
		}

		if (functionChanged)
			llil->GenerateSSAForm();
	}
}

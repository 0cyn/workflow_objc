#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	namespace
	{
		const std::vector<std::string_view> kObjCMsgSendSuperFunctions = {
			"_objc_msgSendSuper2",
			"j__objc_msgSendSuper2",
			"_objc_msgSendSuper",
			"j__objc_msgSendSuper",
		};

		bool IsSameObjCClassPointer(Type* existingType, Type* newType)
		{
			if (!existingType || !newType || !existingType->IsPointer() || !newType->IsPointer())
				return false;

			auto existingClassName = ClassNameFromType(existingType);
			auto newClassName = ClassNameFromType(newType);
			return existingClassName && newClassName && *existingClassName == *newClassName;
		}

		void RewriteStructStore(const MediumLevelILInstruction& instr, BinaryView* view)
		{
			if (instr.operation != MLIL_STORE_SSA)
				return;

			auto dest = instr.GetDestExpr<MLIL_STORE_SSA>();
			if (dest.operation != MLIL_ADD)
				return;

			auto constantInteger = [](const MediumLevelILInstruction& expr) -> std::optional<uint64_t> {
				switch (expr.operation)
				{
				case MLIL_CONST:
					return static_cast<uint64_t>(expr.GetConstant<MLIL_CONST>());
				case MLIL_CONST_PTR:
					return static_cast<uint64_t>(expr.GetConstant<MLIL_CONST_PTR>());
				default:
					return std::nullopt;
				}
			};

			auto left = dest.GetLeftExpr<MLIL_ADD>();
			auto right = dest.GetRightExpr<MLIL_ADD>();
			MediumLevelILInstruction base;
			std::optional<uint64_t> offset;
			if (left.operation == MLIL_VAR_SSA)
			{
				base = left;
				offset = constantInteger(right);
			}
			else if (right.operation == MLIL_VAR_SSA)
			{
				base = right;
				offset = constantInteger(left);
			}

			if (!offset)
				return;

			auto pointeeHasStructMemberAtOffset = [&](Type* pointerType) {
				if (!pointerType || !pointerType->IsPointer())
					return false;

				auto child = pointerType->GetChildType();
				if (child.IsUnknown() || !child.GetValue())
					return false;

				Ref<Type> pointee = child.GetValue();
				if (pointee && pointee->IsNamedTypeRefer())
				{
					auto ref = pointee->GetNamedTypeReference();
					if (ref)
					{
						if (auto resolved = view->GetTypeByRef(ref))
							pointee = resolved;
						else if (auto resolved = view->GetTypeByName(ref->GetName()))
							pointee = resolved;
					}
				}

				if (!pointee || !pointee->IsStructure())
					return false;

				StructureMember member;
				return pointee->GetStructure()->GetMemberAtOffset(static_cast<int64_t>(*offset), member);
			};

			auto type = base.GetType();
			bool hasStructMember = !type.IsUnknown() && pointeeHasStructMemberAtOffset(type.GetValue());
			if (!hasStructMember)
			{
				auto function = instr.function ? instr.function->GetFunction() : nullptr;
				if (!function)
					return;

				type = function->GetVariableType(base.GetSourceSSAVariable<MLIL_VAR_SSA>().var);
				hasStructMember = !type.IsUnknown() && pointeeHasStructMemberAtOffset(type.GetValue());
			}

			if (!hasStructMember)
				return;

			auto baseExpr = base.CopyTo(instr.function);
			auto source = instr.GetSourceExpr<MLIL_STORE_SSA>().CopyTo(instr.function);
			auto replacement = instr.function->StoreStructSSA(instr.size, baseExpr, *offset,
			    instr.GetDestMemoryVersion<MLIL_STORE_SSA>(), instr.GetSourceMemoryVersion<MLIL_STORE_SSA>(), source,
			    ILSourceLocation(instr));
			instr.function->ReplaceExpr(instr.exprIndex, replacement);
		}

		void ProcessInstruction(const MediumLevelILInstruction& instr, BinaryView* view)
		{
			auto call = MatchCallToFunctionNamed(instr, view, kObjCMsgSendSuperFunctions);
			if (!call || call->params.size() < 2)
				return;

			auto selectorAddr = MatchConstantPointerOrLoadOfConstantPointer(call->params[1]);
			if (!selectorAddr)
				return;

			auto selector = Selector::FromAddress(view, *selectorAddr);
			if (!selector || !selector->IsInitFamily())
				return;

			auto superParam = call->params[0];
			if (superParam.operation != MLIL_VAR_SSA)
				return;

			auto mlil = call->instr.function;
			size_t superDefIndex = mlil->GetSSAVarDefinition(superParam.GetSourceSSAVariable<MLIL_VAR_SSA>());
			if (superDefIndex == BN_INVALID_EXPR)
				return;

			auto superDef = mlil->GetInstruction(superDefIndex);
			if (superDef.operation != MLIL_SET_VAR_SSA)
				return;

			auto superSrc = superDef.GetSourceExpr<MLIL_SET_VAR_SSA>();
			if (superSrc.operation != MLIL_ADDRESS_OF)
				return;

			std::optional<std::pair<uint64_t, std::string>> superClassFieldClassReference;
			Variable objcSuperVar = superSrc.GetSourceVariable<MLIL_ADDRESS_OF>();
			for (size_t defIndex : mlil->GetVariableDefinitions(objcSuperVar))
			{
				auto def = mlil->GetInstruction(defIndex);
				if (def.operation != MLIL_SET_VAR_ALIASED_FIELD)
					continue;
				if (def.GetOffset<MLIL_SET_VAR_ALIASED_FIELD>() != view->GetAddressSize())
					continue;

				auto fieldSrc = def.GetSourceExpr<MLIL_SET_VAR_ALIASED_FIELD>();
				auto classAddress = MatchConstantPointerOrLoadOfConstantPointer(fieldSrc);
				if (!classAddress || *classAddress == 0)
					continue;

				auto className = ClassNameFromClassReferenceAddress(view, *classAddress);
				if (!className)
					continue;

				auto classObjectAddress = ClassObjectAddressFromClassReferenceAddress(view, *classAddress).value_or(0);
				if (superClassFieldClassReference &&
				    (superClassFieldClassReference->first != classObjectAddress ||
				        superClassFieldClassReference->second != *className))
				{
					return;
				}
				superClassFieldClassReference = std::make_pair(classObjectAddress, std::move(*className));
			}

			auto function = mlil->GetFunction();
			auto symbol = function ? function->GetSymbol() : nullptr;
			std::optional<std::string> receiverClassName;
			if (symbol)
				receiverClassName = ClassNameFromObjCMethodSymbolName(symbol->GetRawName());

			if (!receiverClassName && superClassFieldClassReference)
			{
				std::string targetName = call->targetName;
				if (targetName.empty())
				{
					auto targetSymbol = call->target ? call->target->GetSymbol() : nullptr;
					if (targetSymbol)
						targetName = targetSymbol->GetFullName();
				}

				std::string_view targetView(targetName);
				if (targetView.starts_with("j_"))
					targetView.remove_prefix(2);
				if (targetView == "_objc_msgSendSuper2")
					receiverClassName = superClassFieldClassReference->second;
			}

			if (!receiverClassName)
				return;

			if (auto analysisInfo = GlobalState::GetAnalysisInfo(view))
				analysisInfo->EnsureClassIvarTypes(view, *receiverClassName);

			auto classType = ClassInstanceType(view, *receiverClassName);
			if (!classType)
				return;

			auto arch = function ? function->GetArchitecture() : nullptr;
			if (!arch)
				return;

			auto returnType = Type::PointerType(arch, classType);

			uint8_t confidence = static_cast<uint8_t>(ConfidenceLevel::SuperInit);
			AdjustReturnTypeOfCall(*call, returnType, confidence);

			auto applyReturnTypeToVariable = [&](const Variable& var) {
				if (!function || function->IsVariableUserDefinded(var))
					return;

				auto existingType = function->GetVariableType(var);
				if (!existingType.IsUnknown() && existingType.GetValue() &&
				    (*existingType.GetValue() == *returnType || IsSameObjCClassPointer(existingType.GetValue(), returnType)))
				{
					return;
				}

				function->CreateAutoVariable(var, Confidence<Ref<Type>>(returnType, confidence),
				    function->GetVariableNameOrDefault(var));
			};

			std::vector<SSAVariable> outputs;
			switch (call->instr.operation)
			{
			case MLIL_CALL_SSA:
				outputs = call->instr.GetOutputSSAVariables<MLIL_CALL_SSA>();
				break;
			case MLIL_TAILCALL_SSA:
				outputs = call->instr.GetOutputSSAVariables<MLIL_TAILCALL_SSA>();
				break;
			default:
				break;
			}

			if (!outputs.empty())
			{
				const auto& objectOutput = outputs[0];
				bool copiedToAnotherVariable = false;
				bool hasNonCopyUse = false;

				for (size_t useIndex : call->instr.function->GetSSAVarUses(objectOutput))
				{
					auto use = call->instr.function->GetInstruction(useIndex);
					if (use.operation != MLIL_SET_VAR_SSA)
					{
						hasNonCopyUse = true;
						continue;
					}

					auto src = use.GetSourceExpr<MLIL_SET_VAR_SSA>();
					if (src.operation != MLIL_VAR_SSA || src.GetSourceSSAVariable<MLIL_VAR_SSA>() != objectOutput)
					{
						hasNonCopyUse = true;
						continue;
					}

					copiedToAnotherVariable = true;
					applyReturnTypeToVariable(use.GetDestSSAVariable<MLIL_SET_VAR_SSA>().var);
				}

				if (!copiedToAnotherVariable || hasNonCopyUse)
					applyReturnTypeToVariable(objectOutput.var);
			}

			if (!function || function->HasUserType() || !symbol)
				return;

			std::string currentMethodName = symbol->GetRawName();
			std::string_view currentMethodView(currentMethodName);
			if (currentMethodView.size() < 5 ||
			    (currentMethodView.front() != '-' && currentMethodView.front() != '+') || currentMethodView[1] != '[' ||
			    currentMethodView.back() != ']')
			{
				return;
			}

			size_t separator = currentMethodView.find(' ', 2);
			if (separator == std::string_view::npos || separator + 1 >= currentMethodView.size() - 1)
				return;

			Selector currentMethodSelector {
			    std::string(currentMethodView.substr(separator + 1, currentMethodView.size() - separator - 2)), 0};
			if (!currentMethodSelector.IsInitFamily())
				return;

			auto existingReturn = function->GetReturnType();
			if (!existingReturn.IsUnknown() && existingReturn.GetValue() &&
			    (*existingReturn.GetValue() == *returnType || IsSameObjCClassPointer(existingReturn.GetValue(), returnType)))
			{
				return;
			}

			function->SetAutoReturnType(Confidence<Ref<Type>>(returnType, confidence));
		}
	}

	void ProcessSuperInit(Ref<AnalysisContext> ac)
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

		for (const auto& block : mlilSSA->GetBasicBlocks())
		{
			for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
				RewriteStructStore(mlilSSA->GetInstruction(i), view);
		}
	}
}

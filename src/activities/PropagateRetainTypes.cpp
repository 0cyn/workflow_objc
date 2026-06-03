#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

#include <algorithm>
#include <optional>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	namespace
	{
		const std::vector<std::string_view> kRetainFunctions = {
			"_objc_retain",
			"_objc_retainAutorelease",
			"_objc_retainAutoreleaseReturnValue",
			"_objc_retainAutoreleasedReturnValue",
			"_objc_retainBlock",
			"j__objc_retain",
			"j__objc_retainAutorelease",
			"j__objc_retainAutoreleaseReturnValue",
			"j__objc_retainAutoreleasedReturnValue",
			"j__objc_retainBlock",
		};

		bool IsRetainReturnTypeCandidate(Type* type)
		{
			if (!type || type->IsVoid())
				return false;

			return type->IsPointer() || type->IsNamedTypeRefer();
		}

		bool IsNamedType(Type* type, std::string_view name)
		{
			if (!type || !type->IsNamedTypeRefer())
				return false;

			auto ref = type->GetNamedTypeReference();
			return ref && ref->GetName().GetString() == name;
		}

		bool IsGenericRetainReturnType(Type* type)
		{
			if (IsNamedType(type, "id") || IsNamedType(type, "Class") || IsNamedType(type, "SEL"))
				return true;

			if (!type || !type->IsPointer())
				return false;

			auto child = type->GetChildType();
			if (child.IsUnknown() || !child.GetValue())
				return false;

			return child.GetValue()->IsVoid() || IsNamedType(child.GetValue(), "objc_object") ||
			    child.GetValue()->GetTypeName().GetString() == "objc_object";
		}

		bool IsNamedRetainedObjectType(Type* type)
		{
			if (!type || !type->IsNamedTypeRefer() || IsGenericRetainReturnType(type))
				return false;

			auto ref = type->GetNamedTypeReference();
			if (!ref)
				return false;

			std::string name = ref->GetName().GetString();
			return !name.empty();
		}

		Confidence<Ref<Type>> NormalizeObjCMethodRetainReturnType(
		    const MediumLevelILInstruction& instr, Confidence<Ref<Type>> type)
		{
			auto value = type.GetValue();
			if (!value || value->IsPointer())
				return type;

			if (!IsNamedRetainedObjectType(value))
				return type;

			auto function = instr.function ? instr.function->GetFunction() : nullptr;
			auto arch = function ? function->GetArchitecture() : nullptr;
			if (!arch)
				return type;

			return Confidence<Ref<Type>>(Type::PointerType(arch, value), type.GetConfidence());
		}

		bool IsObjCMethodTarget(BinaryView* view, const std::optional<uint64_t>& callTarget)
		{
			if (!view || !callTarget)
				return false;

			auto symbol = view->GetSymbolByAddress(*callTarget);
			if (!symbol)
				return false;

			return ClassNameFromObjCMethodSymbolName(symbol->GetRawName()).has_value();
		}

		std::optional<Confidence<Ref<Type>>> RetainReturnTypeCandidate(Confidence<Ref<Type>> type)
		{
			if (type.IsUnknown() || !IsRetainReturnTypeCandidate(type.GetValue()))
				return std::nullopt;

			return type;
		}

		std::optional<MediumLevelILInstruction> CallDestExpr(const MediumLevelILInstruction& instr)
		{
			switch (instr.operation)
			{
			case MLIL_CALL_SSA:
				return instr.GetDestExpr<MLIL_CALL_SSA>();
			case MLIL_TAILCALL_SSA:
				return instr.GetDestExpr<MLIL_TAILCALL_SSA>();
			default:
				return std::nullopt;
			}
		}

		std::optional<Confidence<Ref<Type>>> ReturnTypeFromFunctionType(Type* type)
		{
			if (!type || !type->IsFunction())
				return std::nullopt;

			return RetainReturnTypeCandidate(type->GetReturnValue().type);
		}

		std::optional<Confidence<Ref<Type>>> ReturnTypeForCallExpression(
		    const MediumLevelILInstruction& instr, BinaryView* view)
		{
			auto dest = CallDestExpr(instr);
			if (!dest)
				return std::nullopt;

			auto function = instr.function ? instr.function->GetFunction() : nullptr;
			if (!function)
				return std::nullopt;

			auto callTarget = MatchConstantPointerOrLoadOfConstantPointer(*dest);
			bool isObjCMethodTarget = IsObjCMethodTarget(view, callTarget);
			auto normalizeReturnType = [&](Confidence<Ref<Type>> type) {
				if (isObjCMethodTarget)
					return NormalizeObjCMethodRetainReturnType(instr, type);
				return type;
			};

			std::optional<Confidence<Ref<Type>>> adjustedReturnType;
			auto arch = function->GetArchitecture();
			if (arch)
			{
				auto adjustment = function->GetCallTypeAdjustment(arch, instr.address);
				if (!adjustment.IsUnknown())
					adjustedReturnType = ReturnTypeFromFunctionType(adjustment.GetValue());
				if (adjustedReturnType && !IsGenericRetainReturnType(adjustedReturnType->GetValue()))
					return normalizeReturnType(*adjustedReturnType);
			}

			if (view && callTarget)
			{
				DataVariable dataVariable;
				if (view->GetDataVariableAtAddress(*callTarget, dataVariable) && !dataVariable.type.IsUnknown())
				{
					if (auto type = ReturnTypeFromFunctionType(dataVariable.type.GetValue()))
					{
						if (!IsGenericRetainReturnType(type->GetValue()))
							return normalizeReturnType(*type);
					}
				}
			}

			if (adjustedReturnType)
				return normalizeReturnType(*adjustedReturnType);
			return std::nullopt;
		}

		std::optional<Confidence<Ref<Type>>> ReturnTypeForRetainedExpression(
		    const MediumLevelILInstruction& expr, BinaryView* view, size_t depth = 0)
		{
			if (auto type = ReturnTypeForCallExpression(expr, view))
				return type;

			if (depth < 4 && expr.operation == MLIL_VAR_SSA && expr.function)
			{
				auto var = expr.GetSourceSSAVariable<MLIL_VAR_SSA>();
				size_t defIndex = expr.function->GetSSAVarDefinition(var);
				if (defIndex != BN_INVALID_EXPR)
				{
					auto def = expr.function->GetInstruction(defIndex);
					if (auto type = ReturnTypeForCallExpression(def, view))
						return type;

					if (def.operation == MLIL_SET_VAR_SSA)
					{
						auto src = def.GetSourceExpr<MLIL_SET_VAR_SSA>();
						if (auto type = ReturnTypeForRetainedExpression(src, view, depth + 1))
							return type;
					}
				}
			}

			return RetainReturnTypeCandidate(expr.GetType());
		}

		std::optional<Confidence<Ref<Type>>> ReturnTypeForRetainCall(const Call& call, BinaryView* view)
		{
			if (call.params.empty())
				return std::nullopt;

			auto type = ReturnTypeForRetainedExpression(call.params[0], view);
			if (!type)
				return std::nullopt;

			return type;
		}

		std::vector<SSAVariable> OutputSSAVariables(const MediumLevelILInstruction& instr)
		{
			switch (instr.operation)
			{
			case MLIL_CALL_SSA:
				return instr.GetOutputSSAVariables<MLIL_CALL_SSA>();
			case MLIL_TAILCALL_SSA:
				return instr.GetOutputSSAVariables<MLIL_TAILCALL_SSA>();
			default:
				return {};
			}
		}

		bool ApplyReturnTypeToVariable(Function* function, const Variable& var, Type* returnType, uint8_t confidence)
		{
			if (!function || function->IsVariableUserDefinded(var))
				return false;

			auto existingType = function->GetVariableType(var);
			if (!existingType.IsUnknown() && existingType.GetValue() && *existingType.GetValue() == *returnType)
				return false;

			function->CreateAutoVariable(var, Confidence<Ref<Type>>(returnType, confidence),
			    function->GetVariableNameOrDefault(var));
			return true;
		}

		bool ApplyReturnTypeToOutputVariables(const Call& call, Type* returnType, uint8_t confidence)
		{
			auto outputs = OutputSSAVariables(call.instr);
			if (outputs.size() != 1)
				return false;

			auto function = call.instr.function->GetFunction();
			bool changed = false;
			bool copiedToAnotherVariable = false;
			bool hasNonCopyUse = false;

			for (size_t useIndex : call.instr.function->GetSSAVarUses(outputs[0]))
			{
				auto use = call.instr.function->GetInstruction(useIndex);
				if (use.operation != MLIL_SET_VAR_SSA)
				{
					hasNonCopyUse = true;
					continue;
				}

				auto src = use.GetSourceExpr<MLIL_SET_VAR_SSA>();
				if (src.operation != MLIL_VAR_SSA || src.GetSourceSSAVariable<MLIL_VAR_SSA>() != outputs[0])
				{
					hasNonCopyUse = true;
					continue;
				}

				copiedToAnotherVariable = true;
				changed |= ApplyReturnTypeToVariable(
				    function, use.GetDestSSAVariable<MLIL_SET_VAR_SSA>().var, returnType, confidence);
			}

			if (!copiedToAnotherVariable || hasNonCopyUse)
				changed |= ApplyReturnTypeToVariable(function, outputs[0].var, returnType, confidence);

			return changed;
		}

		void ProcessInstruction(const MediumLevelILInstruction& instr, BinaryView* view)
		{
			auto call = MatchCallToFunctionNamed(instr, view, kRetainFunctions);
			if (!call)
				return;

			auto returnType = ReturnTypeForRetainCall(*call, view);
			if (!returnType)
				return;

			uint8_t confidence = returnType->GetConfidence();
			AdjustReturnTypeOfCall(*call, returnType->GetValue(), confidence);
			ApplyReturnTypeToOutputVariables(*call, returnType->GetValue(), confidence);
		}
	}

	void ProcessPropagateRetainTypes(Ref<AnalysisContext> ac)
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

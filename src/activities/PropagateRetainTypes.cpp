#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

#include <functional>
#include <optional>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
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

		static const std::vector<std::string_view> retainFunctions = {
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

		std::function<std::optional<Confidence<Ref<Type>>>(const MediumLevelILInstruction&, size_t)>
		    returnTypeForRetainedExpression;
		returnTypeForRetainedExpression = [&](const MediumLevelILInstruction& expr, size_t depth)
		    -> std::optional<Confidence<Ref<Type>>> {
			auto isNamedType = [](Type* type, std::string_view name) {
				if (!type || !type->IsNamedTypeRefer())
					return false;

				auto ref = type->GetNamedTypeReference();
				return ref && ref->GetName().GetString() == name;
			};

			auto isGenericRetainReturnType = [&](Type* type) {
				if (isNamedType(type, "id") || isNamedType(type, "Class") || isNamedType(type, "SEL"))
					return true;

				if (!type || !type->IsPointer())
					return false;

				auto child = type->GetChildType();
				if (child.IsUnknown() || !child.GetValue())
					return false;

				return child.GetValue()->IsVoid() || isNamedType(child.GetValue(), "objc_object") ||
				    child.GetValue()->GetTypeName().GetString() == "objc_object";
			};

			auto retainReturnTypeCandidate = [](Confidence<Ref<Type>> type) -> std::optional<Confidence<Ref<Type>>> {
				if (type.IsUnknown())
					return std::nullopt;

				auto value = type.GetValue();
				if (!value || value->IsVoid() || (!value->IsPointer() && !value->IsNamedTypeRefer()))
					return std::nullopt;

				return type;
			};

			auto returnTypeFromFunctionType = [&](Type* type) -> std::optional<Confidence<Ref<Type>>> {
				if (!type || !type->IsFunction())
					return std::nullopt;

				return retainReturnTypeCandidate(type->GetReturnValue().type);
			};

			auto normalizeObjCMethodReturnType = [&](
			    const MediumLevelILInstruction& instr, Confidence<Ref<Type>> type) {
				auto value = type.GetValue();
				if (!value || value->IsPointer())
					return type;

				if (!value->IsNamedTypeRefer() || isGenericRetainReturnType(value))
					return type;

				auto ref = value->GetNamedTypeReference();
				if (!ref || ref->GetName().GetString().empty())
					return type;

				auto function = instr.function ? instr.function->GetFunction() : nullptr;
				auto arch = function ? function->GetArchitecture() : nullptr;
				if (!arch)
					return type;

				return Confidence<Ref<Type>>(Type::PointerType(arch, value), type.GetConfidence());
			};

			auto returnTypeForCallExpression = [&](
			    const MediumLevelILInstruction& instr) -> std::optional<Confidence<Ref<Type>>> {
				MediumLevelILInstruction dest;
				switch (instr.operation)
				{
				case MLIL_CALL_SSA:
					dest = instr.GetDestExpr<MLIL_CALL_SSA>();
					break;
				case MLIL_TAILCALL_SSA:
					dest = instr.GetDestExpr<MLIL_TAILCALL_SSA>();
					break;
				default:
					return std::nullopt;
				}

				auto function = instr.function ? instr.function->GetFunction() : nullptr;
				if (!function)
					return std::nullopt;

				auto callTarget = MatchConstantPointerOrLoadOfConstantPointer(dest);
				bool isObjCMethodTarget = false;
				if (view && callTarget)
				{
					auto symbol = view->GetSymbolByAddress(*callTarget);
					isObjCMethodTarget = symbol && ClassNameFromObjCMethodSymbolName(symbol->GetRawName()).has_value();
				}

				auto normalizeReturnType = [&](Confidence<Ref<Type>> type) {
					if (isObjCMethodTarget)
						return normalizeObjCMethodReturnType(instr, type);
					return type;
				};

				std::optional<Confidence<Ref<Type>>> adjustedReturnType;
				auto arch = function->GetArchitecture();
				if (arch)
				{
					auto adjustment = function->GetCallTypeAdjustment(arch, instr.address);
					if (!adjustment.IsUnknown())
						adjustedReturnType = returnTypeFromFunctionType(adjustment.GetValue());
					if (adjustedReturnType && !isGenericRetainReturnType(adjustedReturnType->GetValue()))
						return normalizeReturnType(*adjustedReturnType);
				}

				if (view && callTarget)
				{
					DataVariable dataVariable;
					if (view->GetDataVariableAtAddress(*callTarget, dataVariable) && !dataVariable.type.IsUnknown())
					{
						if (auto type = returnTypeFromFunctionType(dataVariable.type.GetValue()))
						{
							if (!isGenericRetainReturnType(type->GetValue()))
								return normalizeReturnType(*type);
						}
					}
				}

				if (adjustedReturnType)
					return normalizeReturnType(*adjustedReturnType);
				return std::nullopt;
			};

			if (auto type = returnTypeForCallExpression(expr))
				return type;

			if (depth < 4 && expr.operation == MLIL_VAR_SSA && expr.function)
			{
				auto var = expr.GetSourceSSAVariable<MLIL_VAR_SSA>();
				size_t defIndex = expr.function->GetSSAVarDefinition(var);
				if (defIndex != BN_INVALID_EXPR)
				{
					auto def = expr.function->GetInstruction(defIndex);
					if (auto type = returnTypeForCallExpression(def))
						return type;

					if (def.operation == MLIL_SET_VAR_SSA)
					{
						auto src = def.GetSourceExpr<MLIL_SET_VAR_SSA>();
						if (auto type = returnTypeForRetainedExpression(src, depth + 1))
							return type;
					}
				}
			}

			return retainReturnTypeCandidate(expr.GetType());
		};

		for (const auto& block : mlilSSA->GetBasicBlocks())
		{
			for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
			{
				auto instr = mlilSSA->GetInstruction(i);
				auto call = MatchCallToFunctionNamed(instr, view, retainFunctions);
				if (!call || call->params.empty())
					continue;

				auto returnType = returnTypeForRetainedExpression(call->params[0], 0);
				if (!returnType)
					continue;

				uint8_t confidence = returnType->GetConfidence();
				auto returnTypeValue = returnType->GetValue();
				AdjustReturnTypeOfCall(*call, returnTypeValue, confidence);

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

				if (outputs.size() != 1)
					continue;

				auto function = call->instr.function->GetFunction();
				auto applyReturnTypeToVariable = [&](const Variable& var) {
					if (!function || function->IsVariableUserDefinded(var))
						return;

					auto existingType = function->GetVariableType(var);
					if (!existingType.IsUnknown() && existingType.GetValue() && *existingType.GetValue() == *returnTypeValue)
						return;

					function->CreateAutoVariable(var, Confidence<Ref<Type>>(returnTypeValue, confidence),
					    function->GetVariableNameOrDefault(var));
				};

				bool copiedToAnotherVariable = false;
				bool hasNonCopyUse = false;
				for (size_t useIndex : call->instr.function->GetSSAVarUses(outputs[0]))
				{
					auto use = call->instr.function->GetInstruction(useIndex);
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
					applyReturnTypeToVariable(use.GetDestSSAVariable<MLIL_SET_VAR_SSA>().var);
				}

				if (!copiedToAnotherVariable || hasNonCopyUse)
					applyReturnTypeToVariable(outputs[0].var);
			}
		}
	}
}

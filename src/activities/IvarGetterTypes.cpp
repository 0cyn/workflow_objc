#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

#include <optional>
#include <string_view>
#include <vector>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	namespace
	{
		const std::vector<std::string_view> kObjCGetPropertyFunctions = {
			"_objc_getProperty",
			"j__objc_getProperty",
		};

		bool IsObjectiveCInstanceGetter(Function* func)
		{
			// FIXME: We could with some wiring check metadata to know this more confidently.
			if (!func)
				return false;

			auto symbol = func->GetSymbol();
			if (!symbol)
				return false;

			std::string name = symbol->GetRawName();
			std::string_view view(name);
			if (!view.starts_with("-[") || !view.ends_with("]"))
				return false;

			view.remove_prefix(2);
			view.remove_suffix(1);
			size_t selectorStart = view.find(' ');
			if (selectorStart == std::string_view::npos || selectorStart + 1 >= view.size())
				return false;

			std::string_view selector = view.substr(selectorStart + 1);
			return selector.find(':') == std::string_view::npos;
		}

		bool IsNamedType(Type* type, std::string_view name)
		{
			if (!type || !type->IsNamedTypeRefer())
				return false;

			auto ref = type->GetNamedTypeReference();
			return ref && ref->GetName().GetString() == name;
		}

		bool IsIdType(Type* type)
		{
			if (IsNamedType(type, "id"))
				return true;

			if (!type || !type->IsPointer())
				return false;

			auto childType = type->GetChildType();
			if (childType.IsUnknown() || !childType.GetValue())
				return false;

			return IsNamedType(childType.GetValue(), "objc_object") ||
			    childType.GetValue()->GetTypeName().GetString() == "objc_object";
		}

		bool ShouldRefineReturnType(Function* func, Type* returnType)
		{
			if (!func || !returnType || returnType->IsVoid())
				return false;

			auto currentReturn = func->GetReturnType();
			if (currentReturn.IsUnknown() || !currentReturn.GetValue())
				return true;

			if (*currentReturn.GetValue() == *returnType)
				return false;

			return IsIdType(currentReturn.GetValue());
		}

		bool IsSelfVariable(Function* func, const SSAVariable& var)
		{
			if (!func)
				return false;

			auto params = func->GetParameterVariables();
			if (params.IsUnknown() || params.GetValue().empty())
				return func->GetVariableNameOrDefault(var.var) == "self";

			return var.var == params.GetValue().front();
		}

		Ref<Type> ResolveNamedType(BinaryView* view, Type* type)
		{
			if (!type || !type->IsNamedTypeRefer())
				return type;

			auto ref = type->GetNamedTypeReference();
			if (!ref)
				return type;

			if (auto resolved = view->GetTypeByRef(ref))
				return resolved;
			if (auto resolved = view->GetTypeByName(ref->GetName()))
				return resolved;
			return type;
		}

		std::optional<Confidence<Ref<Type>>> MemberTypeAtOffset(BinaryView* view, Type* pointerType, uint64_t offset)
		{
			if (!view || !pointerType || !pointerType->IsPointer())
				return std::nullopt;

			auto child = pointerType->GetChildType();
			if (child.IsUnknown() || !child.GetValue())
				return std::nullopt;

			auto pointee = ResolveNamedType(view, child.GetValue());
			if (!pointee || !pointee->IsStructure())
				return std::nullopt;

			InheritedStructureMember inheritedMember;
			if (pointee->GetStructure()->GetMemberIncludingInheritedAtOffset(view, static_cast<int64_t>(offset), inheritedMember) &&
			    !inheritedMember.member.type.IsUnknown() && inheritedMember.member.type.GetValue())
			{
				return inheritedMember.member.type;
			}

			StructureMember member;
			if (pointee->GetStructure()->GetMemberAtOffset(static_cast<int64_t>(offset), member) &&
			    !member.type.IsUnknown() && member.type.GetValue())
			{
				return member.type;
			}

			return std::nullopt;
		}

		std::optional<Confidence<Ref<Type>>> MemberTypeForSelfArgument(
		    BinaryView* view, Function* func, const MediumLevelILInstruction& selfArg, uint64_t offset)
		{
			if (!func || selfArg.operation != MLIL_VAR_SSA)
				return std::nullopt;

			auto selfVar = selfArg.GetSourceSSAVariable<MLIL_VAR_SSA>();
			if (!IsSelfVariable(func, selfVar))
				return std::nullopt;

			auto varType = func->GetVariableType(selfVar.var);
			if (!varType.IsUnknown())
			{
				if (auto type = MemberTypeAtOffset(view, varType.GetValue(), offset))
					return type;
			}

			auto exprType = selfArg.GetType();
			if (!exprType.IsUnknown())
				return MemberTypeAtOffset(view, exprType.GetValue(), offset);

			return std::nullopt;
		}

		std::optional<Confidence<Ref<Type>>> ReturnTypeForObjCGetPropertyCall(
		    BinaryView* view, Function* func, const MediumLevelILInstruction& instr)
		{
			auto call = MatchCallToFunctionNamed(instr, view, kObjCGetPropertyFunctions);
			if (!call || call->params.size() < 3)
				return std::nullopt;

			const auto& offsetParam = call->params.size() == 3 ? call->params[1] : call->params[2];
			auto offsetValue = offsetParam.GetPossibleValues();
			switch (offsetValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				return MemberTypeForSelfArgument(
				    view, func, call->params[0], static_cast<uint64_t>(offsetValue.value));
			default:
				return std::nullopt;
			}
		}

		std::vector<MediumLevelILInstruction> Instructions(MediumLevelILFunction* function)
		{
			std::vector<MediumLevelILInstruction> result;
			for (const auto& block : function->GetBasicBlocks())
			{
				for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
				{
					auto instr = function->GetInstruction(i);
					if (instr.operation == MLIL_GOTO || instr.operation == MLIL_NOP)
						continue;
					result.push_back(instr);
				}
			}
			return result;
		}

		void EnsureCurrentMethodIvarTypes(BinaryView* view, Function* func)
		{
			if (!view || !func)
				return;

			auto symbol = func->GetSymbol();
			if (!symbol)
				return;

			auto className = ClassNameFromObjCMethodSymbolName(symbol->GetRawName());
			if (!className)
				return;

			if (auto info = GlobalState::GetAnalysisInfo(view))
				info->EnsureClassIvarTypes(view, *className);
		}

	}

	void ProcessIvarGetterTypes(Ref<AnalysisContext> ac)
	{
		auto view = ac->GetBinaryView();
		if (GlobalState::ShouldIgnoreView(view))
			return;

		auto func = ac->GetFunction();
		if (!func || func->HasUserType() || !IsObjectiveCInstanceGetter(func))
			return;

		auto mlil = ac->GetMediumLevelILFunction();
		if (!mlil)
			return;

		auto mlilSSA = mlil->GetSSAForm();
		if (!mlilSSA)
			return;

		EnsureCurrentMethodIvarTypes(view, func);

		std::optional<Confidence<Ref<Type>>> returnType;
		auto instructions = Instructions(mlilSSA);
		if (instructions.size() == 1)
		{
			returnType = ReturnTypeForObjCGetPropertyCall(view, func, instructions[0]);
		}
		else if (instructions.size() == 2)
		{
			auto setVar = instructions[0];
			auto ret = instructions[1];
			if (auto type = ReturnTypeForObjCGetPropertyCall(view, func, setVar))
			{
				returnType = type;
			}
			else
			{
				if (setVar.operation != MLIL_SET_VAR_SSA || ret.operation != MLIL_RET)
					return;

				auto returnExprs = ret.GetSourceExprs<MLIL_RET>();
				if (returnExprs.size() != 1 || returnExprs[0].operation != MLIL_VAR_SSA)
					return;

				if (returnExprs[0].GetSourceSSAVariable<MLIL_VAR_SSA>() !=
				    setVar.GetDestSSAVariable<MLIL_SET_VAR_SSA>())
				{
					return;
				}

				auto value = setVar.GetSourceExpr<MLIL_SET_VAR_SSA>();
				if (auto type = ReturnTypeForObjCGetPropertyCall(view, func, value))
				{
					returnType = type;
				}
				else
				{
					if (value.operation != MLIL_LOAD_STRUCT_SSA)
						return;

					auto base = value.GetSourceExpr<MLIL_LOAD_STRUCT_SSA>();
					if (base.operation != MLIL_VAR_SSA)
						return;

					if (!IsSelfVariable(func, base.GetSourceSSAVariable<MLIL_VAR_SSA>()))
						return;

					auto valueType = value.GetType();
					if (valueType.IsUnknown() || !valueType.GetValue())
						return;

					returnType = valueType;
				}
			}
		}

		if (!returnType || !ShouldRefineReturnType(func, returnType->GetValue()))
			return;

		func->SetAutoReturnType(Confidence<Ref<Type>>(
		    returnType->GetValue(), static_cast<uint8_t>(ConfidenceLevel::IvarGetter)));
	}
}

#include "../Metadata.h"
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
		bool IsObjectiveCInstanceGetter(Function* func)
		{
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
			auto params = func->GetParameterVariables();
			if (params.IsUnknown() || params.GetValue().empty())
				return false;

			return var.var == params.GetValue().front();
		}

		std::vector<MediumLevelILInstruction> Instructions(MediumLevelILFunction* function)
		{
			std::vector<MediumLevelILInstruction> result;
			for (const auto& block : function->GetBasicBlocks())
			{
				for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
					result.push_back(function->GetInstruction(i));
			}
			return result;
		}

		std::optional<Confidence<Ref<Type>>> ReturnTypeForSimpleIvarGetter(Function* func, MediumLevelILFunction* ssa)
		{
			auto instructions = Instructions(ssa);
			if (instructions.size() != 2)
				return std::nullopt;

			auto setVar = instructions[0];
			auto ret = instructions[1];
			if (setVar.operation != MLIL_SET_VAR_SSA || ret.operation != MLIL_RET)
				return std::nullopt;

			auto returnExprs = ret.GetSourceExprs<MLIL_RET>();
			if (returnExprs.size() != 1 || returnExprs[0].operation != MLIL_VAR_SSA)
				return std::nullopt;

			if (returnExprs[0].GetSourceSSAVariable<MLIL_VAR_SSA>() != setVar.GetDestSSAVariable<MLIL_SET_VAR_SSA>())
				return std::nullopt;

			auto value = setVar.GetSourceExpr<MLIL_SET_VAR_SSA>();
			if (value.operation != MLIL_LOAD_STRUCT_SSA)
				return std::nullopt;

			auto base = value.GetSourceExpr<MLIL_LOAD_STRUCT_SSA>();
			if (base.operation != MLIL_VAR_SSA)
				return std::nullopt;

			if (!IsSelfVariable(func, base.GetSourceSSAVariable<MLIL_VAR_SSA>()))
				return std::nullopt;

			auto type = value.GetType();
			if (type.IsUnknown() || !type.GetValue())
				return std::nullopt;

			return type;
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

		auto returnType = ReturnTypeForSimpleIvarGetter(func, mlilSSA);
		if (!returnType || !ShouldRefineReturnType(func, returnType->GetValue()))
			return;

		func->SetAutoReturnType(Confidence<Ref<Type>>(
		    returnType->GetValue(), static_cast<uint8_t>(ConfidenceLevel::IvarGetter)));
	}
}

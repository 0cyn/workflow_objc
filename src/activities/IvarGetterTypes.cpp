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

		std::optional<uint64_t> ConstantInteger(const MediumLevelILInstruction& expr)
		{
			switch (expr.operation)
			{
			case MLIL_CONST:
				return static_cast<uint64_t>(expr.GetConstant<MLIL_CONST>());
			case MLIL_CONST_PTR:
				return static_cast<uint64_t>(expr.GetConstant<MLIL_CONST_PTR>());
			default:
				break;
			}

			auto value = expr.GetPossibleValues();
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

		std::optional<uint64_t> ObjCGetPropertyOffset(const std::vector<MediumLevelILInstruction>& params)
		{
			if (params.size() == 3)
				return ConstantInteger(params[1]);
			if (params.size() >= 4)
				return ConstantInteger(params[2]);
			return std::nullopt;
		}

		std::optional<Confidence<Ref<Type>>> ReturnTypeForObjCGetPropertyCall(
		    BinaryView* view, Function* func, const MediumLevelILInstruction& instr)
		{
			auto call = MatchCallToFunctionNamed(instr, view, kObjCGetPropertyFunctions);
			if (!call || call->params.size() < 3)
				return std::nullopt;

			auto offset = ObjCGetPropertyOffset(call->params);
			if (!offset)
				return std::nullopt;

			return MemberTypeForSelfArgument(view, func, call->params[0], *offset);
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

		std::optional<Confidence<Ref<Type>>> ReturnTypeForSimpleIvarGetter(
		    BinaryView* view, Function* func, MediumLevelILFunction* ssa)
		{
			auto instructions = Instructions(ssa);
			if (instructions.size() == 1)
				return ReturnTypeForObjCGetPropertyCall(view, func, instructions[0]);

			if (instructions.size() != 2)
				return std::nullopt;

			auto setVar = instructions[0];
			auto ret = instructions[1];
			if (auto type = ReturnTypeForObjCGetPropertyCall(view, func, setVar))
				return type;

			if (setVar.operation != MLIL_SET_VAR_SSA || ret.operation != MLIL_RET)
				return std::nullopt;

			auto returnExprs = ret.GetSourceExprs<MLIL_RET>();
			if (returnExprs.size() != 1 || returnExprs[0].operation != MLIL_VAR_SSA)
				return std::nullopt;

			if (returnExprs[0].GetSourceSSAVariable<MLIL_VAR_SSA>() != setVar.GetDestSSAVariable<MLIL_SET_VAR_SSA>())
				return std::nullopt;

			auto value = setVar.GetSourceExpr<MLIL_SET_VAR_SSA>();
			if (auto type = ReturnTypeForObjCGetPropertyCall(view, func, value))
				return type;

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

		EnsureCurrentMethodIvarTypes(view, func);

		auto returnType = ReturnTypeForSimpleIvarGetter(view, func, mlilSSA);
		if (!returnType || !ShouldRefineReturnType(func, returnType->GetValue()))
			return;

		func->SetAutoReturnType(Confidence<Ref<Type>>(
		    returnType->GetValue(), static_cast<uint8_t>(ConfidenceLevel::IvarGetter)));
	}
}

#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

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

		struct SuperCallInfo
		{
			Selector selector;
			std::string receiverClassName;
		};

		struct ClassReferenceInfo
		{
			uint64_t address = 0;
			std::string className;
		};

		bool IsObjCMsgSendSuper2(const Call& call)
		{
			std::string name = call.targetName;
			if (name.empty())
			{
				auto symbol = call.target ? call.target->GetSymbol() : nullptr;
				if (!symbol)
					return false;
				name = symbol->GetFullName();
			}

			std::string_view view(name);
			if (view.starts_with("j_"))
				view.remove_prefix(2);
			return view == "_objc_msgSendSuper2";
		}

		bool IsSameObjCClassPointer(Type* existingType, Type* newType)
		{
			if (!existingType || !newType || !existingType->IsPointer() || !newType->IsPointer())
				return false;

			auto existingClassName = ClassNameFromType(existingType);
			auto newClassName = ClassNameFromType(newType);
			return existingClassName && newClassName && *existingClassName == *newClassName;
		}

		std::optional<ClassReferenceInfo> ClassReferenceFromExpr(
		    BinaryView* view, const MediumLevelILInstruction& expr)
		{
			auto classAddress = MatchConstantPointerOrLoadOfConstantPointer(expr);
			if (!classAddress || *classAddress == 0)
				return std::nullopt;

			auto classObjectAddress = ClassObjectAddressFromClassReferenceAddress(view, *classAddress);
			auto className = ClassNameFromClassReferenceAddress(view, *classAddress);
			if (!className)
				return std::nullopt;
			return ClassReferenceInfo {classObjectAddress.value_or(0), std::move(*className)};
		}

		std::optional<ClassReferenceInfo> SuperClassFieldClassReference(
		    const Call& call, BinaryView* view, Variable objcSuperVar)
		{
			auto function = call.instr.function;
			std::optional<ClassReferenceInfo> classReference;
			for (size_t defIndex : function->GetVariableDefinitions(objcSuperVar))
			{
				auto def = function->GetInstruction(defIndex);
				if (def.operation != MLIL_SET_VAR_ALIASED_FIELD)
					continue;
				if (def.GetOffset<MLIL_SET_VAR_ALIASED_FIELD>() != view->GetAddressSize())
					continue;

				auto fieldSrc = def.GetSourceExpr<MLIL_SET_VAR_ALIASED_FIELD>();
				auto candidate = ClassReferenceFromExpr(view, fieldSrc);
				if (!candidate)
					continue;
				if (classReference &&
				    (classReference->address != candidate->address || classReference->className != candidate->className))
					return std::nullopt;
				classReference = std::move(*candidate);
			}

			return classReference;
		}

		std::optional<std::string> ClassNameFromCurrentMethod(const Call& call)
		{
			auto function = call.instr.function ? call.instr.function->GetFunction() : nullptr;
			auto symbol = function ? function->GetSymbol() : nullptr;
			if (!symbol)
				return std::nullopt;

			std::string name = symbol->GetRawName();
			return ClassNameFromObjCMethodSymbolName(name);
		}

		std::optional<Selector> SelectorFromCurrentMethod(const Call& call)
		{
			auto function = call.instr.function ? call.instr.function->GetFunction() : nullptr;
			auto symbol = function ? function->GetSymbol() : nullptr;
			if (!symbol)
				return std::nullopt;

			std::string name = symbol->GetRawName();
			std::string_view view(name);
			if (view.size() < 5 || (view.front() != '-' && view.front() != '+') || view[1] != '[' ||
			    view.back() != ']')
				return std::nullopt;

			size_t separator = view.find(' ', 2);
			if (separator == std::string_view::npos || separator + 1 >= view.size() - 1)
				return std::nullopt;

			return Selector {std::string(view.substr(separator + 1, view.size() - separator - 2)), 0};
		}

		std::optional<std::string> ReceiverClassNameForSuperCall(
		    const Call& call, const std::optional<ClassReferenceInfo>& superClassFieldClassReference)
		{
			if (auto className = ClassNameFromCurrentMethod(call))
				return className;

			if (superClassFieldClassReference && IsObjCMsgSendSuper2(call))
				return superClassFieldClassReference->className;

			return std::nullopt;
		}

		std::optional<SuperCallInfo> ResolveSuperCallInfo(const Call& call, BinaryView* view)
		{
			if (call.params.size() < 2)
				return std::nullopt;

			auto selectorAddr = MatchConstantPointerOrLoadOfConstantPointer(call.params[1]);
			if (!selectorAddr)
				return std::nullopt;

			auto selector = Selector::FromAddress(view, *selectorAddr);
			if (!selector)
				return std::nullopt;

			auto superParam = call.params[0];
			if (superParam.operation != MLIL_VAR_SSA)
				return std::nullopt;

			auto function = call.instr.function;
			size_t superDefIndex = function->GetSSAVarDefinition(superParam.GetSourceSSAVariable<MLIL_VAR_SSA>());
			if (superDefIndex == BN_INVALID_EXPR)
				return std::nullopt;

			auto superDef = function->GetInstruction(superDefIndex);
			if (superDef.operation != MLIL_SET_VAR_SSA)
				return std::nullopt;

			auto src = superDef.GetSourceExpr<MLIL_SET_VAR_SSA>();
			if (src.operation != MLIL_ADDRESS_OF)
				return std::nullopt;

			Variable objcSuperVar = src.GetSourceVariable<MLIL_ADDRESS_OF>();
			auto superClassFieldClassReference = SuperClassFieldClassReference(call, view, objcSuperVar);
			auto receiverClassName = ReceiverClassNameForSuperCall(call, superClassFieldClassReference);
			if (!receiverClassName)
				return std::nullopt;

			return SuperCallInfo {*selector, std::move(*receiverClassName)};
		}

		Ref<Type> ReturnTypeForSuperInit(const SuperCallInfo& info, const Call& call, BinaryView* view)
		{
			if (!info.selector.IsInitFamily())
				return nullptr;

			if (auto analysisInfo = GlobalState::GetAnalysisInfo(view))
				analysisInfo->EnsureClassIvarTypes(view, info.receiverClassName);

			auto classType = ClassInstanceType(view, info.receiverClassName);
			if (!classType)
				return nullptr;

			auto function = call.instr.function ? call.instr.function->GetFunction() : nullptr;
			auto arch = function ? function->GetArchitecture() : nullptr;
			if (!arch)
				return nullptr;

			return Type::PointerType(arch, classType);
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
			if (!existingType.IsUnknown() && existingType.GetValue() &&
			    (*existingType.GetValue() == *returnType || IsSameObjCClassPointer(existingType.GetValue(), returnType)))
				return false;

			function->CreateAutoVariable(var, Confidence<Ref<Type>>(returnType, confidence),
			    function->GetVariableNameOrDefault(var));
			return true;
		}

		void ApplyReturnTypeToOutputVariables(const Call& call, Type* returnType, uint8_t confidence)
		{
			auto outputs = OutputSSAVariables(call.instr);
			if (outputs.empty())
				return;

			const auto& objectOutput = outputs[0];
			auto function = call.instr.function->GetFunction();
			bool copiedToAnotherVariable = false;
			bool hasNonCopyUse = false;

			for (size_t useIndex : call.instr.function->GetSSAVarUses(objectOutput))
			{
				auto use = call.instr.function->GetInstruction(useIndex);
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
				ApplyReturnTypeToVariable(
				    function, use.GetDestSSAVariable<MLIL_SET_VAR_SSA>().var, returnType, confidence);
			}

			if (!copiedToAnotherVariable || hasNonCopyUse)
				ApplyReturnTypeToVariable(function, objectOutput.var, returnType, confidence);
		}

		void ApplyReturnTypeToCurrentInitMethod(const Call& call, Type* returnType, uint8_t confidence)
		{
			auto function = call.instr.function ? call.instr.function->GetFunction() : nullptr;
			if (!function || function->HasUserType())
				return;

			auto selector = SelectorFromCurrentMethod(call);
			if (!selector || !selector->IsInitFamily())
				return;

			auto existingReturn = function->GetReturnType();
			if (!existingReturn.IsUnknown() && existingReturn.GetValue() &&
			    (*existingReturn.GetValue() == *returnType || IsSameObjCClassPointer(existingReturn.GetValue(), returnType)))
				return;

			function->SetAutoReturnType(Confidence<Ref<Type>>(returnType, confidence));
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
				return std::nullopt;
			}
		}

		struct FieldAddress
		{
			MediumLevelILInstruction base;
			SSAVariable baseVar;
			uint64_t offset = 0;
		};

		std::optional<FieldAddress> MatchFieldAddress(const MediumLevelILInstruction& expr)
		{
			if (expr.operation != MLIL_ADD)
				return std::nullopt;

			auto left = expr.GetLeftExpr<MLIL_ADD>();
			auto right = expr.GetRightExpr<MLIL_ADD>();
			MediumLevelILInstruction base;
			std::optional<uint64_t> offset;

			if (left.operation == MLIL_VAR_SSA)
			{
				base = left;
				offset = ConstantInteger(right);
			}
			else if (right.operation == MLIL_VAR_SSA)
			{
				base = right;
				offset = ConstantInteger(left);
			}

			if (!offset)
				return std::nullopt;
			return FieldAddress {base, base.GetSourceSSAVariable<MLIL_VAR_SSA>(), *offset};
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

		bool PointeeHasStructMemberAtOffset(BinaryView* view, Type* pointerType, uint64_t offset)
		{
			if (!pointerType || !pointerType->IsPointer())
				return false;

			auto child = pointerType->GetChildType();
			if (child.IsUnknown() || !child.GetValue())
				return false;

			auto pointee = ResolveNamedType(view, child.GetValue());
			if (!pointee || !pointee->IsStructure())
				return false;

			StructureMember member;
			return pointee->GetStructure()->GetMemberAtOffset(static_cast<int64_t>(offset), member);
		}

		bool BaseVariableHasStructMemberAtOffset(
		    const FieldAddress& address, BinaryView* view, MediumLevelILFunction* mlil, uint64_t offset)
		{
			auto type = address.base.GetType();
			if (!type.IsUnknown() && PointeeHasStructMemberAtOffset(view, type.GetValue(), offset))
				return true;

			auto function = mlil ? mlil->GetFunction() : nullptr;
			if (!function)
				return false;

			type = function->GetVariableType(address.baseVar.var);
			return !type.IsUnknown() && PointeeHasStructMemberAtOffset(view, type.GetValue(), offset);
		}

		void RewriteStructStore(const MediumLevelILInstruction& instr, BinaryView* view)
		{
			if (instr.operation != MLIL_STORE_SSA)
				return;

			auto address = MatchFieldAddress(instr.GetDestExpr<MLIL_STORE_SSA>());
			if (!address || !BaseVariableHasStructMemberAtOffset(*address, view, instr.function, address->offset))
				return;

			auto base = address->base.CopyTo(instr.function);
			auto source = instr.GetSourceExpr<MLIL_STORE_SSA>().CopyTo(instr.function);
			auto replacement = instr.function->StoreStructSSA(instr.size, base, address->offset,
			    instr.GetDestMemoryVersion<MLIL_STORE_SSA>(), instr.GetSourceMemoryVersion<MLIL_STORE_SSA>(), source,
			    ILSourceLocation(instr));
			instr.function->ReplaceExpr(instr.exprIndex, replacement);
		}

		void ProcessInstruction(const MediumLevelILInstruction& instr, BinaryView* view)
		{
			auto call = MatchCallToFunctionNamed(instr, view, kObjCMsgSendSuperFunctions);
			if (!call)
				return;

			auto info = ResolveSuperCallInfo(*call, view);
			if (!info)
				return;

			auto returnType = ReturnTypeForSuperInit(*info, *call, view);
			if (!returnType)
				return;

			uint8_t confidence = static_cast<uint8_t>(ConfidenceLevel::SuperInit);
			AdjustReturnTypeOfCall(*call, returnType, confidence);
			ApplyReturnTypeToOutputVariables(*call, returnType, confidence);
			ApplyReturnTypeToCurrentInitMethod(*call, returnType, confidence);
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

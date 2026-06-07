#include "ObjCExterns.h"

#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <lowlevelilinstruction.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	namespace
	{
		enum class MessageSendType
		{
			Normal,
			Super,
		};

		struct InferredReturnType
		{
			Ref<Type> type;
			uint8_t confidence = static_cast<uint8_t>(ConfidenceLevel::ObjCMsgSend);
		};

		struct ReceiverInfo
		{
			std::string className;
			ObjCMethodKind methodKind = ObjCMethodKind::Instance;
		};

		const std::vector<std::string_view> kAllocFunctions = {
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

		std::optional<MessageSendType> CallTargetType(BinaryView* bv, uint64_t callTarget)
		{
			auto symbol = bv->GetSymbolByAddress(callTarget);
			if (!symbol)
				return std::nullopt;

			std::string name = symbol->GetRawName();
			std::string_view view(name);
			if (view.starts_with("j_"))
				view.remove_prefix(2);

			if (view == "_objc_msgSend")
				return MessageSendType::Normal;
			if (view == "_objc_msgSendSuper" || view == "_objc_msgSendSuper2")
				return MessageSendType::Super;
			return std::nullopt;
		}

		std::optional<Selector> SelectorFromCall(BinaryView* bv, LowLevelILFunction* ssa, const LowLevelILInstruction& instr)
		{
			std::vector<LowLevelILInstruction> params = instr.GetParameterExprs();
			if (!params.empty() && params[0].operation == LLIL_SEPARATE_PARAM_LIST_SSA)
				params = params[0].GetParameterExprs<LLIL_SEPARATE_PARAM_LIST_SSA>();
			if (params.size() < 2 || params[1].operation != LLIL_REG_SSA)
				return std::nullopt;

			SSARegister selReg = params[1].GetSourceSSARegister<LLIL_REG_SSA>();
			auto selectorValue = ssa->GetSSARegisterValue(selReg);
			switch (selectorValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				if (selectorValue.value == 0)
					return std::nullopt;
				return Selector::FromAddress(bv, static_cast<uint64_t>(selectorValue.value));
			default:
				return std::nullopt;
			}
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

		std::optional<std::string> ObjCClassNameFromTypeEncoding(std::string_view encoding)
		{
			if (!encoding.starts_with("@\""))
				return std::nullopt;

			size_t end = encoding.find('"', 2);
			if (end == std::string_view::npos || end == 2)
				return std::nullopt;

			std::string className(encoding.substr(2, end - 2));
			if (className.starts_with("<"))
				return std::nullopt;

			if (size_t genericStart = className.find('<'); genericStart != std::string::npos)
				className.resize(genericStart);

			if (className.empty())
				return std::nullopt;
			return className;
		}

		Ref<Type> NamedStructTypeFromEncoding(BinaryView* bv, std::string_view encoding)
		{
			if (!encoding.starts_with("{"))
				return nullptr;

			size_t end = encoding.find_first_of("=}", 1);
			if (end == std::string_view::npos || end == 1)
				return nullptr;

			std::string name(encoding.substr(1, end - 1));
			return NamedType(bv, name);
		}

		Ref<Type> TypeForPropertyEncoding(BinaryView* bv, Architecture* arch, std::string_view encoding)
		{
			while (!encoding.empty() && std::string_view("rnNoORV").find(encoding.front()) != std::string_view::npos)
				encoding.remove_prefix(1);

			if (encoding.empty())
				return nullptr;

			switch (encoding.front())
			{
			case '^':
			{
				Ref<Type> pointee = TypeForPropertyEncoding(bv, arch, encoding.substr(1));
				if (!pointee)
					pointee = Type::VoidType();
				return Type::PointerType(arch, pointee);
			}
			case '@':
			{
				if (auto className = ObjCClassNameFromTypeEncoding(encoding))
				{
					if (auto classType = NamedType(bv, *className))
						return Type::PointerType(arch, classType);
				}

				return NamedType(bv, "id");
			}
			case '#':
				if (auto classType = NamedType(bv, "objc_class_t"))
					return Type::PointerType(arch, classType);
				return nullptr;
			case ':':
				return NamedType(bv, "SEL");
			case '*':
				return Type::PointerType(arch, Type::IntegerType(1, Confidence<bool>(true)));
			case 'c':
				return Type::IntegerType(1, Confidence<bool>(true));
			case 'C':
			case 'A':
				return Type::IntegerType(1, Confidence<bool>(false));
			case 's':
				return Type::IntegerType(2, Confidence<bool>(true));
			case 'S':
				return Type::IntegerType(2, Confidence<bool>(false));
			case 'i':
			case 'l':
				return Type::IntegerType(4, Confidence<bool>(true));
			case 'I':
			case 'L':
				return Type::IntegerType(4, Confidence<bool>(false));
			case 'q':
				return Type::IntegerType(8, Confidence<bool>(true));
			case 'Q':
				return Type::IntegerType(8, Confidence<bool>(false));
			case 'f':
				return Type::FloatType(4);
			case 'd':
				return Type::FloatType(8);
			case 'B':
			case 'b':
				return Type::BoolType();
			case 'v':
				return Type::VoidType();
			case '{':
				return NamedStructTypeFromEncoding(bv, encoding);
			default:
				return nullptr;
			}
		}

		Ref<Type> ReturnTypeForPropertyGetter(BinaryView* bv, Architecture* arch, const Selector& selector)
		{
			if (selector.name.find(':') != std::string::npos)
				return nullptr;

			auto info = GlobalState::GetAnalysisInfo(bv);
			if (!info)
				return nullptr;

			auto encoding = info->GetPropertyGetterTypeEncoding(bv, selector.name);
			if (!encoding)
				return nullptr;

			return TypeForPropertyEncoding(bv, arch, *encoding);
		}

		Ref<Type> ReturnTypeForMethodEncoding(BinaryView* bv, Architecture* arch, const Selector& selector)
		{
			auto info = GlobalState::GetAnalysisInfo(bv);
			if (!info)
				return nullptr;

			auto encoding = info->GetMethodReturnTypeEncoding(bv, selector.name);
			if (!encoding)
				return nullptr;

			return TypeForPropertyEncoding(bv, arch, *encoding);
		}

		std::optional<InferredReturnType> ReturnTypeForMethodImplementation(
		    BinaryView* bv, Function* func, const ObjCMethodRequestKey& key)
		{
			auto info = GlobalState::GetAnalysisInfo(bv);
			if (!info)
				return std::nullopt;

			auto implAddress = info->GetMethodImpl(bv, key);
			if (!implAddress)
				return std::nullopt;

			auto impl = bv->GetAnalysisFunction(func->GetPlatform(), *implAddress);
			if (!impl)
				return std::nullopt;

			auto returnType = impl->GetReturnType();
			if (returnType.IsUnknown() || !returnType.GetValue() || returnType.GetValue()->IsVoid())
				return std::nullopt;

			if (IsIdType(returnType.GetValue()))
				return std::nullopt;

			return InferredReturnType {
			    returnType.GetValue(),
			    std::max(returnType.GetConfidence(), static_cast<uint8_t>(ConfidenceLevel::ObjCMsgSend))};
		}

		std::optional<InferredReturnType> ReturnTypeForTypeLibraryMethod(
		    BinaryView* bv, const ObjCMethodRequestKey& key)
		{
			auto methodType = TypeLibraryObjectType(bv, ObjCMethodSymbolName(key));
			if (!methodType || !methodType->IsFunction())
				return std::nullopt;

			auto returnType = methodType->GetReturnValue().type;
			if (returnType.IsUnknown() || !returnType.GetValue() || returnType.GetValue()->IsVoid())
				return std::nullopt;

			if (IsIdType(returnType.GetValue()))
				return std::nullopt;

			return InferredReturnType {
			    returnType.GetValue(),
			    std::max(returnType.GetConfidence(), static_cast<uint8_t>(ConfidenceLevel::ObjCMsgSend))};
		}

		bool IsAllocFunctionName(std::string_view name)
		{
			return std::find(kAllocFunctions.begin(), kAllocFunctions.end(), name) != kAllocFunctions.end();
		}

		std::optional<LowLevelILInstruction> SourceDefForRegister(LowLevelILFunction* ssa, SSARegister reg)
		{
			size_t defIndex = ssa->GetSSARegisterDefinition(reg);
			if (defIndex == BN_INVALID_EXPR)
				return std::nullopt;

			LowLevelILInstruction def = ssa->GetInstruction(defIndex);
			while (def.operation == LLIL_SET_REG_SSA)
			{
				auto src = def.GetSourceExpr<LLIL_SET_REG_SSA>();
				if (src.operation != LLIL_REG_SSA)
					break;

				defIndex = ssa->GetSSARegisterDefinition(src.GetSourceSSARegister<LLIL_REG_SSA>());
				if (defIndex == BN_INVALID_EXPR)
					return std::nullopt;
				def = ssa->GetInstruction(defIndex);
			}

			return def;
		}

		std::optional<SSARegister> CopiedParameterRegister(LowLevelILFunction* ssa, SSARegister reg)
		{
			for (size_t depth = 0; depth < 16; ++depth)
			{
				size_t defIndex = ssa->GetSSARegisterDefinition(reg);
				if (defIndex == BN_INVALID_EXPR)
					return reg;

				auto def = ssa->GetInstruction(defIndex);
				if (def.operation != LLIL_SET_REG_SSA)
					return std::nullopt;

				auto src = def.GetSourceExpr<LLIL_SET_REG_SSA>();
				if (src.operation != LLIL_REG_SSA)
					return std::nullopt;

				reg = src.GetSourceSSARegister<LLIL_REG_SSA>();
			}

			return std::nullopt;
		}

		std::optional<uint64_t> ConstantLoadedPointer(const LowLevelILInstruction& expr)
		{
			if (expr.operation != LLIL_LOAD_SSA)
				return std::nullopt;

			auto src = expr.GetSourceExpr<LLIL_LOAD_SSA>();
			auto value = src.GetPossibleValues();
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

		std::optional<uint64_t> CallTargetFromInstruction(
		    LowLevelILFunction* ssa, const LowLevelILInstruction& instr)
		{
			auto dest = instr.GetDestExpr();
			auto destValue = dest.GetPossibleValues();
			switch (destValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				return static_cast<uint64_t>(destValue.value);
			default:
				break;
			}

			if (dest.operation != LLIL_REG_SSA)
				return std::nullopt;

			auto def = SourceDefForRegister(ssa, dest.GetSourceSSARegister<LLIL_REG_SSA>());
			if (!def || def->operation != LLIL_SET_REG_SSA)
				return std::nullopt;

			auto src = def->GetSourceExpr<LLIL_SET_REG_SSA>();
			auto srcValue = src.GetPossibleValues();
			switch (srcValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				return static_cast<uint64_t>(srcValue.value);
			default:
				break;
			}

			return ConstantLoadedPointer(src);
		}

		std::optional<ReceiverInfo> ReceiverInfoFromCallReceiver(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& instr,
		    size_t depth = 0);
		std::optional<ReceiverInfo> ReceiverInfoFromExpr(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& expr, size_t depth);
		std::optional<ReceiverInfo> ReceiverInfoFromRegister(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const SSARegister& reg, size_t depth);

		std::optional<ReceiverInfo> ReceiverInfoFromObjCMethodSymbol(Function* func)
		{
			auto symbol = func ? func->GetSymbol() : nullptr;
			if (!symbol)
				return std::nullopt;

			std::string name = symbol->GetRawName();
			if (name.size() < 5 || (name[0] != '-' && name[0] != '+') || name[1] != '[' || name.back() != ']')
				return std::nullopt;

			size_t separator = name.find(' ', 2);
			if (separator == std::string::npos || separator <= 2)
				return std::nullopt;

			ReceiverInfo result;
			result.className = name.substr(2, separator - 2);
			result.methodKind = name[0] == '+' ? ObjCMethodKind::Class : ObjCMethodKind::Instance;
			return result;
		}

		std::optional<size_t> ParameterIndexForRegister(Function* func, uint32_t reg)
		{
			if (!func)
				return std::nullopt;

			auto parameterLocations = func->GetParameterLocations();
			if (!parameterLocations.IsUnknown())
			{
				auto locations = parameterLocations.GetValue();
				for (size_t i = 0; i < locations.size(); ++i)
				{
					if (locations[i].indirect || locations[i].components.size() != 1)
						continue;
					const auto& var = locations[i].components[0].variable;
					if (var.type == RegisterVariableSourceType && static_cast<uint32_t>(var.storage) == reg)
						return i;
				}
			}

			Ref<CallingConvention> cc = nullptr;
			auto confidence = func->GetCallingConvention();
			if (!confidence.IsUnknown())
				cc = confidence.GetValue();
			if (!cc && func->GetPlatform())
				cc = func->GetPlatform()->GetDefaultCallingConvention();
			if (!cc)
				return std::nullopt;

			auto registers = cc->GetIntegerArgumentRegisters();
			for (size_t i = 0; i < registers.size(); ++i)
			{
				if (registers[i] == reg)
					return i;
			}
			return std::nullopt;
		}

		std::optional<ReceiverInfo> ReceiverInfoFromParameter(Function* func, uint32_t reg)
		{
			auto index = ParameterIndexForRegister(func, reg);
			if (!index)
				return std::nullopt;

			if (*index == 0)
			{
				if (auto method = ReceiverInfoFromObjCMethodSymbol(func))
					return method;
			}

			auto functionType = func->GetType();
			if (!functionType || !functionType->IsFunction())
				return std::nullopt;

			auto params = functionType->GetParameters();
			if (*index >= params.size())
				return std::nullopt;

			auto paramType = params[*index].type.GetValue();
			if (auto className = ClassNameFromType(paramType))
				return ReceiverInfo {*className, ObjCMethodKind::Instance};
			return std::nullopt;
		}

		std::optional<ReceiverInfo> ReceiverInfoFromClassAddress(BinaryView* bv, uint64_t address)
		{
			auto classSymbol = bv->GetSymbolByAddress(address);
			if (!classSymbol)
				return std::nullopt;

			std::string classSymbolName = classSymbol->GetFullName();
			auto className = ClassNameFromSymbolName(classSymbolName);
			if (!className)
				return std::nullopt;

			return ReceiverInfo {std::string(*className), ObjCMethodKind::Class};
		}

		Ref<Type> ReturnTypeForCallTarget(BinaryView* bv, Function* func, LowLevelILFunction* ssa,
		    const LowLevelILInstruction& instr)
		{
			auto arch = func->GetArchitecture();
			if (auto adjustment = func->GetCallTypeAdjustment(arch, instr.address); !adjustment.IsUnknown())
			{
				if (auto type = adjustment.GetValue())
					return type->GetReturnValue().type.GetValue();
			}

			auto callTarget = CallTargetFromInstruction(ssa, instr);
			if (!callTarget)
				return nullptr;

			if (auto targetFunction = bv->GetAnalysisFunction(func->GetPlatform(), *callTarget))
			{
				auto returnType = targetFunction->GetReturnType();
				if (!returnType.IsUnknown())
					return returnType.GetValue();
			}

			DataVariable dataVariable;
			if (bv->GetDataVariableAtAddress(*callTarget, dataVariable))
			{
				auto dataType = dataVariable.type.GetValue();
				if (dataType && dataType->IsFunction())
					return dataType->GetReturnValue().type.GetValue();
			}

			return nullptr;
		}

		std::optional<ReceiverInfo> ReceiverInfoFromMessageSendResult(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& instr, size_t depth)
		{
			auto callTarget = CallTargetFromInstruction(ssa, instr);
			if (!callTarget)
				return std::nullopt;

			auto messageSendType = CallTargetType(bv, *callTarget);
			if (!messageSendType || *messageSendType != MessageSendType::Normal)
				return std::nullopt;

			auto selector = SelectorFromCall(bv, ssa, instr);
			if (!selector)
				return std::nullopt;

			auto receiver = ReceiverInfoFromCallReceiver(bv, func, ssa, instr, depth + 1);
			if (!receiver)
				return std::nullopt;

			if (IsAllocLikeSelector(selector->name))
				return ReceiverInfo {receiver->className, ObjCMethodKind::Instance};

			if (selector->IsInitFamily() && receiver->methodKind == ObjCMethodKind::Instance)
				return receiver;

			return std::nullopt;
		}

		std::optional<ReceiverInfo> ReceiverInfoFromAllocFunctionResult(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& instr, size_t depth)
		{
			auto callTarget = CallTargetFromInstruction(ssa, instr);
			if (!callTarget)
				return std::nullopt;

			auto targetSymbol = bv->GetSymbolByAddress(*callTarget);
			if (!targetSymbol || !IsAllocFunctionName(targetSymbol->GetRawName()))
				return std::nullopt;

			std::vector<LowLevelILInstruction> params = instr.GetParameterExprs();
			if (!params.empty() && params[0].operation == LLIL_SEPARATE_PARAM_LIST_SSA)
				params = params[0].GetParameterExprs<LLIL_SEPARATE_PARAM_LIST_SSA>();
			if (params.empty())
				return std::nullopt;

			auto receiver = ReceiverInfoFromExpr(bv, func, ssa, params[0], depth + 1);
			if (!receiver || receiver->methodKind != ObjCMethodKind::Class)
				return std::nullopt;

			return ReceiverInfo {receiver->className, ObjCMethodKind::Instance};
		}

		std::optional<uint64_t> ConstantOffsetFromExpr(const LowLevelILInstruction& expr)
		{
			if (expr.operation == LLIL_CONST || expr.operation == LLIL_CONST_PTR)
				return static_cast<uint64_t>(expr.GetConstant());

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

		struct RegisterOffset
		{
			SSARegister reg;
			uint64_t offset = 0;
		};

		std::optional<RegisterOffset> RegisterOffsetFromExpr(const LowLevelILInstruction& expr)
		{
			if (expr.operation == LLIL_REG_SSA)
				return RegisterOffset {expr.GetSourceSSARegister<LLIL_REG_SSA>(), 0};

			if (expr.operation != LLIL_ADD)
				return std::nullopt;

			auto left = expr.GetLeftExpr();
			auto right = expr.GetRightExpr();
			if (auto leftBase = RegisterOffsetFromExpr(left))
			{
				if (auto rightOffset = ConstantOffsetFromExpr(right))
				{
					leftBase->offset += *rightOffset;
					return leftBase;
				}
			}

			if (auto rightBase = RegisterOffsetFromExpr(right))
			{
				if (auto leftOffset = ConstantOffsetFromExpr(left))
				{
					rightBase->offset += *leftOffset;
					return rightBase;
				}
			}

			return std::nullopt;
		}

		std::optional<ReceiverInfo> ReceiverInfoFromIvarLoad(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& expr, size_t depth)
		{
			if (expr.operation != LLIL_LOAD_SSA)
				return std::nullopt;

			auto address = expr.GetSourceExpr<LLIL_LOAD_SSA>();
			auto regOffset = RegisterOffsetFromExpr(address);
			if (!regOffset || regOffset->offset == 0)
				return std::nullopt;

			auto baseReceiver = ReceiverInfoFromRegister(bv, func, ssa, regOffset->reg, depth + 1);
			if (!baseReceiver || baseReceiver->methodKind != ObjCMethodKind::Instance)
				return std::nullopt;

			auto info = GlobalState::GetAnalysisInfo(bv);
			if (!info)
				return std::nullopt;

			auto ivarClassName = info->GetIvarClassName(bv, baseReceiver->className, regOffset->offset);
			if (!ivarClassName)
				return std::nullopt;

			return ReceiverInfo {*ivarClassName, ObjCMethodKind::Instance};
		}

		std::optional<ReceiverInfo> ReceiverInfoFromRegister(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const SSARegister& reg, size_t depth)
		{
			auto regValue = ssa->GetSSARegisterValue(reg);
			switch (regValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				if (auto receiver = ReceiverInfoFromClassAddress(bv, static_cast<uint64_t>(regValue.value)))
					return receiver;
				break;
			default:
				break;
			}

			auto def = SourceDefForRegister(ssa, reg);
			if (!def)
			{
				if (auto parameterReg = CopiedParameterRegister(ssa, reg))
				{
					if (auto receiver = ReceiverInfoFromParameter(func, parameterReg->reg))
						return receiver;
				}

				return ReceiverInfoFromParameter(func, reg.reg);
			}

			if (def->operation == LLIL_CALL_SSA || def->operation == LLIL_TAILCALL_SSA)
			{
				if (Ref<Type> returnType = ReturnTypeForCallTarget(bv, func, ssa, *def))
				{
					if (auto className = ClassNameFromType(returnType))
						return ReceiverInfo {*className, ObjCMethodKind::Instance};
				}

				if (auto receiver = ReceiverInfoFromAllocFunctionResult(bv, func, ssa, *def, depth))
					return receiver;

				return ReceiverInfoFromMessageSendResult(bv, func, ssa, *def, depth);
			}

			if (def->operation != LLIL_SET_REG_SSA)
				return std::nullopt;

			auto src = def->GetSourceExpr<LLIL_SET_REG_SSA>();
			if (auto receiver = ReceiverInfoFromIvarLoad(bv, func, ssa, src, depth + 1))
				return receiver;

			auto srcValue = src.GetPossibleValues();
			switch (srcValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				if (auto receiver = ReceiverInfoFromClassAddress(bv, static_cast<uint64_t>(srcValue.value)))
					return receiver;
				break;
			default:
				break;
			}

			if (auto classRef = ConstantLoadedPointer(src))
				return ReceiverInfoFromClassAddress(bv, *classRef);

			return std::nullopt;
		}

		std::optional<ReceiverInfo> ReceiverInfoFromExpr(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& expr, size_t depth)
		{
			if (depth > 8)
				return std::nullopt;

			if (auto receiver = ReceiverInfoFromIvarLoad(bv, func, ssa, expr, depth + 1))
				return receiver;

			auto exprValue = expr.GetPossibleValues();
			switch (exprValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				if (auto receiver = ReceiverInfoFromClassAddress(bv, static_cast<uint64_t>(exprValue.value)))
					return receiver;
				break;
			default:
				break;
			}

			if (auto classRef = ConstantLoadedPointer(expr))
				return ReceiverInfoFromClassAddress(bv, *classRef);

			if (expr.operation == LLIL_REG_SSA)
				return ReceiverInfoFromRegister(bv, func, ssa, expr.GetSourceSSARegister<LLIL_REG_SSA>(), depth + 1);

			return std::nullopt;
		}

		std::optional<ReceiverInfo> ReceiverInfoFromCallReceiver(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& instr, size_t depth)
		{
			std::vector<LowLevelILInstruction> params = instr.GetParameterExprs();
			if (!params.empty() && params[0].operation == LLIL_SEPARATE_PARAM_LIST_SSA)
				params = params[0].GetParameterExprs<LLIL_SEPARATE_PARAM_LIST_SSA>();
			if (params.empty())
				return std::nullopt;

			return ReceiverInfoFromExpr(bv, func, ssa, params[0], depth + 1);
		}

		Ref<Type> ReturnTypeForInitReceiver(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& instr,
		    const Selector& selector, MessageSendType messageSendType)
		{
			if (!selector.IsInitFamily())
				return nullptr;

			if (messageSendType == MessageSendType::Super)
			{
				auto symbol = func ? func->GetSymbol() : nullptr;
				if (!symbol)
					return nullptr;

				std::string symbolName = symbol->GetRawName();
				auto className = ClassNameFromObjCMethodSymbolName(symbolName);
				if (!className)
					return nullptr;

				if (auto info = GlobalState::GetAnalysisInfo(bv))
					info->EnsureClassIvarTypes(bv, *className);

				auto classType = ClassInstanceType(bv, *className);
				if (!classType)
					return nullptr;

				return Type::PointerType(func->GetArchitecture(), classType);
			}

			if (messageSendType != MessageSendType::Normal)
				return nullptr;

			std::vector<LowLevelILInstruction> params = instr.GetParameterExprs();
			if (!params.empty() && params[0].operation == LLIL_SEPARATE_PARAM_LIST_SSA)
				params = params[0].GetParameterExprs<LLIL_SEPARATE_PARAM_LIST_SSA>();
			if (params.empty() || params[0].operation != LLIL_REG_SSA)
				return nullptr;

			auto def = SourceDefForRegister(ssa, params[0].GetSourceSSARegister<LLIL_REG_SSA>());
			if (!def || (def->operation != LLIL_CALL_SSA && def->operation != LLIL_TAILCALL_SSA))
				return nullptr;

			auto callTargetValue = def->GetDestExpr().GetPossibleValues();
			uint64_t callTarget = 0;
			switch (callTargetValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				callTarget = static_cast<uint64_t>(callTargetValue.value);
				break;
			default:
				return nullptr;
			}

			auto targetSymbol = bv->GetSymbolByAddress(callTarget);
			if (!targetSymbol || !IsAllocFunctionName(targetSymbol->GetRawName()))
				return nullptr;

			std::vector<LowLevelILInstruction> allocParams = def->GetParameterExprs();
			if (!allocParams.empty() && allocParams[0].operation == LLIL_SEPARATE_PARAM_LIST_SSA)
				allocParams = allocParams[0].GetParameterExprs<LLIL_SEPARATE_PARAM_LIST_SSA>();
			if (allocParams.empty() || allocParams[0].operation != LLIL_REG_SSA)
				return nullptr;

			auto classRegisterValue = ssa->GetSSARegisterValue(allocParams[0].GetSourceSSARegister<LLIL_REG_SSA>());
			uint64_t classValue = 0;
			switch (classRegisterValue.state)
			{
			case ConstantValue:
			case ConstantPointerValue:
			case ImportedAddressValue:
				classValue = static_cast<uint64_t>(classRegisterValue.value);
				break;
			default:
				return nullptr;
			}

			if (classValue == 0)
				return nullptr;

			auto classSymbol = bv->GetSymbolByAddress(classValue);
			if (!classSymbol)
				return nullptr;

			std::string classSymbolName = classSymbol->GetFullName();
			auto className = ClassNameFromSymbolName(classSymbolName);
			if (!className)
				return nullptr;

			auto classType = NamedType(bv, *className);
			if (!classType)
				return nullptr;

			return Type::PointerType(func->GetArchitecture(), classType);
		}

		std::vector<SSARegister> OutputSSARegisters(const LowLevelILInstruction& instr)
		{
			switch (instr.operation)
			{
			case LLIL_CALL_SSA:
				return instr.GetOutputSSARegisters<LLIL_CALL_SSA>();
			case LLIL_TAILCALL_SSA:
				return instr.GetOutputSSARegisters<LLIL_TAILCALL_SSA>();
			default:
				return {};
			}
		}

		ReturnValue ReturnValueForCall(const LowLevelILInstruction& instr, const InferredReturnType& inferredReturn)
		{
			Confidence<Ref<Type>> type(inferredReturn.type, inferredReturn.confidence);
			auto outputs = OutputSSARegisters(instr);
			if (outputs.empty())
				return ReturnValue(type);

			ValueLocation location(Variable::Register(outputs[0].reg));
			return ReturnValue(type, false, Confidence<ValueLocation>(location, inferredReturn.confidence));
		}

		bool IsEquivalentObjCType(Type* existingType, Type* newType)
		{
			if (!existingType || !newType)
				return false;
			if (*existingType == *newType)
				return true;

			if (!existingType->IsPointer() || !newType->IsPointer())
				return false;

			auto existingClassName = ClassNameFromType(existingType);
			auto newClassName = ClassNameFromType(newType);
			return existingClassName && newClassName && *existingClassName == *newClassName;
		}

		bool IsEquivalentTypeConfidence(Confidence<Ref<Type>> existingType, Confidence<Ref<Type>> newType)
		{
			if (existingType.IsUnknown() || newType.IsUnknown())
				return false;
			return IsEquivalentObjCType(existingType.GetValue(), newType.GetValue());
		}

		bool IsEquivalentObjCMsgSendFunctionType(Type* existingType, Type* newType)
		{
			if (!existingType || !newType || !existingType->IsFunction() || !newType->IsFunction())
				return false;

			if (!IsEquivalentTypeConfidence(existingType->GetReturnValue().type, newType->GetReturnValue().type))
				return false;

			auto existingParams = existingType->GetParameters();
			auto newParams = newType->GetParameters();
			if (existingParams.size() != newParams.size())
				return false;

			for (size_t i = 0; i < existingParams.size(); ++i)
			{
				if (!IsEquivalentTypeConfidence(existingParams[i].type, newParams[i].type))
					return false;
			}

			return true;
		}

		bool AdjustCallType(
		    BinaryView* bv, Function* func, LowLevelILFunction* ssa, const LowLevelILInstruction& instr,
		    const Selector& selector, MessageSendType messageSendType)
		{
			uint8_t confidence = static_cast<uint8_t>(ConfidenceLevel::ObjCMsgSend);
			auto arch = func->GetArchitecture();
			Ref<CallingConvention> callingConvention = nullptr;
			auto currentCallingConvention = func->GetCallingConvention();
			if (!currentCallingConvention.IsUnknown())
				callingConvention = currentCallingConvention.GetValue();
			if (!callingConvention && func->GetPlatform())
				callingConvention = func->GetPlatform()->GetDefaultCallingConvention();

			Ref<Type> id = NamedType(bv, "id");
			if (!id)
				id = Type::PointerType(arch, Type::VoidType());

			Ref<Type> receiverType;
			std::string receiverName;
			if (messageSendType == MessageSendType::Normal)
			{
				receiverType = id;
				receiverName = "self";
			}
			else
			{
				Ref<Type> objcSuper = NamedType(bv, "objc_super");
				if (!objcSuper)
					objcSuper = Type::VoidType();
				receiverType = Type::PointerType(arch, objcSuper);
				receiverName = "super";
			}

			Ref<Type> sel = NamedType(bv, "SEL");
			if (!sel)
				sel = Type::PointerType(arch, Type::IntegerType(1, Confidence<bool>(true)));

			std::optional<ReceiverInfo> receiver;
			if (messageSendType == MessageSendType::Normal)
				receiver = ReceiverInfoFromCallReceiver(bv, func, ssa, instr);

			InferredReturnType inferredReturn {id, confidence};
			if (Ref<Type> initReturnType = ReturnTypeForInitReceiver(bv, func, ssa, instr, selector, messageSendType))
				inferredReturn = {initReturnType, confidence};
			else
			{
				std::optional<InferredReturnType> implReturnType;
				std::optional<InferredReturnType> typeLibraryReturnType;
				if (receiver)
				{
					ObjCMethodRequestKey key {receiver->className, selector.name, receiver->methodKind};
					implReturnType = ReturnTypeForMethodImplementation(bv, func, key);
					if (!implReturnType)
						typeLibraryReturnType = ReturnTypeForTypeLibraryMethod(bv, key);
				}

				if (implReturnType)
					inferredReturn = *implReturnType;
				else if (typeLibraryReturnType)
					inferredReturn = *typeLibraryReturnType;
				else if (Ref<Type> propertyReturnType = ReturnTypeForPropertyGetter(bv, arch, selector))
					inferredReturn = {propertyReturnType, confidence};
				else if (Ref<Type> methodReturnType = ReturnTypeForMethodEncoding(bv, arch, selector))
					inferredReturn = {methodReturnType, confidence};
			}

			std::vector<FunctionParameter> params;
			params.emplace_back(receiverName, Confidence<Ref<Type>>(receiverType, confidence));
			params.emplace_back("sel", Confidence<Ref<Type>>(sel, confidence));

			auto labels = selector.ArgumentLabels();
			auto argumentNames = GenerateArgumentNames(labels);
			for (size_t i = argumentNames.size(); i < labels.size(); ++i)
				argumentNames.push_back("arg" + std::to_string(i));

			Ref<Type> argType = Type::IntegerType(bv->GetAddressSize(), Confidence<bool>(true));
			for (const auto& name : argumentNames)
				params.emplace_back(name, Confidence<Ref<Type>>(argType, confidence));

			auto functionType = Type::FunctionType(
			    ReturnValueForCall(instr, inferredReturn),
			    Confidence<Ref<CallingConvention>>(callingConvention, callingConvention ? confidence : 0), params,
			    Confidence<bool>(false));

			auto existingAdjustment = func->GetCallTypeAdjustment(arch, instr.address);
			if (!existingAdjustment.IsUnknown() && existingAdjustment.GetValue() &&
			    IsEquivalentObjCMsgSendFunctionType(existingAdjustment.GetValue(), functionType) &&
			    existingAdjustment.GetConfidence() >= inferredReturn.confidence)
			{
				return false;
			}

			func->SetAutoCallTypeAdjustment(
			    arch, instr.address, Confidence<Ref<Type>>(functionType, inferredReturn.confidence));
			return true;
		}

		bool ReplaceCallTarget(
		    BinaryView* bv, LowLevelILFunction* llil, const LowLevelILInstruction& instr, uint64_t target)
		{
			auto nonSSAInstr = instr.GetNonSSAForm();
			if (nonSSAInstr.operation != LLIL_CALL && nonSSAInstr.operation != LLIL_TAILCALL)
			{
				LogError("Unexpected LLIL operation for objc_msgSend call at 0x%llx",
				    static_cast<unsigned long long>(instr.address));
				return false;
			}

			auto destExpr = nonSSAInstr.GetDestExpr();
			llil->SetCurrentAddress(llil->GetArchitecture(), nonSSAInstr.address);
			llil->ReplaceExpr(
			    destExpr.exprIndex, llil->ConstPointer(bv->GetAddressSize(), target, ILSourceLocation(nonSSAInstr)));
			return true;
		}

		bool RewriteToDirectCall(
		    BinaryView* bv, Function* func, LowLevelILFunction* llil, LowLevelILFunction* ssa,
		    const LowLevelILInstruction& instr,
		    const Selector& selector, MessageSendType messageSendType)
		{
			if (messageSendType == MessageSendType::Super)
				return false;

			auto info = GlobalState::GetAnalysisInfo(bv);
			if (!info)
				return false;

			auto receiver = ReceiverInfoFromCallReceiver(bv, func, ssa, instr);
			if (!receiver)
				return false;

			ObjCMethodRequestKey key {receiver->className, selector.name, receiver->methodKind};
			if (auto externAddress = ObjCExternMethodAddress(bv, key))
				return ReplaceCallTarget(bv, llil, instr, *externAddress);

			if (info->shouldRewriteToDirectCalls)
			{
				if (auto implAddress = info->GetMethodImpl(bv, key))
					return ReplaceCallTarget(bv, llil, instr, *implAddress);
			}

			auto resolution = ResolveMethodDispatch(bv, *info, key);
			if (!resolution)
				return false;

			if (resolution->externAddress)
				return ReplaceCallTarget(bv, llil, instr, *resolution->externAddress);
			if (info->shouldRewriteToDirectCalls && resolution->implAddress)
				return ReplaceCallTarget(bv, llil, instr, *resolution->implAddress);

			if (!resolution->implAddress && !info->HasClass(bv, resolution->key.className))
				GlobalState::AddObjCExternRequest(bv, resolution->key, func);

			return false;
		}

		bool ProcessInstruction(
		    BinaryView* bv, Function* func, LowLevelILFunction* llil, LowLevelILFunction* ssa,
		    const LowLevelILInstruction& instr)
		{
			if (instr.operation != LLIL_CALL_SSA && instr.operation != LLIL_TAILCALL_SSA)
				return false;

			auto callTarget = CallTargetFromInstruction(ssa, instr);
			if (!callTarget)
				return false;

			auto messageSendType = CallTargetType(bv, *callTarget);
			if (!messageSendType)
				return false;

			auto selector = SelectorFromCall(bv, ssa, instr);
			if (!selector)
				return false;

			bool changed = AdjustCallType(bv, func, ssa, instr, *selector, *messageSendType);
			changed |= RewriteToDirectCall(bv, func, llil, ssa, instr, *selector, *messageSendType);
			return changed;
		}
	}

	void ProcessObjCMsgSendCalls(Ref<AnalysisContext> ac)
	{
		auto view = ac->GetBinaryView();
		if (GlobalState::ShouldIgnoreView(view))
			return;

		auto func = ac->GetFunction();
		auto llil = ac->GetLowLevelILFunction();
		if (!func || !llil)
			return;

		auto ssa = llil->GetSSAForm();
		if (!ssa)
			return;

		bool functionChanged = false;
		for (const auto& block : ssa->GetBasicBlocks())
		{
			for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
				functionChanged |= ProcessInstruction(view, func, llil, ssa, ssa->GetInstruction(i));
		}

		if (functionChanged)
			llil->GenerateSSAForm();
	}

}

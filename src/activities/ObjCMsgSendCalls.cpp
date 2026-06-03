#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <lowlevelilinstruction.h>
#include <mediumlevelilinstruction.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
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

		struct MethodDispatchResolution
		{
			ObjCMethodRequestKey key;
			std::optional<uint64_t> implAddress;
			std::optional<uint64_t> externAddress;
		};

		struct ObjCExternLayoutEntry
		{
			ObjCExternRequest request;
			std::optional<std::string> libraryName;
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

		const std::vector<std::string_view> kObjCMsgSendFunctions = {
			"_objc_msgSend",
			"j__objc_msgSend",
		};

		std::optional<uint64_t> ConstantLikeValue(const PossibleValueSet& value)
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

		std::optional<uint64_t> ConstantLikeValue(const RegisterValue& value)
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

		std::vector<LowLevelILInstruction> CallParamExprs(const LowLevelILInstruction& instr)
		{
			std::vector<LowLevelILInstruction> params = instr.GetParameterExprs();
			if (!params.empty() && params[0].operation == LLIL_SEPARATE_PARAM_LIST_SSA)
				return params[0].GetParameterExprs<LLIL_SEPARATE_PARAM_LIST_SSA>();
			return params;
		}

		std::optional<Selector> SelectorFromCall(BinaryView* bv, LowLevelILFunction* ssa, const LowLevelILInstruction& instr)
		{
			auto params = CallParamExprs(instr);
			if (params.size() < 2 || params[1].operation != LLIL_REG_SSA)
				return std::nullopt;

			SSARegister selReg = params[1].GetSourceSSARegister<LLIL_REG_SSA>();
			auto selectorValue = ConstantLikeValue(ssa->GetSSARegisterValue(selReg));
			if (!selectorValue || *selectorValue == 0)
				return std::nullopt;

			return Selector::FromAddress(bv, *selectorValue);
		}

		Ref<Type> PointerToType(Architecture* arch, Ref<Type> type)
		{
			if (!type)
				return nullptr;
			return Type::PointerType(arch, type);
		}

		bool IsGenericObjCTypeName(std::string_view name)
		{
			return name == "id" || name == "Class" || name == "SEL" || name == "objc_object" ||
			    name == "objc_class_t" || name == "objc_super";
		}

		void NormalizeClassTypeName(std::string& name)
		{
			if (name.starts_with("struct "))
				name.erase(0, 7);
		}

		std::optional<std::string> ClassNameFromType(Type* type)
		{
			if (!type)
				return std::nullopt;

			if (type->IsPointer())
			{
				auto childType = type->GetChildType();
				if (childType.IsUnknown() || !childType.GetValue())
					return std::nullopt;
				return ClassNameFromType(childType.GetValue());
			}

			std::string name;
			if (type->IsNamedTypeRefer())
			{
				auto ref = type->GetNamedTypeReference();
				if (!ref)
					return std::nullopt;
				name = ref->GetName().GetString();
			}
			else if (type->IsStructure())
			{
				if (auto registeredName = type->GetRegisteredName())
					name = registeredName->GetName().GetString();
				if (name.empty())
					name = type->GetStructureName().GetString();
			}
			else
			{
				name = type->GetTypeName().GetString();
			}
			NormalizeClassTypeName(name);

			if (name.empty() || IsGenericObjCTypeName(name))
				return std::nullopt;
			return name;
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
						return PointerToType(arch, classType);
				}

				return NamedType(bv, "id");
			}
			case '#':
				return PointerToType(arch, NamedType(bv, "objc_class_t"));
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

		bool IsAllocLikeSelector(std::string_view name)
		{
			return name == "alloc" || name == "new" || name.starts_with("allocWith") || name.starts_with("newWith");
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
			return ConstantLikeValue(src.GetPossibleValues());
		}

		std::optional<uint64_t> CallTargetFromInstruction(
		    LowLevelILFunction* ssa, const LowLevelILInstruction& instr)
		{
			auto dest = instr.GetDestExpr();
			if (auto value = ConstantLikeValue(dest.GetPossibleValues()))
				return value;

			if (dest.operation != LLIL_REG_SSA)
				return std::nullopt;

			auto def = SourceDefForRegister(ssa, dest.GetSourceSSARegister<LLIL_REG_SSA>());
			if (!def || def->operation != LLIL_SET_REG_SSA)
				return std::nullopt;

			auto src = def->GetSourceExpr<LLIL_SET_REG_SSA>();
			if (auto value = ConstantLikeValue(src.GetPossibleValues()))
				return value;

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

			auto params = CallParamExprs(instr);
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
			return ConstantLikeValue(expr.GetPossibleValues());
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
			if (auto classValue = ConstantLikeValue(ssa->GetSSARegisterValue(reg)))
			{
				if (auto receiver = ReceiverInfoFromClassAddress(bv, *classValue))
					return receiver;
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

			if (auto classValue = ConstantLikeValue(src.GetPossibleValues()))
			{
				if (auto receiver = ReceiverInfoFromClassAddress(bv, *classValue))
					return receiver;
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

			if (auto classValue = ConstantLikeValue(expr.GetPossibleValues()))
			{
				if (auto receiver = ReceiverInfoFromClassAddress(bv, *classValue))
					return receiver;
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
			auto params = CallParamExprs(instr);
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

			auto params = CallParamExprs(instr);
			if (params.empty() || params[0].operation != LLIL_REG_SSA)
				return nullptr;

			auto def = SourceDefForRegister(ssa, params[0].GetSourceSSARegister<LLIL_REG_SSA>());
			if (!def || (def->operation != LLIL_CALL_SSA && def->operation != LLIL_TAILCALL_SSA))
				return nullptr;

			auto callTarget = ConstantLikeValue(def->GetDestExpr().GetPossibleValues());
			if (!callTarget)
				return nullptr;

			auto targetSymbol = bv->GetSymbolByAddress(*callTarget);
			if (!targetSymbol || !IsAllocFunctionName(targetSymbol->GetRawName()))
				return nullptr;

			auto allocParams = CallParamExprs(*def);
			if (allocParams.empty() || allocParams[0].operation != LLIL_REG_SSA)
				return nullptr;

			auto classValue = ConstantLikeValue(
			    ssa->GetSSARegisterValue(allocParams[0].GetSourceSSARegister<LLIL_REG_SSA>()));
			if (!classValue || *classValue == 0)
				return nullptr;

			auto classSymbol = bv->GetSymbolByAddress(*classValue);
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

		std::optional<std::string> SelectorLabelWithoutPrefix(std::string_view prefix, std::string_view label)
		{
			if (label.size() <= prefix.size() || !label.starts_with(prefix))
				return std::nullopt;

			std::string afterPrefix(label.substr(prefix.size()));
			if (afterPrefix.empty() || std::islower(static_cast<unsigned char>(afterPrefix[0])))
				return std::nullopt;

			if (afterPrefix.size() >= 2 && std::islower(static_cast<unsigned char>(afterPrefix[1])))
				afterPrefix[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(afterPrefix[0])));

			return afterPrefix;
		}

		std::string ArgumentNameFromSelectorLabel(const std::string& label)
		{
			static constexpr std::string_view prefixes[] = {
				"initWith", "with", "and", "using", "set", "read", "to", "for",
			};

			for (auto prefix : prefixes)
			{
				if (auto name = SelectorLabelWithoutPrefix(prefix, label))
					return *name;
			}

			return label;
		}

		std::vector<std::string> GenerateArgumentNames(const std::vector<std::string>& labels)
		{
			std::vector<std::string> result;
			result.reserve(labels.size());
			for (const auto& label : labels)
				result.push_back(ArgumentNameFromSelectorLabel(label));
			return result;
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

		std::optional<uint64_t> ObjCExternMethodAddress(BinaryView* bv, const ObjCMethodRequestKey& key)
		{
			std::string name = ObjCMethodSymbolName(key);
			for (const auto& nameSpace : {BinaryView::GetInternalNameSpace(), BinaryView::GetExternalNameSpace()})
			{
				auto symbol = bv->GetSymbolByRawName(name, nameSpace);
				if (symbol && symbol->GetAddress() != 0 && bv->IsOffsetExternSemantics(symbol->GetAddress()))
					return symbol->GetAddress();
			}

			return std::nullopt;
		}

		std::optional<std::string> ExternalLibraryNameFromMetadata(BinaryView* bv, std::string_view symbolName)
		{
			if (!bv || symbolName.empty())
				return std::nullopt;

			auto mapping = bv->QueryMetadata("SymbolExternalLibraryMapping");
			if (!mapping || mapping->GetType() != KeyValueDataType)
				return std::nullopt;

			auto value = mapping->Get(std::string(symbolName));
			if (!value || value->GetType() != StringDataType)
				return std::nullopt;

			std::string libraryName = value->GetString();
			if (libraryName.empty())
				return std::nullopt;
			return libraryName;
		}

		std::optional<std::string> ExternalLibraryNameForSymbol(BinaryView* bv, Symbol* symbol)
		{
			if (!bv || !symbol)
				return std::nullopt;

			if (auto location = bv->GetExternalLocation(symbol))
			{
				if (auto library = location->GetExternalLibrary())
				{
					std::string libraryName = library->GetName();
					if (!libraryName.empty())
						return libraryName;
				}
			}

			for (const auto& name : {symbol->GetRawName(), symbol->GetFullName(), symbol->GetShortName()})
			{
				if (auto libraryName = ExternalLibraryNameFromMetadata(bv, name))
					return libraryName;
			}

			return std::nullopt;
		}

		std::optional<std::string> ExternalLibraryNameForObjCClass(BinaryView* bv, const std::string& className)
		{
			if (!bv || className.empty())
				return std::nullopt;

			std::string symbolName = "_OBJC_CLASS_$_" + className;
			for (const auto& nameSpace : {BinaryView::GetInternalNameSpace(), BinaryView::GetExternalNameSpace()})
			{
				auto symbol = bv->GetSymbolByRawName(symbolName, nameSpace);
				if (auto libraryName = ExternalLibraryNameForSymbol(bv, symbol))
					return libraryName;
			}

			return ExternalLibraryNameFromMetadata(bv, symbolName);
		}

		int ObjCMethodKindSortValue(ObjCMethodKind methodKind)
		{
			return methodKind == ObjCMethodKind::Class ? 0 : 1;
		}

		std::vector<ObjCExternLayoutEntry> ObjCExternLayoutEntries(
		    BinaryView* bv, std::vector<ObjCExternRequest> requests)
		{
			std::vector<ObjCExternLayoutEntry> entries;
			entries.reserve(requests.size());
			for (auto& request : requests)
			{
				auto libraryName = ExternalLibraryNameForObjCClass(bv, request.key.className);
				entries.push_back(ObjCExternLayoutEntry {
				    std::move(request), std::move(libraryName)});
			}

			std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
				if (lhs.libraryName.has_value() != rhs.libraryName.has_value())
					return lhs.libraryName.has_value();
				if (lhs.libraryName != rhs.libraryName)
					return lhs.libraryName < rhs.libraryName;
				if (lhs.request.key.className != rhs.request.key.className)
					return lhs.request.key.className < rhs.request.key.className;
				if (lhs.request.key.methodKind != rhs.request.key.methodKind)
					return ObjCMethodKindSortValue(lhs.request.key.methodKind) <
					    ObjCMethodKindSortValue(rhs.request.key.methodKind);
				if (lhs.request.key.selectorName != rhs.request.key.selectorName)
					return lhs.request.key.selectorName < rhs.request.key.selectorName;
				return ObjCMethodSymbolName(lhs.request.key) < ObjCMethodSymbolName(rhs.request.key);
			});

			return entries;
		}

		Ref<ExternalLibrary> ObjCExternExternalLibrary(
		    BinaryView* bv, const std::optional<std::string>& libraryName)
		{
			if (!bv || !libraryName || libraryName->empty())
				return nullptr;

			auto library = bv->GetExternalLibrary(*libraryName);
			if (!library)
				library = bv->AddExternalLibrary(*libraryName, {}, true);
			return library;
		}

		void RecordObjCExternLibraryMapping(
		    BinaryView* bv, std::string_view symbolName, const std::optional<std::string>& libraryName)
		{
			if (!bv || symbolName.empty() || !libraryName || libraryName->empty())
				return;

			std::map<std::string, Ref<Metadata>> mapping;
			auto existingMapping = bv->QueryMetadata("SymbolExternalLibraryMapping");
			if (existingMapping && existingMapping->GetType() == KeyValueDataType)
				mapping = existingMapping->GetKeyValueStore();

			mapping[std::string(symbolName)] = new Metadata(*libraryName);
			bv->StoreMetadata("SymbolExternalLibraryMapping", new Metadata(mapping), true);
		}

		void RecordObjCExternExternalLocation(
		    BinaryView* bv, std::string_view symbolName, const std::optional<std::string>& libraryName)
		{
			auto library = ObjCExternExternalLibrary(bv, libraryName);
			if (!bv || symbolName.empty() || !library)
				return;

			auto symbol = bv->GetSymbolByRawName(std::string(symbolName), BinaryView::GetInternalNameSpace());
			if (!symbol)
				return;

			std::string targetSymbol(symbolName);
			if (auto location = bv->GetExternalLocation(symbol))
			{
				location->SetExternalLibrary(library);
				location->SetTargetSymbol(targetSymbol);
				return;
			}

			bv->AddExternalLocation(symbol, library, targetSymbol, std::nullopt, true);
		}

		std::optional<ObjCMethodRequestKey> SuperclassMethodKey(BinaryView* bv, const ObjCMethodRequestKey& key)
		{
			if (key.methodKind != ObjCMethodKind::Instance)
				return std::nullopt;

			auto superclassName = SuperclassNameFromClassName(bv, key.className);
			if (!superclassName || superclassName->empty() || *superclassName == key.className)
				return std::nullopt;

			return ObjCMethodRequestKey {*superclassName, key.selectorName, key.methodKind};
		}

		std::optional<MethodDispatchResolution> ResolveMethodDispatch(
		    BinaryView* bv, const AnalysisInfo& info, const ObjCMethodRequestKey& key)
		{
			ObjCMethodRequestKey current = key;
			for (size_t depth = 0; depth < 32; ++depth)
			{
				if (auto implAddress = info.GetMethodImpl(bv, current))
					return MethodDispatchResolution {current, implAddress, std::nullopt};
				if (auto externAddress = ObjCExternMethodAddress(bv, current))
					return MethodDispatchResolution {current, std::nullopt, externAddress};
				if (!info.HasClass(bv, current.className))
					return MethodDispatchResolution {current, std::nullopt, std::nullopt};

				if (auto superclassKey = SuperclassMethodKey(bv, current))
				{
					current = std::move(*superclassKey);
					continue;
				}

				return std::nullopt;
			}

			return std::nullopt;
		}

		Ref<Type> ReceiverTypeForObjCExtern(BinaryView* bv, Architecture* arch, const ObjCMethodRequestKey& key)
		{
			if (key.methodKind == ObjCMethodKind::Instance)
			{
				if (auto classType = NamedType(bv, key.className))
					return PointerToType(arch, classType);
			}
			else
			{
				if (auto classType = NamedType(bv, "Class"))
					return classType;
				if (auto objcClassType = NamedType(bv, "objc_class_t"))
					return PointerToType(arch, objcClassType);
			}

			if (auto idType = NamedType(bv, "id"))
				return idType;
			return Type::PointerType(arch, Type::VoidType());
		}

		Ref<Type> ReturnTypeForObjCExtern(BinaryView* bv, Architecture* arch, const ObjCMethodRequestKey& key)
		{
			Selector selector {key.selectorName, 0};
			if (selector.IsInitFamily() ||
			    (key.methodKind == ObjCMethodKind::Class && IsAllocLikeSelector(key.selectorName)))
			{
				if (auto classType = NamedType(bv, key.className))
					return PointerToType(arch, classType);
			}

			if (auto idType = NamedType(bv, "id"))
				return idType;
			return Type::PointerType(arch, Type::VoidType());
		}

		Ref<Type> FunctionTypeForObjCExtern(BinaryView* bv, const ObjCMethodRequestKey& key)
		{
			auto arch = bv->GetDefaultArchitecture();
			if (!arch)
				return nullptr;

			uint8_t confidence = static_cast<uint8_t>(ConfidenceLevel::ObjCMsgSend);
			Ref<Type> sel = NamedType(bv, "SEL");
			if (!sel)
				sel = Type::PointerType(arch, Type::IntegerType(1, Confidence<bool>(true)));

			std::vector<FunctionParameter> params;
			params.emplace_back("self", Confidence<Ref<Type>>(ReceiverTypeForObjCExtern(bv, arch, key), confidence));
			params.emplace_back("sel", Confidence<Ref<Type>>(sel, confidence));

			Selector selector {key.selectorName, 0};
			auto labels = selector.ArgumentLabels();
			auto argumentNames = GenerateArgumentNames(labels);
			Ref<Type> argType = Type::IntegerType(bv->GetAddressSize(), Confidence<bool>(true));
			for (const auto& name : argumentNames)
				params.emplace_back(name, Confidence<Ref<Type>>(argType, confidence));

			return Type::FunctionType(
			    ReturnValue(Confidence<Ref<Type>>(ReturnTypeForObjCExtern(bv, arch, key), confidence)),
			    Confidence<Ref<CallingConvention>>(nullptr, 0), params, Confidence<bool>(false));
		}

		uint64_t AlignUp(uint64_t value, uint64_t alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		uint64_t ObjCExternSectionStart(BinaryView* bv, uint64_t length)
		{
			if (auto section = bv->GetSectionByName(".objc-externs"))
			{
				if (section->GetEnd() - section->GetStart() < length)
					bv->AddAutoSection(".objc-externs", section->GetStart(), length, ExternalSectionSemantics);
				return section->GetStart();
			}

			uint64_t end = bv->GetEnd();
			for (const auto& range : bv->GetMappedAddressRanges())
				end = std::max(end, range.end);

			uint64_t start = AlignUp(std::max<uint64_t>(end, 0x1000), 0x1000);
			bv->AddAutoSegment(start, length, 0, 0, SegmentReadable | SegmentContainsData | SegmentDenyWrite);
			bv->AddAutoSection(".objc-externs", start, length, ExternalSectionSemantics);
			return start;
		}

		bool DefineObjCExternMethod(
		    BinaryView* bv, const ObjCMethodRequestKey& key, uint64_t address,
		    const std::optional<std::string>& libraryName)
		{
			if (ObjCExternMethodAddress(bv, key))
				return false;

			std::string name = ObjCMethodSymbolName(key);
			auto externType = TypeLibraryObjectType(bv, name, address);
			if (!externType)
				externType = FunctionTypeForObjCExtern(bv, key);
			if (!externType)
				return false;

			auto symbol = new Symbol(
			    SymbolicFunctionSymbol, name, name, name, address, GlobalBinding, BinaryView::GetInternalNameSpace());
			bv->DefineAutoSymbol(symbol);
			bv->DefineDataVariable(address, Confidence<Ref<Type>>(externType, BN_FULL_CONFIDENCE));
			RecordObjCExternLibraryMapping(bv, name, libraryName);
			RecordObjCExternExternalLocation(bv, name, libraryName);
			return true;
		}

		std::optional<uint64_t> ConstantPointerFromMLILExpr(const MediumLevelILInstruction& expr)
		{
			if (auto value = MatchConstantPointerOrLoadOfConstantPointer(expr))
				return value;

			if (expr.operation == MLIL_CONST)
				return static_cast<uint64_t>(expr.GetConstant<MLIL_CONST>());
			if (expr.operation == MLIL_CONST_PTR)
				return static_cast<uint64_t>(expr.GetConstant<MLIL_CONST_PTR>());

			if (expr.operation == MLIL_VAR_SSA && expr.function)
			{
				auto var = expr.GetSourceSSAVariable<MLIL_VAR_SSA>();
				return ConstantLikeValue(expr.function->GetSSAVarValue(var));
			}

			return std::nullopt;
		}

		std::optional<Selector> SelectorFromMLILCall(BinaryView* bv, const Call& call)
		{
			if (call.params.size() < 2)
				return std::nullopt;

			auto selectorValue = ConstantPointerFromMLILExpr(call.params[1]);
			if (!selectorValue || *selectorValue == 0)
				return std::nullopt;

			return Selector::FromAddress(bv, *selectorValue);
		}

		std::optional<ReceiverInfo> TypedReceiverFromMLILCall(const Call& call)
		{
			if (call.params.empty())
				return std::nullopt;

			auto type = call.params[0].GetType();
			if (type.IsUnknown() || !type.GetValue())
				return std::nullopt;

			if (auto className = ClassNameFromType(type.GetValue()))
				return ReceiverInfo {*className, ObjCMethodKind::Instance};

			return std::nullopt;
		}

		bool DiscoverTypedObjCExtern(BinaryView* bv, const MediumLevelILInstruction& instr)
		{
			auto call = MatchCallToFunctionNamed(instr, bv, kObjCMsgSendFunctions);
			if (!call)
				return false;

			auto selector = SelectorFromMLILCall(bv, *call);
			if (!selector)
				return false;

			auto receiver = TypedReceiverFromMLILCall(*call);
			if (!receiver)
				return false;

			auto info = GlobalState::GetAnalysisInfo(bv);
			if (!info)
				return false;

			auto function = instr.function ? instr.function->GetFunction() : nullptr;
			if (!function)
				return false;

			ObjCMethodRequestKey key {receiver->className, selector->name, receiver->methodKind};
			auto resolution = ResolveMethodDispatch(bv, *info, key);
			if (!resolution)
				return false;
			if (resolution->implAddress || resolution->externAddress || info->HasClass(bv, resolution->key.className))
				return false;

			GlobalState::AddObjCExternRequest(bv, resolution->key, function);
			return true;
		}

		std::unordered_set<uint64_t> MaterializeObjCExterns(BinaryView* bv, std::vector<ObjCExternRequest> requests)
		{
			std::unordered_set<uint64_t> functionsToReanalyze;
			if (requests.empty())
				return functionsToReanalyze;

			auto entries = ObjCExternLayoutEntries(bv, std::move(requests));

			uint64_t slotSize = std::max<uint64_t>(bv->GetAddressSize(), 1);
			uint64_t start = ObjCExternSectionStart(bv, entries.size() * slotSize);
			uint64_t address = start;

			for (const auto& entry : entries)
			{
				const auto& request = entry.request;
				if (ObjCExternMethodAddress(bv, request.key))
					continue;

				while (bv->GetSymbolByAddress(address))
					address += slotSize;

				if (!DefineObjCExternMethod(bv, request.key, address, entry.libraryName))
					continue;

				address += slotSize;
				functionsToReanalyze.insert(request.functionStarts.begin(), request.functionStarts.end());
			}

			return functionsToReanalyze;
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

	void ProcessObjCExterns(Ref<AnalysisContext> ac)
	{
		auto view = ac->GetBinaryView();
		if (GlobalState::ShouldIgnoreView(view))
			return;

		auto requests = GlobalState::DrainObjCExternRequests(view);
		auto functionsToReanalyze = MaterializeObjCExterns(view, requests);
		for (uint64_t functionStart : functionsToReanalyze)
		{
			for (auto& func : view->GetAnalysisFunctionsForAddress(functionStart))
				func->MarkUpdatesRequired(FullAutoFunctionUpdate);
		}
	}

	void ProcessDiscoverTypedObjCExterns(Ref<AnalysisContext> ac)
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
				DiscoverTypedObjCExtern(view, mlilSSA->GetInstruction(i));
		}
	}
}

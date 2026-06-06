#include "Metadata.h"
#include "Util.h"
#include "Workflow.h"

#include <binaryninjaapi.h>
#include <highlevelilinstruction.h>

#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace BinaryNinja;

namespace WorkflowObjC
{
	namespace
	{
		struct CallOperands
		{
			HighLevelILInstruction dest;
			std::vector<HighLevelILInstruction> params;
		};

		struct CallTargetInfo
		{
			uint64_t address = 0;
			Ref<Symbol> symbol;
		};

		struct RuntimeMsgSendInfo
		{
			std::string name;
			bool isSuper = false;
			bool usesSuperclassOfSuperClass = false;
		};

		struct SelectorInfo
		{
			std::string name;
			uint64_t address = 0;
			bool hasAddress = false;
		};

		struct ClassReferenceInfo
		{
			uint64_t address = 0;
			std::string className;
		};

		struct MsgSendResolution
		{
			size_t exprIndex = BN_INVALID_EXPR;
			uint64_t sourceAddress = 0;
			std::string callName;
			SelectorInfo selector;
			std::optional<uint64_t> targetAddress;
			std::string targetName;
			std::string unresolvedReason;
		};

		std::string Hex(uint64_t value)
		{
			std::ostringstream out;
			out << "0x" << std::hex << value;
			return out.str();
		}

		std::string SymbolDisplayName(Symbol* symbol)
		{
			if (!symbol)
				return {};

			std::string name = symbol->GetShortName();
			if (!name.empty())
				return name;

			name = symbol->GetFullName();
			if (!name.empty())
				return name;

			return symbol->GetRawName();
		}

		std::string AddressDisplayName(BinaryView* bv, uint64_t address)
		{
			if (auto symbol = bv->GetSymbolByAddress(address))
			{
				std::string name = SymbolDisplayName(symbol);
				if (!name.empty())
					return name;
			}

			return Hex(address);
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

		std::optional<uint64_t> ConstantLikeValue(const HighLevelILInstruction& expr)
		{
			switch (expr.operation)
			{
			case HLIL_CONST:
				return static_cast<uint64_t>(expr.GetConstant<HLIL_CONST>());
			case HLIL_CONST_PTR:
				return static_cast<uint64_t>(expr.GetConstant<HLIL_CONST_PTR>());
			case HLIL_IMPORT:
				return static_cast<uint64_t>(expr.GetConstant<HLIL_IMPORT>());
			case HLIL_EXTERN_PTR:
				return static_cast<uint64_t>(expr.GetConstant<HLIL_EXTERN_PTR>() + expr.GetOffset<HLIL_EXTERN_PTR>());
			default:
				return ConstantLikeValue(expr.GetValue());
			}
		}

		std::optional<CallOperands> GetCallOperands(const HighLevelILInstruction& instr)
		{
			switch (instr.operation)
			{
			case HLIL_CALL:
			case HLIL_TAILCALL:
			case HLIL_CALL_SSA:
				return CallOperands {instr.GetDestExpr(), instr.GetParameterExprs()};
			default:
				return std::nullopt;
			}
		}

		std::optional<CallTargetInfo> GetCallTargetInfo(BinaryView* bv, const HighLevelILInstruction& callTarget)
		{
			auto address = ConstantLikeValue(callTarget);
			if (!address)
				return std::nullopt;

			auto symbol = bv->GetSymbolByAddress(*address);
			if (!symbol)
				return std::nullopt;

			if (callTarget.operation == HLIL_IMPORT)
			{
				auto symbolType = symbol->GetType();
				if (symbolType == ImportedDataSymbol || symbolType == ImportAddressSymbol)
					symbol = Symbol::ImportedFunctionFromImportAddressSymbol(symbol, *address);
			}

			return CallTargetInfo {*address, symbol};
		}

		std::optional<RuntimeMsgSendInfo> RuntimeMsgSendInfoFromSymbolName(std::string_view name)
		{
			if (name.starts_with("j_"))
				name.remove_prefix(2);

			if (name == "_objc_msgSend")
				return RuntimeMsgSendInfo {"objc_msgSend", false, false};
			if (name == "_objc_msgSendSuper")
				return RuntimeMsgSendInfo {"objc_msgSendSuper", true, false};
			if (name == "_objc_msgSendSuper2")
				return RuntimeMsgSendInfo {"objc_msgSendSuper2", true, true};

			return std::nullopt;
		}

		bool IsObjCMethodSymbolName(std::string_view name)
		{
			return name.size() >= 6 && (name.front() == '-' || name.front() == '+') && name[1] == '[' &&
			    name.back() == ']' && name.find(' ') != std::string_view::npos;
		}

		std::optional<std::string> SelectorNameFromObjCMethodSymbol(std::string_view name)
		{
			size_t space = name.find(' ');
			size_t end = name.rfind(']');
			if (space == std::string_view::npos || end == std::string_view::npos || space + 1 >= end)
				return std::nullopt;

			return std::string(name.substr(space + 1, end - space - 1));
		}

		SelectorInfo SelectorFromParameters(BinaryView* bv, const std::vector<HighLevelILInstruction>& params)
		{
			if (params.size() < 2)
				return {"<selector unavailable>", 0, false};

			auto selectorAddress = ConstantLikeValue(params[1]);
			if (!selectorAddress || *selectorAddress == 0)
				return {"<selector unavailable>", 0, false};

			if (auto selector = Selector::FromAddress(bv, *selectorAddress))
				return {selector->name, selector->addr, true};

			return {Hex(*selectorAddress), *selectorAddress, true};
		}

		std::optional<Variable> VariableFromExpr(const HighLevelILInstruction& expr)
		{
			if (expr.operation == HLIL_VAR)
				return expr.GetVariable<HLIL_VAR>();
			if (expr.operation == HLIL_VAR_SSA)
				return expr.GetSSAVariable<HLIL_VAR_SSA>().var;
			return std::nullopt;
		}

		bool IsGenericObjCTypeName(std::string_view name)
		{
			return name == "id" || name == "Class" || name == "SEL" || name == "objc_object" ||
			    name == "objc_class" || name == "objc_class_t" || name == "objc_super";
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

		std::optional<ObjCMethodRequestKey> MethodKeyFromCurrentMethodSelf(
		    const HighLevelILInstruction& instr, const HighLevelILInstruction& receiver, std::string_view selectorName)
		{
			auto receiverVar = VariableFromExpr(receiver);
			if (!receiverVar)
				return std::nullopt;

			auto function = instr.function ? instr.function->GetFunction() : nullptr;
			if (!function || function->GetVariableNameOrDefault(*receiverVar) != "self")
				return std::nullopt;

			auto symbol = function->GetSymbol();
			if (!symbol)
				return std::nullopt;

			std::string symbolName = symbol->GetRawName();
			auto className = ClassNameFromObjCMethodSymbolName(symbolName);
			if (!className)
				return std::nullopt;

			ObjCMethodKind methodKind = symbolName.front() == '+' ? ObjCMethodKind::Class : ObjCMethodKind::Instance;
			return ObjCMethodRequestKey {*className, std::string(selectorName), methodKind};
		}

		std::optional<ObjCMethodRequestKey> MethodKeyFromReceiverParameter(
		    const HighLevelILInstruction& instr, BinaryView* bv, const HighLevelILInstruction& receiver,
		    std::string_view selectorName)
		{
			if (selectorName.empty())
				return std::nullopt;

			if (auto address = ConstantLikeValue(receiver))
			{
				if (auto className = ClassNameFromClassReferenceAddress(bv, *address))
					return ObjCMethodRequestKey {*className, std::string(selectorName), ObjCMethodKind::Class};
			}

			if (auto selfKey = MethodKeyFromCurrentMethodSelf(instr, receiver, selectorName))
				return selfKey;

			if (auto receiverVar = VariableFromExpr(receiver))
			{
				auto function = instr.function ? instr.function->GetFunction() : nullptr;
				if (function)
				{
					auto variableType = function->GetVariableType(*receiverVar);
					if (!variableType.IsUnknown())
					{
						if (auto className = ClassNameFromType(variableType.GetValue()))
							return ObjCMethodRequestKey {
							    *className, std::string(selectorName), ObjCMethodKind::Instance};
					}
				}
			}

			auto type = receiver.GetType();
			if (type.IsUnknown() || !type.GetValue())
				return std::nullopt;
			if (auto className = ClassNameFromType(type.GetValue()))
				return ObjCMethodRequestKey {*className, std::string(selectorName), ObjCMethodKind::Instance};

			return std::nullopt;
		}

		std::optional<Variable> ObjCSuperVariableFromParameter(const HighLevelILInstruction& param)
		{
			if (param.operation != HLIL_ADDRESS_OF)
				return std::nullopt;

			return VariableFromExpr(param.GetSourceExpr<HLIL_ADDRESS_OF>());
		}

		bool IsSuperClassFieldOfVariable(const HighLevelILInstruction& expr, const Variable& var, uint64_t pointerSize)
		{
			if (expr.operation != HLIL_STRUCT_FIELD)
				return false;
			if (expr.GetOffset<HLIL_STRUCT_FIELD>() != pointerSize)
				return false;

			auto sourceVariable = VariableFromExpr(expr.GetSourceExpr<HLIL_STRUCT_FIELD>());
			return sourceVariable && *sourceVariable == var;
		}

		struct AssignmentExprs
		{
			HighLevelILInstruction dest;
			HighLevelILInstruction source;
		};

		std::optional<AssignmentExprs> AssignmentFromInstruction(const HighLevelILInstruction& instr)
		{
			if (instr.operation == HLIL_ASSIGN)
				return AssignmentExprs {instr.GetDestExpr<HLIL_ASSIGN>(), instr.GetSourceExpr<HLIL_ASSIGN>()};
			if (instr.operation == HLIL_ASSIGN_MEM_SSA)
				return AssignmentExprs {instr.GetDestExpr<HLIL_ASSIGN_MEM_SSA>(), instr.GetSourceExpr<HLIL_ASSIGN_MEM_SSA>()};
			return std::nullopt;
		}

		std::optional<ClassReferenceInfo> ClassReferenceFromAddress(BinaryView* bv, uint64_t address)
		{
			auto classObjectAddress = ClassObjectAddressFromClassReferenceAddress(bv, address);
			auto className = ClassNameFromClassReferenceAddress(bv, address);
			if (!className)
				return std::nullopt;
			return ClassReferenceInfo {classObjectAddress.value_or(0), std::move(*className)};
		}

		std::optional<ClassReferenceInfo> SuperClassFieldClassReference(
		    HighLevelILFunction* hlil, BinaryView* bv, uint64_t callAddress, const Variable& objcSuperVar)
		{
			std::optional<ClassReferenceInfo> classReference;
			for (size_t i = 0; i < hlil->GetInstructionCount(); ++i)
			{
				auto instr = hlil->GetInstruction(i);
				if (instr.address > callAddress)
					continue;

				auto assignment = AssignmentFromInstruction(instr);
				if (!assignment)
					continue;
				if (!IsSuperClassFieldOfVariable(assignment->dest, objcSuperVar, bv->GetAddressSize()))
					continue;

				auto address = ConstantLikeValue(assignment->source);
				if (!address)
					continue;

				auto candidate = ClassReferenceFromAddress(bv, *address);
				if (!candidate)
					continue;
				if (classReference &&
				    (classReference->address != candidate->address || classReference->className != candidate->className))
					return std::nullopt;
				classReference = std::move(*candidate);
			}

			return classReference;
		}

		std::optional<std::string> ClassNameFromCurrentMethod(const HighLevelILInstruction& instr)
		{
			auto function = instr.function ? instr.function->GetFunction() : nullptr;
			auto symbol = function ? function->GetSymbol() : nullptr;
			if (!symbol)
				return std::nullopt;

			std::string name = symbol->GetRawName();
			return ClassNameFromObjCMethodSymbolName(name);
		}

		std::optional<std::string> DispatchClassNameFromCurrentMethod(
		    const HighLevelILInstruction& instr, BinaryView* bv)
		{
			auto className = ClassNameFromCurrentMethod(instr);
			if (!className)
				return std::nullopt;

			return SuperclassNameFromClassName(bv, *className);
		}

		std::optional<std::string> SuperDispatchClassName(const HighLevelILInstruction& instr, BinaryView* bv,
		    const std::vector<HighLevelILInstruction>& params, const RuntimeMsgSendInfo& runtime)
		{
			if (params.empty() || !instr.function)
				return std::nullopt;

			auto objcSuperVar = ObjCSuperVariableFromParameter(params[0]);
			if (objcSuperVar)
			{
				auto classReference = SuperClassFieldClassReference(instr.function, bv, instr.address, *objcSuperVar);
				if (classReference)
				{
					if (!runtime.usesSuperclassOfSuperClass)
						return classReference->className;
					if (classReference->address != 0)
					{
						if (auto className = SuperclassNameFromClassObjectAddress(bv, classReference->address))
							return className;
					}
					if (auto className = SuperclassNameFromClassName(bv, classReference->className))
						return className;
				}
			}

			return DispatchClassNameFromCurrentMethod(instr, bv);
		}

		bool ResolveSuperMsgSendTarget(const HighLevelILInstruction& instr, BinaryView* bv,
		    const std::vector<HighLevelILInstruction>& params, const RuntimeMsgSendInfo& runtime,
		    MsgSendResolution& result)
		{
			if (!result.selector.hasAddress)
			{
				result.unresolvedReason = "selector unavailable";
				return false;
			}

			auto className = SuperDispatchClassName(instr, bv, params, runtime);
			if (!className)
			{
				result.unresolvedReason = "superclass unavailable";
				return false;
			}

			ObjCMethodRequestKey key {*className, result.selector.name, ObjCMethodKind::Instance};
			result.targetName = ObjCMethodSymbolName(key);

			if (auto symbol = bv->GetSymbolByRawName(result.targetName, BinaryView::GetInternalNameSpace()))
			{
				result.targetAddress = symbol->GetAddress();
				result.targetName = AddressDisplayName(bv, symbol->GetAddress());
			}

			return true;
		}

		std::optional<uint64_t> ObjCExternMethodAddress(BinaryView* bv, const ObjCMethodRequestKey& key)
		{
			if (!bv)
				return std::nullopt;

			std::string name = ObjCMethodSymbolName(key);
			for (const auto& nameSpace : {BinaryView::GetInternalNameSpace(), BinaryView::GetExternalNameSpace()})
			{
				auto symbol = bv->GetSymbolByRawName(name, nameSpace);
				if (symbol && symbol->GetAddress() != 0 && bv->IsOffsetExternSemantics(symbol->GetAddress()))
					return symbol->GetAddress();
			}

			return std::nullopt;
		}

		struct MethodDispatchResolution
		{
			ObjCMethodRequestKey key;
			std::optional<uint64_t> implAddress;
			std::optional<uint64_t> externAddress;
		};

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

		std::optional<MsgSendResolution> ResolveMsgSendCall(const HighLevelILInstruction& instr)
		{
			auto operands = GetCallOperands(instr);
			if (!operands)
				return std::nullopt;

			auto function = instr.function ? instr.function->GetFunction() : nullptr;
			if (!function)
				return std::nullopt;

			auto bv = function->GetView();
			if (!bv)
				return std::nullopt;

			auto target = GetCallTargetInfo(bv, operands->dest);
			if (!target)
				return std::nullopt;

			std::string targetSymbolName = SymbolDisplayName(target->symbol);
			bool isSuper = false;
			bool isRewritten = false;
			std::string callName;
			std::optional<RuntimeMsgSendInfo> runtime;

			runtime = RuntimeMsgSendInfoFromSymbolName(targetSymbolName);
			if (runtime)
			{
				callName = runtime->name;
				isSuper = runtime->isSuper;
			}
			else if (IsObjCMethodSymbolName(targetSymbolName))
			{
				callName = "objc_msgSend";
				isRewritten = true;
			}
			else
			{
				return std::nullopt;
			}

			MsgSendResolution result;
			result.exprIndex = instr.exprIndex;
			result.sourceAddress = instr.address;
			result.callName = std::move(callName);
			result.selector = SelectorFromParameters(bv, operands->params);

			if (!result.selector.hasAddress && isRewritten)
			{
				if (auto selectorName = SelectorNameFromObjCMethodSymbol(targetSymbolName))
					result.selector.name = *selectorName;
			}

			if (isRewritten)
			{
				result.targetAddress = target->address;
				result.targetName = targetSymbolName.empty() ? Hex(target->address) : targetSymbolName;
				return result;
			}

			if (isSuper)
			{
				ResolveSuperMsgSendTarget(instr, bv, operands->params, *runtime, result);
				return result;
			}

			if (!result.selector.hasAddress)
			{
				result.unresolvedReason = "selector unavailable";
				return result;
			}

			auto info = GlobalState::GetAnalysisInfo(bv);
			if (!info)
			{
				result.unresolvedReason = "metadata unavailable";
				return result;
			}

			if (operands->params.empty())
			{
				result.unresolvedReason = "receiver unavailable";
				return result;
			}

			auto key = MethodKeyFromReceiverParameter(instr, bv, operands->params[0], result.selector.name);
			if (!key)
			{
				result.unresolvedReason = "receiver type unavailable";
				return result;
			}

			auto dispatch = ResolveMethodDispatch(bv, *info, *key);
			if (!dispatch)
			{
				result.unresolvedReason = "no implementation found";
				return result;
			}
			if (dispatch->externAddress)
			{
				result.targetAddress = *dispatch->externAddress;
				result.targetName = AddressDisplayName(bv, *dispatch->externAddress);
				return result;
			}
			if (!dispatch->implAddress)
			{
				result.targetName = ObjCMethodSymbolName(dispatch->key);
				return result;
			}

			result.targetAddress = *dispatch->implAddress;
			result.targetName = AddressDisplayName(bv, *dispatch->implAddress);
			return result;
		}

		bool IsScopeContainerInstruction(const HighLevelILInstruction& instr)
		{
			switch (instr.operation)
			{
			case HLIL_BLOCK:
			case HLIL_IF:
			case HLIL_WHILE:
			case HLIL_DO_WHILE:
			case HLIL_FOR:
			case HLIL_SWITCH:
			case HLIL_CASE:
			case HLIL_WHILE_SSA:
			case HLIL_DO_WHILE_SSA:
			case HLIL_FOR_SSA:
				return true;
			default:
				return false;
			}
		}

		void AddIfMsgSend(const HighLevelILInstruction& instr, std::set<size_t>& visitedExprs,
		    std::set<size_t>& emittedCalls, std::vector<MsgSendResolution>& results,
		    std::optional<uint64_t> requiredAddress = std::nullopt)
		{
			if (instr.exprIndex == BN_INVALID_EXPR)
				return;
			if (requiredAddress && instr.address != *requiredAddress)
				return;
			if (!visitedExprs.insert(instr.exprIndex).second)
				return;

			auto resolution = ResolveMsgSendCall(instr);
			if (!resolution)
				return;

			if (!emittedCalls.insert(resolution->exprIndex).second)
				return;

			results.push_back(std::move(*resolution));
		}

		void AddExprTree(
		    HighLevelILFunction* hlil, size_t exprIndex, std::set<size_t>& visitedExprs,
		    std::set<size_t>& emittedCalls, std::vector<MsgSendResolution>& results,
		    std::optional<uint64_t> requiredAddress = std::nullopt)
		{
			if (exprIndex == BN_INVALID_EXPR || exprIndex >= hlil->GetExprCount())
				return;

			auto expr = hlil->GetExpr(exprIndex, true);
			expr.VisitExprs([&](const HighLevelILInstruction& subExpr) {
				AddIfMsgSend(subExpr, visitedExprs, emittedCalls, results, requiredAddress);
				return true;
			});
		}

		void AddExprAndParents(
		    HighLevelILFunction* hlil, size_t exprIndex, std::set<size_t>& visitedExprs,
		    std::set<size_t>& emittedCalls, std::vector<MsgSendResolution>& results)
		{
			if (exprIndex == BN_INVALID_EXPR || exprIndex >= hlil->GetExprCount())
				return;

			auto expr = hlil->GetExpr(exprIndex, true);
			for (size_t depth = 0; depth < 64; ++depth)
			{
				AddIfMsgSend(expr, visitedExprs, emittedCalls, results);

				if (!expr.HasParent())
					return;

				auto parent = expr.GetParent();
				if (parent.exprIndex == expr.exprIndex || parent.exprIndex == BN_INVALID_EXPR)
					return;

				expr = parent;
			}
		}

		std::vector<MsgSendResolution> FindLineResolutions(
		    HighLevelILFunction* hlil, const DisassemblyTextLine& line, std::set<size_t>& emittedCalls)
		{
			std::vector<MsgSendResolution> results;
			std::set<size_t> visitedExprs;

			if (line.instrIndex != BN_INVALID_EXPR && line.instrIndex < hlil->GetInstructionCount())
			{
				auto instr = hlil->GetInstruction(line.instrIndex);
				AddExprTree(hlil, instr.exprIndex, visitedExprs, emittedCalls, results,
				    IsScopeContainerInstruction(instr) ? std::make_optional(line.addr) : std::nullopt);
			}

			for (const auto& token : line.tokens)
				AddExprAndParents(hlil, token.exprIndex, visitedExprs, emittedCalls, results);

			return results;
		}

		void AppendResolution(DisassemblyTextLine& line, const MsgSendResolution& resolution, bool first)
		{
			line.tokens.emplace_back(AnnotationToken, first ? " " : " | ");
			line.tokens.emplace_back(AnnotationToken, "-> ");
			if (resolution.targetAddress || !resolution.targetName.empty())
			{
				if (resolution.targetAddress)
				{
					line.tokens.emplace_back(
					    CodeSymbolToken, NoTokenContext, resolution.targetName, resolution.sourceAddress,
					    *resolution.targetAddress);
				}
				else
				{
					line.tokens.emplace_back(AnnotationToken, resolution.targetName);
				}

				if (resolution.targetAddress && resolution.targetName != Hex(*resolution.targetAddress))
					line.tokens.emplace_back(AnnotationToken, " (" + Hex(*resolution.targetAddress) + ")");
			}
			else
			{
				std::string text = "unresolved";
				if (!resolution.unresolvedReason.empty())
					text += " (" + resolution.unresolvedReason + ")";
				line.tokens.emplace_back(AnnotationToken, text);
			}
		}

		void AnnotateDisassemblyLines(HighLevelILFunction* hlil, std::vector<DisassemblyTextLine>& lines)
		{
			if (!hlil)
				return;

			std::set<size_t> emittedCalls;
			for (auto& line : lines)
			{
				auto resolutions = FindLineResolutions(hlil, line, emittedCalls);
				for (size_t i = 0; i < resolutions.size(); ++i)
					AppendResolution(line, resolutions[i], i == 0);
			}
		}

		void AnnotateLinearLines(HighLevelILFunction* hlil, std::vector<LinearDisassemblyLine>& lines)
		{
			if (!hlil)
				return;

			std::set<size_t> emittedCalls;
			for (auto& line : lines)
			{
				if (line.type != CodeDisassemblyLineType)
					continue;

				auto resolutions = FindLineResolutions(hlil, line.contents, emittedCalls);
				for (size_t i = 0; i < resolutions.size(); ++i)
					AppendResolution(line.contents, resolutions[i], i == 0);
			}
		}

		class MsgSendResolutionRenderLayer : public RenderLayer
		{
		public:
			MsgSendResolutionRenderLayer() : RenderLayer("Obj-C Message Send Resolution") {}

			void ApplyToHighLevelILBlock(Ref<BasicBlock> block, std::vector<DisassemblyTextLine>& lines) override
			{
				if (!block)
					return;

				AnnotateDisassemblyLines(block->GetHighLevelILFunction(), lines);
			}

			void ApplyToHighLevelILBody(Ref<Function> function, std::vector<LinearDisassemblyLine>& lines) override
			{
				if (!function)
					return;

				AnnotateLinearLines(function->GetHighLevelILIfAvailable(), lines);
			}

			void ApplyToLinearViewObject(
			    Ref<LinearViewObject> obj, Ref<LinearViewObject> prev, Ref<LinearViewObject> next,
			    std::vector<LinearDisassemblyLine>& lines) override
			{
				if (!lines.empty() && obj)
				{
					const auto name = obj->GetIdentifier().name;
					if (name == "HLIL Function Body" || name == "HLIL SSA Function Body" ||
					    name == "Language Representation Function Body")
					{
						auto function = lines[0].function;
						auto hlil = function ? function->GetHighLevelILIfAvailable() : nullptr;
						if (hlil && name == "HLIL SSA Function Body")
							hlil = hlil->GetSSAForm();

						AnnotateLinearLines(hlil, lines);
						return;
					}
				}

				RenderLayer::ApplyToLinearViewObject(obj, prev, next, lines);
			}
		};
	}

	void RegisterRenderLayers()
	{
		static MsgSendResolutionRenderLayer* layer = nullptr;
		if (layer)
			return;

		layer = new MsgSendResolutionRenderLayer();
		RenderLayer::Register(layer, DisabledByDefaultRenderLayerDefaultEnableState);
	}
}

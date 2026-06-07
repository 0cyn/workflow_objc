#include "ObjCExterns.h"

#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

#include <algorithm>
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
		struct ReceiverInfo
		{
			std::string className;
			ObjCMethodKind methodKind = ObjCMethodKind::Instance;
		};

		struct ObjCExternLayoutEntry
		{
			ObjCExternRequest request;
			std::optional<std::string> libraryName;
		};

		const std::vector<std::string_view> kObjCMsgSendFunctions = {
			"_objc_msgSend",
			"j__objc_msgSend",
		};

		std::optional<std::string> ExternalLibraryNameForObjCClass(BinaryView* bv, const std::string& className)
		{
			if (!bv || className.empty())
				return std::nullopt;

			std::string symbolName = "_OBJC_CLASS_$_" + className;
			auto mapping = bv->QueryMetadata("SymbolExternalLibraryMapping");
			auto metadataLibraryName = [&mapping](std::string_view name) -> std::optional<std::string> {
				if (name.empty() || !mapping || mapping->GetType() != KeyValueDataType)
					return std::nullopt;

				auto value = mapping->Get(std::string(name));
				if (!value || value->GetType() != StringDataType)
					return std::nullopt;

				std::string libraryName = value->GetString();
				if (libraryName.empty())
					return std::nullopt;
				return libraryName;
			};

			for (const auto& nameSpace : {BinaryView::GetInternalNameSpace(), BinaryView::GetExternalNameSpace()})
			{
				auto symbol = bv->GetSymbolByRawName(symbolName, nameSpace);
				if (!symbol)
					continue;

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
					if (auto libraryName = metadataLibraryName(name))
						return libraryName;
				}
			}

			return metadataLibraryName(symbolName);
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
			if (!bv || symbolName.empty() || !libraryName || libraryName->empty())
				return;

			auto library = bv->GetExternalLibrary(*libraryName);
			if (!library)
				library = bv->AddExternalLibrary(*libraryName, {}, true);

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

		Ref<Type> ReceiverTypeForObjCExtern(BinaryView* bv, Architecture* arch, const ObjCMethodRequestKey& key)
		{
			if (key.methodKind == ObjCMethodKind::Instance)
			{
				if (auto classType = NamedType(bv, key.className))
					return Type::PointerType(arch, classType);
			}
			else
			{
				if (auto classType = NamedType(bv, "Class"))
					return classType;
				if (auto objcClassType = NamedType(bv, "objc_class_t"))
					return Type::PointerType(arch, objcClassType);
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
					return Type::PointerType(arch, classType);
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
				auto value = expr.function->GetSSAVarValue(var);
				switch (value.state)
				{
				case ConstantValue:
				case ConstantPointerValue:
				case ImportedAddressValue:
					return static_cast<uint64_t>(value.value);
				default:
					break;
				}
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

			if (call.params[0].operation == MLIL_VAR_SSA && call.params[0].function)
			{
				auto function = call.params[0].function->GetFunction();
				if (function)
				{
					auto variableType = function->GetVariableType(
					    call.params[0].GetSourceSSAVariable<MLIL_VAR_SSA>().var);
					if (!variableType.IsUnknown())
					{
						if (auto className = ClassNameFromType(variableType.GetValue()))
							return ReceiverInfo {*className, ObjCMethodKind::Instance};
					}
				}
			}

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

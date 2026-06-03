#include "Metadata.h"

#include "Util.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string_view>
#include <unordered_set>

using namespace BinaryNinja;

namespace WorkflowObjC
{
	namespace
	{
		std::shared_mutex g_viewInfosMutex;
		std::unordered_map<size_t, std::shared_ptr<AnalysisInfo>> g_viewInfos;

		std::mutex g_ignoredViewsMutex;
		std::unordered_map<size_t, bool> g_ignoredViews;

		std::mutex g_objcExternRequestsMutex;
		std::unordered_map<size_t,
		    std::unordered_map<ObjCMethodRequestKey, std::unordered_set<uint64_t>, ObjCMethodRequestKeyHash>>
		    g_objcExternRequests;

		const std::unordered_set<std::string> kSupportedArchs = {
			"aarch64",
			"x86_64",
			"armv7",
			"thumb2",
		};

		std::once_flag g_cleanupOnce;
		BNObjectDestructionCallbacks g_cleanupCallbacks = {};

		void DestructFileMetadata(void*, BNFileMetadata* file)
		{
			if (file)
				GlobalState::CleanupSession(BNFileMetadataGetSessionId(file));
		}

		std::optional<std::string> ReadCString(BinaryView* bv, uint64_t address, size_t maxLen)
		{
			std::vector<uint8_t> buffer(maxLen);
			size_t bytesRead = bv->Read(buffer.data(), address, buffer.size());
			if (bytesRead == 0)
				return std::nullopt;

			auto nullPos = std::find(buffer.begin(), buffer.begin() + bytesRead, 0);
			return std::string(buffer.begin(), nullPos);
		}

		struct PropertyMethodTypeBuilder
		{
			std::unordered_map<std::string, std::string> getterReturnTypes;
			std::unordered_set<std::string> ambiguousGetterReturnTypes;
			std::unordered_map<std::string, std::string> methodReturnTypes;
			std::unordered_set<std::string> ambiguousMethodReturnTypes;
		};

		struct SerializedMethod
		{
			std::string name;
			std::string types;
			uint64_t imp = 0;
		};

		void HashCombine(size_t& seed, size_t value)
		{
			seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
		}

		void AddPropertyTypeCandidate(
		    std::unordered_map<std::string, std::string>& values, std::unordered_set<std::string>& ambiguous,
		    const std::string& selectorName, const std::string& typeEncoding)
		{
			if (selectorName.empty() || typeEncoding.empty() || ambiguous.contains(selectorName))
				return;

			auto [it, inserted] = values.emplace(selectorName, typeEncoding);
			if (!inserted && it->second != typeEncoding)
			{
				values.erase(it);
				ambiguous.insert(selectorName);
			}
		}

		std::vector<std::string_view> SplitPropertyAttributes(std::string_view attributes)
		{
			std::vector<std::string_view> result;
			size_t start = 0;
			int nestedDepth = 0;
			bool quoted = false;

			for (size_t i = 0; i < attributes.size(); ++i)
			{
				char c = attributes[i];
				if (c == '"')
				{
					quoted = !quoted;
					continue;
				}

				if (!quoted)
				{
					if (c == '{' || c == '(' || c == '[')
						nestedDepth++;
					else if (nestedDepth > 0 && (c == '}' || c == ')' || c == ']'))
						nestedDepth--;
					else if (c == ',' && nestedDepth == 0)
					{
						result.emplace_back(attributes.substr(start, i - start));
						start = i + 1;
					}
				}
			}

			result.emplace_back(attributes.substr(start));
			return result;
		}

		void AddPropertyMethodTypes(
		    PropertyMethodTypeBuilder& builder, const std::string& propertyName, const std::string& attributes)
		{
			std::string typeEncoding;
			std::string getterName = propertyName;

			for (std::string_view attribute : SplitPropertyAttributes(attributes))
			{
				if (attribute.empty())
					continue;

				switch (attribute.front())
				{
				case 'T':
					typeEncoding = std::string(attribute.substr(1));
					break;
				case 'G':
					getterName = std::string(attribute.substr(1));
					break;
				default:
					break;
				}
			}

			AddPropertyTypeCandidate(
			    builder.getterReturnTypes, builder.ambiguousGetterReturnTypes, getterName, typeEncoding);
		}

		void AddMethodReturnType(
		    PropertyMethodTypeBuilder& builder, const std::string& selectorName, const std::string& typeEncoding)
		{
			AddPropertyTypeCandidate(
			    builder.methodReturnTypes, builder.ambiguousMethodReturnTypes, selectorName, typeEncoding);
		}

		void CollectPropertyMethodTypes(Metadata* meta, PropertyMethodTypeBuilder& builder)
		{
			if (!meta)
				return;

			if (meta->GetType() == KeyValueDataType)
			{
				auto nameMeta = meta->Get("name");
				auto attributesMeta = meta->Get("attributes");
				if (nameMeta && attributesMeta && nameMeta->GetType() == StringDataType &&
				    attributesMeta->GetType() == StringDataType)
				{
					AddPropertyMethodTypes(builder, nameMeta->GetString(), attributesMeta->GetString());
				}

				auto typesMeta = meta->Get("types");
				if (nameMeta && typesMeta && nameMeta->GetType() == StringDataType &&
				    typesMeta->GetType() == StringDataType)
				{
					AddMethodReturnType(builder, nameMeta->GetString(), typesMeta->GetString());
				}

				for (const auto& [_, value] : meta->GetKeyValueStore())
					CollectPropertyMethodTypes(value, builder);
			}
			else if (meta->GetType() == ArrayDataType)
			{
				for (const auto& value : meta->GetArray())
					CollectPropertyMethodTypes(value, builder);
			}
		}

		std::optional<std::string> MetadataString(Metadata* meta)
		{
			if (!meta || meta->GetType() != StringDataType)
				return std::nullopt;
			return meta->GetString();
		}

		std::optional<uint64_t> ReadLittleEndian(BinaryView* bv, uint64_t address, size_t size)
		{
			if (!bv || size == 0 || size > 8)
				return std::nullopt;

			uint8_t buffer[8] = {};
			if (bv->Read(buffer, address, size) != size)
				return std::nullopt;

			uint64_t value = 0;
			for (size_t i = 0; i < size; ++i)
				value |= static_cast<uint64_t>(buffer[i]) << (i * 8);
			return value;
		}

		std::optional<uint32_t> ReadU32(BinaryView* bv, uint64_t address)
		{
			if (auto value = ReadLittleEndian(bv, address, 4))
				return static_cast<uint32_t>(*value);
			return std::nullopt;
		}

		std::optional<uint64_t> ReadPointer(BinaryView* bv, uint64_t address)
		{
			return ReadLittleEndian(bv, address, bv->GetAddressSize());
		}

		std::optional<std::string> ClassNameFromObjCTypeEncoding(std::string_view encoding)
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

		std::optional<std::string> StructNameFromObjCTypeEncoding(std::string_view encoding)
		{
			if (!encoding.starts_with("{"))
				return std::nullopt;

			size_t end = encoding.find_first_of("=}", 1);
			if (end == std::string_view::npos || end == 1)
				return std::nullopt;

			return std::string(encoding.substr(1, end - 1));
		}

		std::string IvarFieldName(std::string_view name, uint64_t offset)
		{
			std::string result;
			if (!name.empty() && name.front() == '_')
				name.remove_prefix(1);

			for (char c : name)
			{
				unsigned char uc = static_cast<unsigned char>(c);
				result.push_back(std::isalnum(uc) || c == '_' ? c : '_');
			}

			if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front())))
				result = "ivar_" + std::to_string(offset);
			return result;
		}

		Ref<Type> TypeForObjCTypeEncoding(BinaryView* bv, Architecture* arch, std::string_view encoding, uint64_t size)
		{
			while (!encoding.empty() && std::string_view("rnNoORV").find(encoding.front()) != std::string_view::npos)
				encoding.remove_prefix(1);

			if (encoding.empty())
				return size != 0 ? Type::ArrayType(Type::IntegerType(1, Confidence<bool>(false)), size) : Type::VoidType();

			switch (encoding.front())
			{
			case '^':
			{
				Ref<Type> pointee = TypeForObjCTypeEncoding(bv, arch, encoding.substr(1), 0);
				if (!pointee || pointee->IsVoid())
					pointee = Type::VoidType();
				return Type::PointerType(arch, pointee);
			}
			case '@':
				if (auto className = ClassNameFromObjCTypeEncoding(encoding))
				{
					if (auto classType = NamedType(bv, *className))
						return Type::PointerType(arch, classType);
				}
				if (auto idType = NamedType(bv, "id"))
					return idType;
				return Type::PointerType(arch, Type::VoidType());
			case '#':
				if (auto classType = NamedType(bv, "Class"))
					return classType;
				if (auto objcClassType = NamedType(bv, "objc_class_t"))
					return Type::PointerType(arch, objcClassType);
				return Type::PointerType(arch, Type::VoidType());
			case ':':
				if (auto selType = NamedType(bv, "SEL"))
					return selType;
				return Type::PointerType(arch, Type::VoidType());
			case '*':
				return Type::PointerType(arch, Type::IntegerType(1, Confidence<bool>(true)));
			case 'c':
				return Type::IntegerType(1, Confidence<bool>(true));
			case 'C':
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
			case 'B':
				return Type::BoolType();
			case 'f':
				return Type::FloatType(4);
			case 'd':
				return Type::FloatType(8);
			case '{':
				if (auto structName = StructNameFromObjCTypeEncoding(encoding))
				{
					if (auto structType = NamedType(bv, *structName))
						return structType;
				}
				break;
			default:
				break;
			}

			if (size != 0)
				return Type::ArrayType(Type::IntegerType(1, Confidence<bool>(false)), size);
			return Type::IntegerType(bv ? bv->GetAddressSize() : 8, Confidence<bool>(false));
		}

		std::optional<uint64_t> MetadataUnsigned(Metadata* meta)
		{
			if (!meta || meta->GetType() != UnsignedIntegerDataType)
				return std::nullopt;
			return meta->GetUnsignedInteger();
		}

		std::optional<std::unordered_map<uint64_t, SerializedMethod>> ParseSerializedMethods(Metadata* meta)
		{
			if (!meta || meta->GetType() != ArrayDataType)
				return std::nullopt;

			std::unordered_map<uint64_t, SerializedMethod> result;
			for (const auto& methodMeta : meta->GetArray())
			{
				if (!methodMeta || methodMeta->GetType() != KeyValueDataType)
					continue;

				auto loc = MetadataUnsigned(methodMeta->Get("loc"));
				auto name = MetadataString(methodMeta->Get("name"));
				auto imp = MetadataUnsigned(methodMeta->Get("imp"));
				if (!loc || !name || !imp || name->empty() || *imp == 0)
					continue;

				SerializedMethod method;
				method.name = std::move(*name);
				if (auto types = MetadataString(methodMeta->Get("types")))
					method.types = std::move(*types);
				method.imp = *imp;
				result[*loc] = std::move(method);
			}

			return result;
		}

		void AddClassMethodList(ObjCMethodIndex& index,
		    const std::unordered_map<uint64_t, SerializedMethod>& methods, const std::string& className,
		    Metadata* methodListMeta, std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>>& target)
		{
			if (!methodListMeta)
				return;

			for (uint64_t methodLoc : methodListMeta->GetUnsignedIntegerList())
			{
				auto it = methods.find(methodLoc);
				if (it == methods.end())
					continue;

				auto& classMethods = target[className];
				classMethods.try_emplace(it->second.name, it->second.imp);
			}
		}

		void AddClassToMethodIndex(
		    ObjCMethodIndex& index, const std::unordered_map<uint64_t, SerializedMethod>& methods, Metadata* classMeta)
		{
			if (!classMeta || classMeta->GetType() != KeyValueDataType)
				return;

			auto className = MetadataString(classMeta->Get("name"));
			if (!className || className->empty())
				return;

			index.classes.insert(*className);
			AddClassMethodList(index, methods, *className, classMeta->Get("instanceMethods"), index.instanceMethods);
			AddClassMethodList(index, methods, *className, classMeta->Get("classMethods"), index.classMethods);
		}

		void AddClassesToMethodIndex(
		    ObjCMethodIndex& index, const std::unordered_map<uint64_t, SerializedMethod>& methods, Metadata* classesMeta)
		{
			if (!classesMeta || classesMeta->GetType() != ArrayDataType)
				return;

			for (const auto& classMeta : classesMeta->GetArray())
				AddClassToMethodIndex(index, methods, classMeta);
		}

		std::optional<uint64_t> GetIndexedMethodImpl(const ObjCMethodIndex& index, const ObjCMethodRequestKey& key)
		{
			const auto& methodsByClass = key.methodKind == ObjCMethodKind::Class ? index.classMethods : index.instanceMethods;
			auto classIt = methodsByClass.find(key.className);
			if (classIt == methodsByClass.end())
				return std::nullopt;

			auto methodIt = classIt->second.find(key.selectorName);
			if (methodIt == classIt->second.end() || methodIt->second == 0)
				return std::nullopt;
			return methodIt->second;
		}

		std::optional<uint64_t> ObjCClassROAddress(BinaryView* bv, uint64_t classAddress)
		{
			uint64_t dataField = classAddress + (4 * bv->GetAddressSize());
			auto data = ReadPointer(bv, dataField);
			if (!data || *data == 0 || (*data & 1) != 0)
				return std::nullopt;
			return *data & ~static_cast<uint64_t>(3);
		}

		std::optional<uint64_t> ObjCClassIvarListAddress(BinaryView* bv, uint64_t classROAddress)
		{
			uint64_t offset = classROAddress + 12;
			if (bv->GetAddressSize() == 8)
				offset += 4;

			offset += 4 * bv->GetAddressSize();
			return ReadPointer(bv, offset);
		}

		void AddClassIvarsToIndex(ObjCIvarTypeIndex& index, BinaryView* bv, const std::string& className,
		    uint64_t ivarListAddress)
		{
			if (ivarListAddress == 0)
				return;

			auto count = ReadU32(bv, ivarListAddress + 4);
			if (!count || *count > 0x1000)
				return;

			uint64_t entrySize = (bv->GetAddressSize() * 3) + 8;
			uint64_t cursor = ivarListAddress + 8;
			for (uint32_t i = 0; i < *count; ++i, cursor += entrySize)
			{
				auto offsetPointer = ReadPointer(bv, cursor);
				auto namePointer = ReadPointer(bv, cursor + bv->GetAddressSize());
				auto typePointer = ReadPointer(bv, cursor + (2 * bv->GetAddressSize()));
				if (!offsetPointer || !namePointer || !typePointer || *offsetPointer == 0 || *typePointer == 0)
					continue;

				auto ivarOffset = ReadU32(bv, *offsetPointer);
				auto name = ReadCString(bv, *namePointer, 500);
				auto typeEncoding = ReadCString(bv, *typePointer, 500);
				if (!ivarOffset || !typeEncoding)
					continue;

				auto ivarSize = ReadU32(bv, cursor + (3 * bv->GetAddressSize()) + 4);
				ObjCIvarTypeIndex::Ivar ivar;
				ivar.offset = *ivarOffset;
				ivar.size = ivarSize.value_or(0);
				ivar.name = name.value_or("ivar_" + std::to_string(*ivarOffset));
				ivar.typeEncoding = *typeEncoding;
				index.ivars[className].push_back(std::move(ivar));

				if (auto ivarClassName = ClassNameFromObjCTypeEncoding(*typeEncoding))
					index.ivarClassNames[className][*ivarOffset] = std::move(*ivarClassName);
			}
		}

		void AddClassIvarsToIndex(ObjCIvarTypeIndex& index, BinaryView* bv, Metadata* classMeta)
		{
			if (!classMeta || classMeta->GetType() != KeyValueDataType)
				return;

			auto className = MetadataString(classMeta->Get("name"));
			auto classAddress = MetadataUnsigned(classMeta->Get("loc"));
			if (!className || className->empty() || !classAddress)
				return;

			auto classROAddress = ObjCClassROAddress(bv, *classAddress);
			if (!classROAddress)
				return;

			auto ivarListAddress = ObjCClassIvarListAddress(bv, *classROAddress);
			if (!ivarListAddress)
				return;

			AddClassIvarsToIndex(index, bv, *className, *ivarListAddress);
		}

		void AddClassesIvarsToIndex(ObjCIvarTypeIndex& index, BinaryView* bv, Metadata* classesMeta)
		{
			if (!classesMeta || classesMeta->GetType() != ArrayDataType)
				return;

			for (const auto& classMeta : classesMeta->GetArray())
				AddClassIvarsToIndex(index, bv, classMeta);
		}

		bool EnsureClassIvarTypesFromIndex(BinaryView* bv, const ObjCIvarTypeIndex& index, const std::string& className)
		{
			if (!bv || className.empty())
				return false;

			auto classIt = index.ivars.find(className);
			if (classIt == index.ivars.end() || classIt->second.empty())
				return false;

			QualifiedName classTypeName(className);
			auto existingType = bv->GetTypeByName(classTypeName);
			if (existingType && !bv->IsTypeAutoDefined(classTypeName))
				return false;

			StructureBuilder builder;
			if (existingType && existingType->IsStructure())
				builder = existingType->GetStructure();
			else
				builder.SetStructureType(StructStructureType);

			bool changed = false;
			uint64_t width = builder.GetWidth();
			for (const auto& ivar : classIt->second)
			{
				StructureMember existingMember;
				if (builder.GetMemberAtOffset(static_cast<int64_t>(ivar.offset), existingMember))
					continue;

				auto type = TypeForObjCTypeEncoding(bv, bv->GetDefaultArchitecture(), ivar.typeEncoding, ivar.size);
				if (!type)
					continue;

				builder.AddMemberAtOffset(Confidence<Ref<Type>>(type, BN_FULL_CONFIDENCE),
				    IvarFieldName(ivar.name, ivar.offset), ivar.offset, false);
				width = std::max<uint64_t>(width, ivar.offset + std::max<uint64_t>(ivar.size, type->GetWidth()));
				changed = true;
			}

			if (!changed)
				return false;

			if (width > builder.GetWidth())
				builder.SetWidth(width);

			std::string typeId = bv->GetTypeId(classTypeName);
			if (typeId.empty())
				typeId = Type::GenerateAutoTypeId("objective-c", classTypeName);
			bv->DefineType(typeId, classTypeName, Type::StructureType(builder.Finalize()));
			return true;
		}
	}

	size_t ObjCMethodRequestKeyHash::operator()(const ObjCMethodRequestKey& key) const
	{
		size_t seed = std::hash<std::string>{}(key.className);
		HashCombine(seed, std::hash<std::string>{}(key.selectorName));
		HashCombine(seed, std::hash<int>{}(static_cast<int>(key.methodKind)));
		return seed;
	}

	std::string ObjCMethodSymbolName(const ObjCMethodRequestKey& key)
	{
		return std::string(key.methodKind == ObjCMethodKind::Class ? "+[" : "-[") + key.className + " " +
		    key.selectorName + "]";
	}

	size_t GlobalState::Id(BinaryView* bv)
	{
		return bv->GetFile()->GetSessionId();
	}

	bool GlobalState::IsSupportedArch(BinaryView* bv)
	{
		auto arch = bv->GetDefaultArchitecture();
		return arch && kSupportedArchs.contains(arch->GetName());
	}

	void GlobalState::RegisterCleanup()
	{
		std::call_once(g_cleanupOnce, [] {
			g_cleanupCallbacks.destructFileMetadata = DestructFileMetadata;
			BNRegisterObjectDestructionCallbacks(&g_cleanupCallbacks);
		});
	}

	void GlobalState::CleanupSession(size_t sessionId)
	{
		{
			std::unique_lock lock(g_viewInfosMutex);
			g_viewInfos.erase(sessionId);
		}
		{
			std::lock_guard lock(g_ignoredViewsMutex);
			g_ignoredViews.erase(sessionId);
		}
		{
			std::lock_guard lock(g_objcExternRequestsMutex);
			g_objcExternRequests.erase(sessionId);
		}
	}

	std::shared_ptr<AnalysisInfo> GlobalState::GetAnalysisInfo(BinaryView* bv)
	{
		if (!bv)
			return nullptr;

		size_t id = Id(bv);
		{
			std::shared_lock lock(g_viewInfosMutex);
			auto it = g_viewInfos.find(id);
			if (it != g_viewInfos.end() && it->second->imageBase == bv->GetStart())
				return it->second;
		}

		auto info = AnalysisInfo::FromView(bv);
		if (!info)
			return nullptr;

		std::unique_lock lock(g_viewInfosMutex);
		g_viewInfos[id] = info;
		return info;
	}

	bool GlobalState::ShouldIgnoreView(BinaryView* bv)
	{
		if (!bv)
			return true;

		size_t id = Id(bv);
		{
			std::lock_guard lock(g_ignoredViewsMutex);
			auto it = g_ignoredViews.find(id);
			if (it != g_ignoredViews.end())
				return it->second;
		}

		bool ignore = !(IsSupportedArch(bv) && AnalysisInfo::HasMetadata(bv));
		std::lock_guard lock(g_ignoredViewsMutex);
		g_ignoredViews[id] = ignore;
		return ignore;
	}

	void GlobalState::AddObjCExternRequest(BinaryView* bv, const ObjCMethodRequestKey& key, Function* func)
	{
		if (!bv || !func || key.className.empty() || key.selectorName.empty())
			return;

		std::lock_guard lock(g_objcExternRequestsMutex);
		g_objcExternRequests[Id(bv)][key].insert(func->GetStart());
	}

	std::vector<ObjCExternRequest> GlobalState::DrainObjCExternRequests(BinaryView* bv)
	{
		if (!bv)
			return {};

		std::unordered_map<ObjCMethodRequestKey, std::unordered_set<uint64_t>, ObjCMethodRequestKeyHash> requests;
		{
			std::lock_guard lock(g_objcExternRequestsMutex);
			auto it = g_objcExternRequests.find(Id(bv));
			if (it == g_objcExternRequests.end())
				return {};
			requests = std::move(it->second);
			g_objcExternRequests.erase(it);
		}

		std::vector<ObjCExternRequest> result;
		result.reserve(requests.size());
		for (auto& [key, functions] : requests)
		{
			ObjCExternRequest request;
			request.key = std::move(key);
			request.functionStarts.assign(functions.begin(), functions.end());
			std::sort(request.functionStarts.begin(), request.functionStarts.end());
			result.push_back(std::move(request));
		}

		return result;
	}

	std::shared_ptr<AnalysisInfo> AnalysisInfo::FromView(BinaryView* bv)
	{
		if (!HasMetadata(bv))
			return nullptr;

		auto info = std::make_shared<AnalysisInfo>();
		info->imageBase = bv->GetStart();
		info->shouldRewriteToDirectCalls = Settings::Instance()->Get<bool>(
		    "analysis.objectiveC.resolveDynamicDispatch", bv);

		if (auto section = bv->GetSectionByName("__objc_stubs"))
			info->objcStubs = AddressRange {section->GetStart(), section->GetEnd()};

		return info;
	}

	bool AnalysisInfo::HasMetadata(BinaryView* bv)
	{
		return bv && bv->QueryMetadata("Objective-C") != nullptr;
	}

	std::optional<uint64_t> AnalysisInfo::GetSelectorImpl(BinaryView* bv, uint64_t selectorAddr) const
	{
		auto get = [selectorAddr](const SelectorImplementations& impls) -> std::optional<uint64_t> {
			const std::vector<uint64_t>* values = nullptr;
			if (auto it = impls.selRefToImpl.find(selectorAddr); it != impls.selRefToImpl.end())
				values = &it->second;
			else if (auto it = impls.selToImpl.find(selectorAddr); it != impls.selToImpl.end())
				values = &it->second;

			if (!values || values->empty() || values->front() == 0)
				return std::nullopt;
			return values->front();
		};

		{
			std::shared_lock lock(m_selectorImplsMutex);
			if (m_selectorImplsLoaded)
				return m_selectorImpls ? get(*m_selectorImpls) : std::nullopt;
		}

		std::unique_lock lock(m_selectorImplsMutex);
		if (!m_selectorImplsLoaded)
		{
			m_selectorImpls = LoadSelectorImpls(bv);
			m_selectorImplsLoaded = true;
		}

		return m_selectorImpls ? get(*m_selectorImpls) : std::nullopt;
	}

	bool AnalysisInfo::HasClass(BinaryView* bv, const std::string& className) const
	{
		auto get = [&className](const ObjCMethodIndex& index) -> bool {
			return index.classes.contains(className);
		};

		{
			std::shared_lock lock(m_objcMethodIndexMutex);
			if (m_objcMethodIndexLoaded)
				return m_objcMethodIndex ? get(*m_objcMethodIndex) : false;
		}

		std::unique_lock lock(m_objcMethodIndexMutex);
		if (!m_objcMethodIndexLoaded)
		{
			m_objcMethodIndex = LoadObjCMethodIndex(bv);
			m_objcMethodIndexLoaded = true;
		}

		return m_objcMethodIndex ? get(*m_objcMethodIndex) : false;
	}

	std::optional<uint64_t> AnalysisInfo::GetMethodImpl(BinaryView* bv, const ObjCMethodRequestKey& key) const
	{
		auto get = [&key](const ObjCMethodIndex& index) -> std::optional<uint64_t> {
			return GetIndexedMethodImpl(index, key);
		};

		{
			std::shared_lock lock(m_objcMethodIndexMutex);
			if (m_objcMethodIndexLoaded)
			{
				if (m_objcMethodIndex)
				{
					if (auto impl = get(*m_objcMethodIndex))
						return impl;
				}
			}
		}

		std::unique_lock lock(m_objcMethodIndexMutex);
		if (!m_objcMethodIndexLoaded)
		{
			m_objcMethodIndex = LoadObjCMethodIndex(bv);
			m_objcMethodIndexLoaded = true;
		}

		if (m_objcMethodIndex)
		{
			if (auto impl = get(*m_objcMethodIndex))
				return impl;
		}

		if (bv)
		{
			if (auto symbol = bv->GetSymbolByRawName(ObjCMethodSymbolName(key), BinaryView::GetInternalNameSpace()))
				return symbol->GetAddress();
		}

		return std::nullopt;
	}

	bool AnalysisInfo::EnsureClassIvarTypes(BinaryView* bv, const std::string& className) const
	{
		if (!bv || className.empty())
			return false;

		{
			std::shared_lock lock(m_objcIvarTypeIndexMutex);
			if (m_objcIvarTypeIndexLoaded)
				return m_objcIvarTypeIndex ? EnsureClassIvarTypesFromIndex(bv, *m_objcIvarTypeIndex, className) : false;
		}

		std::unique_lock lock(m_objcIvarTypeIndexMutex);
		if (!m_objcIvarTypeIndexLoaded)
		{
			m_objcIvarTypeIndex = LoadObjCIvarTypeIndex(bv);
			m_objcIvarTypeIndexLoaded = true;
		}

		return m_objcIvarTypeIndex ? EnsureClassIvarTypesFromIndex(bv, *m_objcIvarTypeIndex, className) : false;
	}

	std::optional<std::string> AnalysisInfo::GetIvarClassName(
	    BinaryView* bv, const std::string& className, uint64_t offset) const
	{
		auto get = [&className, offset](const ObjCIvarTypeIndex& index) -> std::optional<std::string> {
			auto classIt = index.ivarClassNames.find(className);
			if (classIt == index.ivarClassNames.end())
				return std::nullopt;

			auto ivarIt = classIt->second.find(offset);
			if (ivarIt == classIt->second.end())
				return std::nullopt;
			return ivarIt->second;
		};

		{
			std::shared_lock lock(m_objcIvarTypeIndexMutex);
			if (m_objcIvarTypeIndexLoaded)
				return m_objcIvarTypeIndex ? get(*m_objcIvarTypeIndex) : std::nullopt;
		}

		std::unique_lock lock(m_objcIvarTypeIndexMutex);
		if (!m_objcIvarTypeIndexLoaded)
		{
			m_objcIvarTypeIndex = LoadObjCIvarTypeIndex(bv);
			m_objcIvarTypeIndexLoaded = true;
		}

		return m_objcIvarTypeIndex ? get(*m_objcIvarTypeIndex) : std::nullopt;
	}

	std::optional<std::string> AnalysisInfo::GetPropertyGetterTypeEncoding(
	    BinaryView* bv, const std::string& selectorName) const
	{
		auto get = [&selectorName](const PropertyMethodTypes& types) -> std::optional<std::string> {
			auto it = types.getterReturnTypes.find(selectorName);
			if (it == types.getterReturnTypes.end())
				return std::nullopt;
			return it->second;
		};

		{
			std::shared_lock lock(m_propertyMethodTypesMutex);
			if (m_propertyMethodTypesLoaded)
				return m_propertyMethodTypes ? get(*m_propertyMethodTypes) : std::nullopt;
		}

		std::unique_lock lock(m_propertyMethodTypesMutex);
		if (!m_propertyMethodTypesLoaded)
		{
			m_propertyMethodTypes = LoadPropertyMethodTypes(bv);
			m_propertyMethodTypesLoaded = true;
		}

		return m_propertyMethodTypes ? get(*m_propertyMethodTypes) : std::nullopt;
	}

	std::optional<std::string> AnalysisInfo::GetMethodReturnTypeEncoding(
	    BinaryView* bv, const std::string& selectorName) const
	{
		auto get = [&selectorName](const PropertyMethodTypes& types) -> std::optional<std::string> {
			auto it = types.methodReturnTypes.find(selectorName);
			if (it == types.methodReturnTypes.end())
				return std::nullopt;
			return it->second;
		};

		{
			std::shared_lock lock(m_propertyMethodTypesMutex);
			if (m_propertyMethodTypesLoaded)
				return m_propertyMethodTypes ? get(*m_propertyMethodTypes) : std::nullopt;
		}

		std::unique_lock lock(m_propertyMethodTypesMutex);
		if (!m_propertyMethodTypesLoaded)
		{
			m_propertyMethodTypes = LoadPropertyMethodTypes(bv);
			m_propertyMethodTypesLoaded = true;
		}

		return m_propertyMethodTypes ? get(*m_propertyMethodTypes) : std::nullopt;
	}

	std::optional<SelectorImplementations> AnalysisInfo::LoadSelectorImpls(BinaryView* bv) const
	{
		auto meta = bv->QueryMetadata("Objective-C");
		if (!meta || meta->GetType() != KeyValueDataType)
			return std::nullopt;

		auto versionMeta = meta->Get("version");
		if (!versionMeta || versionMeta->GetType() != UnsignedIntegerDataType)
			return std::nullopt;

		uint64_t version = versionMeta->GetUnsignedInteger();
		if (version != 1)
		{
			LogError("workflow_objc: Unexpected Objective-C metadata version. Expected 1, got %llu.",
			    static_cast<unsigned long long>(version));
			return std::nullopt;
		}

		SelectorImplementations result;
		if (auto selRefMeta = meta->Get("selRefImplementations"))
		{
			if (auto parsed = ParseSelectorImpls(selRefMeta))
				result.selRefToImpl = std::move(*parsed);
		}
		if (auto selMeta = meta->Get("selImplementations"))
		{
			if (auto parsed = ParseSelectorImpls(selMeta))
				result.selToImpl = std::move(*parsed);
		}

		return result;
	}

	std::optional<PropertyMethodTypes> AnalysisInfo::LoadPropertyMethodTypes(BinaryView* bv) const
	{
		auto meta = bv->QueryMetadata("Objective-C");
		if (!meta || meta->GetType() != KeyValueDataType)
			return std::nullopt;

		auto versionMeta = meta->Get("version");
		if (!versionMeta || versionMeta->GetType() != UnsignedIntegerDataType)
			return std::nullopt;

		uint64_t version = versionMeta->GetUnsignedInteger();
		if (version != 1)
		{
			LogError("workflow_objc: Unexpected Objective-C metadata version. Expected 1, got %llu.",
			    static_cast<unsigned long long>(version));
			return std::nullopt;
		}

		PropertyMethodTypeBuilder builder;
		CollectPropertyMethodTypes(meta, builder);

		PropertyMethodTypes result;
		result.getterReturnTypes = std::move(builder.getterReturnTypes);
		result.methodReturnTypes = std::move(builder.methodReturnTypes);
		return result;
	}

	std::optional<ObjCMethodIndex> AnalysisInfo::LoadObjCMethodIndex(BinaryView* bv) const
	{
		auto meta = bv->QueryMetadata("Objective-C");
		if (!meta || meta->GetType() != KeyValueDataType)
			return std::nullopt;

		auto versionMeta = meta->Get("version");
		if (!versionMeta || versionMeta->GetType() != UnsignedIntegerDataType)
			return std::nullopt;

		uint64_t version = versionMeta->GetUnsignedInteger();
		if (version != 1)
		{
			LogError("workflow_objc: Unexpected Objective-C metadata version. Expected 1, got %llu.",
			    static_cast<unsigned long long>(version));
			return std::nullopt;
		}

		auto methods = ParseSerializedMethods(meta->Get("methods"));
		if (!methods)
			return std::nullopt;

		ObjCMethodIndex result;
		AddClassesToMethodIndex(result, *methods, meta->Get("classes"));
		AddClassesToMethodIndex(result, *methods, meta->Get("categories"));
		return result;
	}

	std::optional<ObjCIvarTypeIndex> AnalysisInfo::LoadObjCIvarTypeIndex(BinaryView* bv) const
	{
		auto meta = bv->QueryMetadata("Objective-C");
		if (!meta || meta->GetType() != KeyValueDataType)
			return std::nullopt;

		auto versionMeta = meta->Get("version");
		if (!versionMeta || versionMeta->GetType() != UnsignedIntegerDataType)
			return std::nullopt;

		uint64_t version = versionMeta->GetUnsignedInteger();
		if (version != 1)
		{
			LogError("workflow_objc: Unexpected Objective-C metadata version. Expected 1, got %llu.",
			    static_cast<unsigned long long>(version));
			return std::nullopt;
		}

		ObjCIvarTypeIndex result;
		AddClassesIvarsToIndex(result, bv, meta->Get("classes"));
		return result;
	}

	std::optional<std::unordered_map<uint64_t, std::vector<uint64_t>>> AnalysisInfo::ParseSelectorImpls(Metadata* meta)
	{
		if (!meta || meta->GetType() != ArrayDataType)
			return std::nullopt;

		std::unordered_map<uint64_t, std::vector<uint64_t>> result;
		for (const auto& itemMeta : meta->GetArray())
		{
			if (!itemMeta || itemMeta->GetType() != ArrayDataType)
				return std::nullopt;

			auto item = itemMeta->GetArray();
			if (item.size() != 2)
			{
				LogWarn("Expected selector implementation metadata to have 2 items, found %zu", item.size());
				return std::nullopt;
			}

			if (!item[0] || item[0]->GetType() != UnsignedIntegerDataType || !item[1])
				return std::nullopt;

			result[item[0]->GetUnsignedInteger()] = item[1]->GetUnsignedIntegerList();
		}

		return result;
	}

	std::optional<Selector> Selector::FromAddress(BinaryView* bv, uint64_t address)
	{
		std::optional<std::string> name;
		if (bv->IsValidOffset(address))
			name = ReadCString(bv, address, 500);
		else if (auto sym = bv->GetSymbolByAddress(address))
		{
			std::string rawName = sym->GetRawName();
			if (std::string_view(rawName).starts_with("sel_"))
				name = rawName.substr(4);
		}

		if (!name)
			return std::nullopt;

		return Selector {*name, address};
	}

	bool Selector::IsInitFamily() const
	{
		std::string_view view(name);
		if (!view.starts_with("init"))
			return false;

		view.remove_prefix(4);
		if (view.empty())
			return true;

		unsigned char c = static_cast<unsigned char>(view.front());
		return std::isupper(c) || c == ':';
	}

	std::vector<std::string> Selector::ArgumentLabels() const
	{
		std::vector<std::string> labels;
		if (name.find(':') == std::string::npos)
			return labels;

		size_t start = 0;
		while (start < name.size())
		{
			size_t end = name.find(':', start);
			if (end == std::string::npos)
				end = name.size();

			if (end != start)
				labels.emplace_back(name.substr(start, end - start));

			start = end + 1;
		}

		return labels;
	}
}

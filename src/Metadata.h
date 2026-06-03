#pragma once

#include <binaryninjaapi.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WorkflowObjC
{
	/// TODO: Fill in documentation.
	struct AddressRange
	{
		uint64_t start = 0;
		uint64_t end = 0;

		/// TODO: Fill in documentation.
		bool Contains(uint64_t value) const { return value >= start && value < end; }
	};

	/// TODO: Fill in documentation.
	struct SelectorImplementations
	{
		std::unordered_map<uint64_t, std::vector<uint64_t>> selRefToImpl;
		std::unordered_map<uint64_t, std::vector<uint64_t>> selToImpl;
	};

	/// TODO: Fill in documentation.
	struct PropertyMethodTypes
	{
		std::unordered_map<std::string, std::string> getterReturnTypes;
		std::unordered_map<std::string, std::string> methodReturnTypes;
	};

	/// TODO: Fill in documentation.
	enum class ObjCMethodKind
	{
		Instance,
		Class,
	};

	/// TODO: Fill in documentation.
	struct ObjCMethodRequestKey
	{
		std::string className;
		std::string selectorName;
		ObjCMethodKind methodKind = ObjCMethodKind::Instance;

		/// TODO: Fill in documentation.
		bool operator==(const ObjCMethodRequestKey& other) const
		{
			return className == other.className && selectorName == other.selectorName && methodKind == other.methodKind;
		}
	};

	/// TODO: Fill in documentation.
	struct ObjCMethodRequestKeyHash
	{
		/// TODO: Fill in documentation.
		size_t operator()(const ObjCMethodRequestKey& key) const;
	};

	/// TODO: Fill in documentation.
	struct ObjCExternRequest
	{
		ObjCMethodRequestKey key;
		std::vector<uint64_t> functionStarts;
	};

	/// TODO: Fill in documentation.
	struct ObjCMethodIndex
	{
		std::unordered_set<std::string> classes;
		std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> instanceMethods;
		std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> classMethods;
	};

	/// TODO: Fill in documentation.
	struct ObjCIvarTypeIndex
	{
		/// TODO: Fill in documentation.
		struct Ivar
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			std::string name;
			std::string typeEncoding;
		};

		std::unordered_map<std::string, std::unordered_map<uint64_t, std::string>> ivarClassNames;
		std::unordered_map<std::string, std::vector<Ivar>> ivars;
	};

	/// TODO: Fill in documentation.
	std::string ObjCMethodSymbolName(const ObjCMethodRequestKey& key);

	/// TODO: Fill in documentation.
	class AnalysisInfo
	{
		mutable std::shared_mutex m_selectorImplsMutex;
		mutable bool m_selectorImplsLoaded = false;
		mutable std::optional<SelectorImplementations> m_selectorImpls;
		mutable std::shared_mutex m_propertyMethodTypesMutex;
		mutable bool m_propertyMethodTypesLoaded = false;
		mutable std::optional<PropertyMethodTypes> m_propertyMethodTypes;
		mutable std::shared_mutex m_objcMethodIndexMutex;
		mutable bool m_objcMethodIndexLoaded = false;
		mutable std::optional<ObjCMethodIndex> m_objcMethodIndex;
		mutable std::shared_mutex m_objcIvarTypeIndexMutex;
		mutable bool m_objcIvarTypeIndexLoaded = false;
		mutable std::optional<ObjCIvarTypeIndex> m_objcIvarTypeIndex;

		std::optional<SelectorImplementations> LoadSelectorImpls(BinaryNinja::BinaryView* bv) const;
		std::optional<PropertyMethodTypes> LoadPropertyMethodTypes(BinaryNinja::BinaryView* bv) const;
		std::optional<ObjCMethodIndex> LoadObjCMethodIndex(BinaryNinja::BinaryView* bv) const;
		std::optional<ObjCIvarTypeIndex> LoadObjCIvarTypeIndex(BinaryNinja::BinaryView* bv) const;
		static std::optional<std::unordered_map<uint64_t, std::vector<uint64_t>>> ParseSelectorImpls(
		    BinaryNinja::Metadata* meta);

	public:
		uint64_t imageBase = 0;
		std::optional<AddressRange> objcStubs;
		bool shouldRewriteToDirectCalls = false;

		/// TODO: Fill in documentation.
		static std::shared_ptr<AnalysisInfo> FromView(BinaryNinja::BinaryView* bv);
		/// TODO: Fill in documentation.
		static bool HasMetadata(BinaryNinja::BinaryView* bv);

		/// TODO: Fill in documentation.
		std::optional<uint64_t> GetSelectorImpl(BinaryNinja::BinaryView* bv, uint64_t selectorAddr) const;
		/// TODO: Fill in documentation.
		bool HasClass(BinaryNinja::BinaryView* bv, const std::string& className) const;
		/// TODO: Fill in documentation.
		std::optional<uint64_t> GetMethodImpl(BinaryNinja::BinaryView* bv, const ObjCMethodRequestKey& key) const;
		/// TODO: Fill in documentation.
		bool EnsureClassIvarTypes(BinaryNinja::BinaryView* bv, const std::string& className) const;
		/// TODO: Fill in documentation.
		std::optional<std::string> GetIvarClassName(
		    BinaryNinja::BinaryView* bv, const std::string& className, uint64_t offset) const;
		/// TODO: Fill in documentation.
		std::optional<std::string> GetPropertyGetterTypeEncoding(
		    BinaryNinja::BinaryView* bv, const std::string& selectorName) const;
		/// TODO: Fill in documentation.
		std::optional<std::string> GetMethodReturnTypeEncoding(
		    BinaryNinja::BinaryView* bv, const std::string& selectorName) const;
	};

	/// TODO: Fill in documentation.
	class GlobalState
	{
		static size_t Id(BinaryNinja::BinaryView* bv);
		static bool IsSupportedArch(BinaryNinja::BinaryView* bv);

	public:
		/// TODO: Fill in documentation.
		static void RegisterCleanup();
		/// TODO: Fill in documentation.
		static void CleanupSession(size_t sessionId);
		/// TODO: Fill in documentation.
		static std::shared_ptr<AnalysisInfo> GetAnalysisInfo(BinaryNinja::BinaryView* bv);
		/// TODO: Fill in documentation.
		static bool ShouldIgnoreView(BinaryNinja::BinaryView* bv);
		/// TODO: Fill in documentation.
		static void AddObjCExternRequest(
		    BinaryNinja::BinaryView* bv, const ObjCMethodRequestKey& key, BinaryNinja::Function* func);
		/// TODO: Fill in documentation.
		static std::vector<ObjCExternRequest> DrainObjCExternRequests(BinaryNinja::BinaryView* bv);
	};

	/// TODO: Fill in documentation.
	struct Selector
	{
		std::string name;
		uint64_t addr = 0;

		/// TODO: Fill in documentation.
		static std::optional<Selector> FromAddress(BinaryNinja::BinaryView* bv, uint64_t addr);
		/// TODO: Fill in documentation.
		bool IsInitFamily() const;
		/// TODO: Fill in documentation.
		std::vector<std::string> ArgumentLabels() const;
	};
}

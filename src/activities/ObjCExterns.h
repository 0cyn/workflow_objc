#pragma once

#include "../Metadata.h"

#include <binaryninjaapi.h>

#include <cstdint>
#include <optional>

namespace WorkflowObjC::Activities
{
	struct MethodDispatchResolution
	{
		ObjCMethodRequestKey key;
		std::optional<uint64_t> implAddress;
		std::optional<uint64_t> externAddress;
	};

	std::optional<uint64_t> ObjCExternMethodAddress(
	    BinaryNinja::BinaryView* bv, const ObjCMethodRequestKey& key);
	std::optional<MethodDispatchResolution> ResolveMethodDispatch(
	    BinaryNinja::BinaryView* bv, const AnalysisInfo& info, const ObjCMethodRequestKey& key);
	size_t MaterializePendingObjCExterns(BinaryNinja::BinaryView* bv);
}

#pragma once

#include <binaryninjaapi.h>
#include <mediumlevelilinstruction.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace WorkflowObjC
{
	/// TODO: Fill in documentation.
	struct Call
	{
		BinaryNinja::MediumLevelILInstruction instr;
		BinaryNinja::MediumLevelILInstruction dest;
		std::vector<BinaryNinja::MediumLevelILInstruction> params;
		BinaryNinja::Ref<BinaryNinja::Function> target;
		BinaryNinja::Ref<BinaryNinja::Type> targetType;
		std::string targetName;
	};

	/// TODO: Fill in documentation.
	std::optional<Call> MatchCallToFunctionNamed(
	    const BinaryNinja::MediumLevelILInstruction& instr,
	    BinaryNinja::BinaryView* view,
	    const std::vector<std::string_view>& functionNames);

	/// TODO: Fill in documentation.
	std::optional<uint64_t> MatchConstantPointerOrLoadOfConstantPointer(
	    const BinaryNinja::MediumLevelILInstruction& instr);

	/// TODO: Fill in documentation.
	std::optional<std::string_view> ClassNameFromSymbolName(std::string_view symbolName);
	/// TODO: Fill in documentation.
	std::optional<std::string> ClassNameFromType(BinaryNinja::Type* type);
	/// TODO: Fill in documentation.
	bool IsAllocLikeSelector(std::string_view name);
	/// TODO: Fill in documentation.
	std::optional<std::string> ClassNameFromObjCMethodSymbolName(std::string_view symbolName);
	/// TODO: Fill in documentation.
	std::optional<std::string> ClassNameFromClassObjectAddress(
	    BinaryNinja::BinaryView* view, uint64_t classAddress);
	/// TODO: Fill in documentation.
	std::optional<uint64_t> ClassObjectAddressFromClassName(
	    BinaryNinja::BinaryView* view, std::string_view className);
	/// TODO: Fill in documentation.
	std::optional<uint64_t> ClassObjectAddressFromClassReferenceAddress(
	    BinaryNinja::BinaryView* view, uint64_t address);
	/// TODO: Fill in documentation.
	std::optional<std::string> ClassNameFromClassReferenceAddress(
	    BinaryNinja::BinaryView* view, uint64_t address);
	/// TODO: Fill in documentation.
	std::optional<std::string> SuperclassNameFromClassObjectAddress(
	    BinaryNinja::BinaryView* view, uint64_t classAddress);
	/// TODO: Fill in documentation.
	std::optional<std::string> SuperclassNameFromClassName(
	    BinaryNinja::BinaryView* view, std::string_view className);
	/// TODO: Fill in documentation.
	BinaryNinja::Ref<BinaryNinja::Type> TypeLibraryObjectType(
	    BinaryNinja::BinaryView* view, std::string_view name, std::optional<uint64_t> address = std::nullopt);
	/// TODO: Fill in documentation.
	BinaryNinja::Ref<BinaryNinja::Type> NamedType(BinaryNinja::BinaryView* view, std::string_view name);
	/// TODO: Fill in documentation.
	BinaryNinja::Ref<BinaryNinja::Type> ClassInstanceType(BinaryNinja::BinaryView* view, std::string_view name);
	/// TODO: Fill in documentation.
	std::vector<std::string> GenerateArgumentNames(const std::vector<std::string>& labels);

	/// TODO: Fill in documentation.
	void AdjustReturnTypeOfCall(const Call& call, BinaryNinja::Type* returnType, uint8_t confidence);
}

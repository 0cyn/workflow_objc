#include "Util.h"

#include <algorithm>
#include <cctype>
#include <string>

using namespace BinaryNinja;

namespace WorkflowObjC
{
	namespace
	{
		std::optional<uint64_t> ReadLittleEndian(BinaryView* view, uint64_t address, size_t size)
		{
			if (!view || size == 0 || size > 8)
				return std::nullopt;

			uint8_t buffer[8] = {};
			if (view->Read(buffer, address, size) != size)
				return std::nullopt;

			uint64_t value = 0;
			for (size_t i = 0; i < size; ++i)
				value |= static_cast<uint64_t>(buffer[i]) << (i * 8);
			return value;
		}

		std::optional<uint64_t> ReadPointer(BinaryView* view, uint64_t address)
		{
			return ReadLittleEndian(view, address, view ? view->GetAddressSize() : 0);
		}

		std::optional<uint64_t> SSAVariableValueOrLoadOfConstantPointer(
		    MediumLevelILFunction* function, const SSAVariable& var)
		{
			RegisterValue value = function->GetSSAVarValue(var);
			if (value.state == ConstantPointerValue)
				return static_cast<uint64_t>(value.value);
			if (value.state != UndeterminedValue)
				return std::nullopt;

			size_t defIndex = function->GetSSAVarDefinition(var);
			if (defIndex == BN_INVALID_EXPR)
				return std::nullopt;

			auto def = function->GetInstruction(defIndex);
			if (def.operation != MLIL_SET_VAR_SSA)
				return std::nullopt;

			auto src = def.GetSourceExpr<MLIL_SET_VAR_SSA>();
			if (src.operation != MLIL_LOAD_SSA)
				return std::nullopt;

			auto loadSrc = src.GetSourceExpr<MLIL_LOAD_SSA>();
			if (loadSrc.operation != MLIL_CONST_PTR)
				return std::nullopt;

			return static_cast<uint64_t>(loadSrc.GetConstant<MLIL_CONST_PTR>());
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
	}

	std::optional<Call> MatchCallToFunctionNamed(
	    const MediumLevelILInstruction& instr, BinaryView* view, const std::vector<std::string_view>& functionNames)
	{
		MediumLevelILInstruction dest;
		std::vector<MediumLevelILInstruction> params;
		if (instr.operation == MLIL_CALL_SSA)
		{
			dest = instr.GetDestExpr<MLIL_CALL_SSA>();
			params = instr.GetParameterExprs<MLIL_CALL_SSA>();
		}
		else if (instr.operation == MLIL_TAILCALL_SSA)
		{
			dest = instr.GetDestExpr<MLIL_TAILCALL_SSA>();
			params = instr.GetParameterExprs<MLIL_TAILCALL_SSA>();
		}
		else
		{
			return std::nullopt;
		}

		if (dest.operation != MLIL_CONST_PTR)
			return std::nullopt;

		uint64_t callTarget = static_cast<uint64_t>(dest.GetConstant<MLIL_CONST_PTR>());
		Ref<Function> targetFunction;
		Ref<Type> targetType;
		std::string fullName;

		if (auto symbol = view->GetSymbolByAddress(callTarget))
			fullName = symbol->GetFullName();

		if (fullName.empty())
			return std::nullopt;

		if (std::find(functionNames.begin(), functionNames.end(), std::string_view(fullName)) == functionNames.end())
			return std::nullopt;

		if (!targetType)
		{
			DataVariable dataVariable;
			if (view->GetDataVariableAtAddress(callTarget, dataVariable) && !dataVariable.type.IsUnknown())
			{
				auto dataType = dataVariable.type.GetValue();
				if (dataType && dataType->IsFunction())
					targetType = dataType;
			}
		}

		if (!targetType)
		{
			targetFunction = view->GetAnalysisFunction(instr.function->GetFunction()->GetPlatform(), callTarget);
			if (targetFunction)
				targetType = targetFunction->GetType();
		}

		return Call {instr, dest, params, targetFunction, targetType, std::move(fullName)};
	}

	std::optional<uint64_t> MatchConstantPointerOrLoadOfConstantPointer(const MediumLevelILInstruction& instr)
	{
		if (instr.operation == MLIL_CONST_PTR)
			return static_cast<uint64_t>(instr.GetConstant<MLIL_CONST_PTR>());

		if (instr.operation == MLIL_VAR_SSA)
			return SSAVariableValueOrLoadOfConstantPointer(
			    instr.function, instr.GetSourceSSAVariable<MLIL_VAR_SSA>());

		return std::nullopt;
	}

	std::optional<std::string_view> ClassNameFromSymbolName(std::string_view symbolName)
	{
		if (symbolName.starts_with("cls_"))
			return symbolName.substr(4);
		if (symbolName.starts_with("clsRef_"))
			return symbolName.substr(7);
		if (symbolName.starts_with("superRef_"))
			return symbolName.substr(9);
		if (symbolName.starts_with("_OBJC_CLASS_$_"))
			return symbolName.substr(14);
		return std::nullopt;
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

	bool IsAllocLikeSelector(std::string_view name)
	{
		/*
		 * I don't know how programmatically solvable this is, maybe apple has grammar rules for this but
		 *
			100045910  extern id +[NSScanner scannerWithString:](Class self, SEL sel, int64_t scannerWithString)
			100045918  extern id +[NSString localizedStringWithFormat:](Class self, SEL sel, int64_t localizedStringWithFormat)
			100045920  extern id +[NSString stringWithCString:encoding:](Class self, SEL sel, int64_t stringWithCString, int64_t encoding)
			100045928  extern id +[NSString stringWithCharacters:length:](Class self, SEL sel, int64_t stringWithCharacters, int64_t length)
			100045930  extern id +[NSString stringWithFormat:](Class self, SEL sel, int64_t stringWithFormat)
			100045938  extern id +[NSString stringWithUTF8String:](Class self, SEL sel, int64_t stringWithUTF8String)
			100045940  extern id -[NSString drawInRect:withAttributes:](NSString* self, SEL sel, int64_t drawInRect, int64_t attributes)
			100045948  extern NSString* -[NSString initWithCString:encoding:](NSString* self, SEL sel, int64_t CString, int64_t encoding)
			100045950  extern NSString* -[NSString initWithCharactersNoCopy:length:freeWhenDone:](NSString* self, SEL sel, int64_t charactersNoCopy, int64_t length, int64_t freeWhenDone)
			100045958  extern id -[NSString isEqualToString:](NSString* self, SEL sel, int64_t isEqualToString)
			100045960  extern uint64_t -[NSString length](void* self, char* _cmd)
			100045968  extern id -[NSString localizedCaseInsensitiveCompare:](NSString* self, SEL sel, int64_t localizedCaseInsensitiveCompare)
			100045970  extern id -[NSString stringByAppendingFormat:](NSString* self, SEL sel, int64_t stringByAppendingFormat)
			100045978  extern id -[NSString stringByAppendingPathComponent:](NSString* self, SEL sel, int64_t stringByAppendingPathComponent)
			100045980  extern id -[NSString stringByAppendingString:](NSString* self, SEL sel, int64_t stringByAppendingString)
			100045988  extern id -[NSString stringByReplacingOccurrencesOfString:withString:](NSString* self, SEL sel, int64_t stringByReplacingOccurrencesOfString, int64_t string)
			100045990  extern id -[NSString substringFromIndex:](NSString* self, SEL sel, int64_t substringFromIndex)
			100045998  extern id +[NSValue valueWithRange:](Class self, SEL sel, int64_t valueWithRange)

			There is also this pattern for stuff.
		 */
		return name == "alloc" || name == "new" || name.starts_with("allocWith") || name.starts_with("newWith");
	}

	std::optional<std::string> ClassNameFromObjCMethodSymbolName(std::string_view symbolName)
	{
		if (symbolName.size() < 5 || (symbolName.front() != '-' && symbolName.front() != '+') ||
		    symbolName[1] != '[' || symbolName.back() != ']')
			return std::nullopt;

		size_t separator = symbolName.find(' ', 2);
		if (separator == std::string_view::npos || separator <= 2)
			return std::nullopt;

		return std::string(symbolName.substr(2, separator - 2));
	}

	std::optional<std::string> ClassNameFromClassObjectAddress(BinaryView* view, uint64_t classAddress)
	{
		if (!view || classAddress == 0)
			return std::nullopt;

		auto symbol = view->GetSymbolByAddress(classAddress);
		if (!symbol)
			return std::nullopt;

		std::string symbolName = symbol->GetFullName();
		if (symbolName.starts_with("clsRef_") || symbolName.starts_with("superRef_"))
			return std::nullopt;
		if (auto className = ClassNameFromSymbolName(symbolName))
			return std::string(*className);
		return std::nullopt;
	}

	std::optional<uint64_t> ClassObjectAddressFromClassName(BinaryView* view, std::string_view className)
	{
		if (!view || className.empty())
			return std::nullopt;

		for (const std::string name : {
		         std::string("cls_") + std::string(className),
		         std::string("_OBJC_CLASS_$_") + std::string(className),
		     })
		{
			for (const auto& nameSpace : {
			         NameSpace(), BinaryView::GetInternalNameSpace(), BinaryView::GetExternalNameSpace(),
			     })
			{
				auto symbol = view->GetSymbolByRawName(name, nameSpace);
				if (!symbol || symbol->GetAddress() == 0)
					continue;

				if (auto resolvedName = ClassNameFromClassObjectAddress(view, symbol->GetAddress()))
				{
					if (*resolvedName == className)
						return symbol->GetAddress();
				}
			}
		}

		return std::nullopt;
	}

	std::optional<uint64_t> ClassObjectAddressFromClassReferenceAddress(BinaryView* view, uint64_t address)
	{
		if (!view || address == 0)
			return std::nullopt;

		auto symbol = view->GetSymbolByAddress(address);
		if (!symbol)
			return std::nullopt;

		std::string symbolName = symbol->GetFullName();
		if (symbolName.starts_with("clsRef_") || symbolName.starts_with("superRef_"))
		{
			if (auto classObjectAddress = ReadPointer(view, address))
			{
				if (ClassNameFromClassObjectAddress(view, *classObjectAddress))
					return classObjectAddress;
			}
			if (auto className = ClassNameFromSymbolName(symbolName))
				return ClassObjectAddressFromClassName(view, *className);
			return std::nullopt;
		}

		if (ClassNameFromClassObjectAddress(view, address))
			return address;
		return std::nullopt;
	}

	std::optional<std::string> ClassNameFromClassReferenceAddress(BinaryView* view, uint64_t address)
	{
		if (!view || address == 0)
			return std::nullopt;

		if (auto symbol = view->GetSymbolByAddress(address))
		{
			std::string symbolName = symbol->GetFullName();
			if (auto className = ClassNameFromSymbolName(symbolName))
				return std::string(*className);
		}

		auto classObjectAddress = ClassObjectAddressFromClassReferenceAddress(view, address);
		if (!classObjectAddress)
			return std::nullopt;
		return ClassNameFromClassReferenceAddress(view, *classObjectAddress);
	}

	std::optional<std::string> SuperclassNameFromClassObjectAddress(BinaryView* view, uint64_t classAddress)
	{
		auto superclassAddress = ReadPointer(view, classAddress + (view ? view->GetAddressSize() : 0));
		if (!superclassAddress || *superclassAddress == 0)
			return std::nullopt;

		return ClassNameFromClassReferenceAddress(view, *superclassAddress);
	}

	std::optional<std::string> SuperclassNameFromClassName(BinaryView* view, std::string_view className)
	{
		auto classObjectAddress = ClassObjectAddressFromClassName(view, className);
		if (!classObjectAddress)
			return std::nullopt;

		return SuperclassNameFromClassObjectAddress(view, *classObjectAddress);
	}

	Ref<Type> TypeLibraryObjectType(BinaryView* view, std::string_view name, std::optional<uint64_t> address)
	{
		if (!view || name.empty())
			return nullptr;

		auto importObject = [&](const std::string& objectName) -> Ref<Type> {
			QualifiedName qualifiedName {objectName};
			Ref<TypeLibrary> typeLibrary = nullptr;
			Ref<Type> type = view->ImportTypeLibraryObject(typeLibrary, qualifiedName);
			if (type && typeLibrary && address)
			{
				if (auto platform = view->GetDefaultPlatform())
					view->RecordImportedObjectLibrary(platform, *address, typeLibrary, qualifiedName);
			}

			return type;
		};

		std::string objectName(name);
		if (auto type = importObject(objectName))
			return type;

		if (objectName.starts_with('_'))
			return nullptr;

		return importObject("_" + objectName);
	}

	Ref<Type> NamedType(BinaryView* view, std::string_view name)
	{
		if (!view || name.empty())
			return nullptr;

		QualifiedName qualifiedName {std::string(name)};
		if (view->GetTypeByName(qualifiedName))
			return Type::NamedType(view, qualifiedName);

		Ref<TypeLibrary> typeLibrary = nullptr;
		return view->ImportTypeLibraryType(typeLibrary, qualifiedName);
	}

	Ref<Type> ClassInstanceType(BinaryView* view, std::string_view name)
	{
		if (!view || name.empty())
			return nullptr;

		QualifiedName qualifiedName {std::string(name)};
		if (auto type = view->GetTypeByName(qualifiedName))
			return type;

		return NamedType(view, name);
	}

	std::vector<std::string> GenerateArgumentNames(const std::vector<std::string>& labels)
	{
		std::vector<std::string> result;
		result.reserve(labels.size());
		for (const auto& label : labels)
			result.push_back(ArgumentNameFromSelectorLabel(label));
		return result;
	}

	void AdjustReturnTypeOfCall(const Call& call, Type* returnType, uint8_t confidence)
	{
		auto function = call.instr.function ? call.instr.function->GetFunction() : nullptr;
		if (!function)
			return;

		auto arch = function->GetArchitecture();

		Ref<Type> targetFunctionType;
		auto existingAdjustment = function->GetCallTypeAdjustment(arch, call.instr.address);
		if (!existingAdjustment.IsUnknown())
			targetFunctionType = existingAdjustment.GetValue();
		if (!targetFunctionType)
			targetFunctionType = call.targetType;
		if (!targetFunctionType && call.target)
			targetFunctionType = call.target->GetType();
		if (!targetFunctionType || !targetFunctionType->IsFunction())
			return;

		Ref<Type> existingReturn = targetFunctionType->GetReturnValue().type.GetValue();
		if (existingReturn && *existingReturn == *returnType)
			return;

		TypeBuilder builder(targetFunctionType);
		builder.SetReturnValue(ReturnValue(Confidence<Ref<Type>>(returnType, confidence)));
		function->SetAutoCallTypeAdjustment(
		    arch, call.instr.address, Confidence<Ref<Type>>(builder.Finalize(), confidence));
	}
}

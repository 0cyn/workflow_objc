//
// LLM Generated Test Boilerplate stuff
// General "here be dragons" warning but it's mostly fine.
//

#include "Metadata.h"
#include "Util.h"
#include "Workflow.h"

#include <binaryninjaapi.h>
#include <gtest/gtest.h>
#include <highlevelilinstruction.h>
#include <lowlevelilinstruction.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	bool Equals(std::optional<std::string_view> actual, std::string_view expected)
	{
		return actual && *actual == expected;
	}

	class BinaryNinjaCoreTest : public testing::Test
	{
	  protected:
		static void SetUpTestSuite()
		{
			static bool initialized = [] {
				BinaryNinja::SetBundledPluginDirectory(BinaryNinja::GetBundledPluginDirectory());
				return BinaryNinja::InitPlugins(false);
			}();

			if (!initialized)
				GTEST_SKIP() << "Binary Ninja core plugins could not be initialized";
		}
	};

	bool ContainsAll(
	    const std::vector<std::string>& values, const std::vector<std::string_view>& expectedValues)
	{
		return std::all_of(expectedValues.begin(), expectedValues.end(), [&](std::string_view expected) {
			return std::find(values.begin(), values.end(), expected) != values.end();
		});
	}

	BinaryNinja::Ref<BinaryNinja::Type> AdjustedCallReturnType(
	    BinaryNinja::Function* func, uint64_t address)
	{
		auto adjustment = func->GetCallTypeAdjustment(func->GetArchitecture(), address);
		if (adjustment.IsUnknown() || !adjustment.GetValue())
			return nullptr;

		auto returnType = adjustment.GetValue()->GetReturnValue().type;
		if (returnType.IsUnknown())
			return nullptr;

		return returnType.GetValue();
	}

	std::optional<uint64_t> ConstantLikeValue(const BinaryNinja::PossibleValueSet& value)
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

	std::optional<uint64_t> ConstantLikeValue(const BinaryNinja::LowLevelILInstruction& expr)
	{
		if (expr.operation == LLIL_CONST || expr.operation == LLIL_CONST_PTR)
			return static_cast<uint64_t>(expr.GetConstant());
		return ConstantLikeValue(expr.GetPossibleValues());
	}

	std::optional<std::string> DirectCallTargetNameAtAddress(
	    BinaryNinja::BinaryView* view, BinaryNinja::Function* func, uint64_t address)
	{
		auto llil = func->GetLowLevelILIfAvailable();
		if (!llil)
			return std::nullopt;

		for (size_t i = 0; i < llil->GetInstructionCount(); ++i)
		{
			auto instr = llil->GetInstruction(i);
			if (instr.address != address || (instr.operation != LLIL_CALL && instr.operation != LLIL_TAILCALL))
				continue;

			auto target = ConstantLikeValue(instr.GetDestExpr());
			if (!target)
				return std::nullopt;

			auto symbol = view->GetSymbolByAddress(*target);
			if (!symbol)
				return std::nullopt;
			return symbol->GetRawName();
		}

		return std::nullopt;
	}

	bool HasDirectCallToSymbol(BinaryNinja::BinaryView* view, std::string_view name)
	{
		auto symbol = view->GetSymbolByRawName(std::string(name), BinaryNinja::BinaryView::GetInternalNameSpace());
		if (!symbol)
			return false;

		for (const auto& func : view->GetAnalysisFunctionList())
		{
			auto llil = func->GetLowLevelILIfAvailable();
			if (!llil)
				continue;

			for (size_t i = 0; i < llil->GetInstructionCount(); ++i)
			{
				auto instr = llil->GetInstruction(i);
				if (instr.operation != LLIL_CALL && instr.operation != LLIL_TAILCALL)
					continue;

				auto target = ConstantLikeValue(instr.GetDestExpr());
				if (target && *target == symbol->GetAddress())
					return true;
			}
		}

		return false;
	}

	bool HasCodeReferenceToSymbolFromAddress(
	    BinaryNinja::BinaryView* view, std::string_view name, uint64_t address)
	{
		auto symbol = view->GetSymbolByRawName(std::string(name), BinaryNinja::BinaryView::GetInternalNameSpace());
		if (!symbol)
			return false;

		for (const auto& ref : view->GetCodeReferences(symbol->GetAddress()))
		{
			if (ref.addr == address)
				return true;
		}

		return false;
	}

	std::string TypeString(BinaryNinja::BinaryView* view, BinaryNinja::Type* type)
	{
		return type ? type->GetString(view->GetDefaultPlatform()) : "";
	}

	std::string LineText(const BinaryNinja::DisassemblyTextLine& line)
	{
		std::string result;
		for (const auto& token : line.tokens)
			result += token.text;
		return result;
	}

	std::string HLILDiagnostics(BinaryNinja::BinaryView* view, BinaryNinja::Function* func, uint64_t start, uint64_t end)
	{
		std::ostringstream stream;
		auto hlil = func->GetHighLevelIL();
		if (!hlil)
			return "<no hlil>";

		for (size_t i = 0; i < hlil->GetInstructionCount(); ++i)
		{
			auto instr = hlil->GetInstruction(i);
			if (instr.address < start || instr.address > end)
				continue;

			stream << "hlil[" << i << "] op=" << static_cast<int>(instr.operation) << " addr=0x" << std::hex
			       << instr.address << std::dec << " expr=" << instr.exprIndex << ": ";
			for (const auto& line : hlil->GetInstructionText(i))
				stream << LineText(line);
			stream << "\n";
		}

		for (const auto& var : hlil->GetVariables())
		{
			auto type = func->GetVariableType(var);
			stream << "var " << func->GetVariableNameOrDefault(var) << " id=" << var.ToIdentifier()
			       << " type=" << (type.IsUnknown() ? "<unknown>" : TypeString(view, type.GetValue())) << "\n";
		}

		return stream.str();
	}

	std::string HLILTextInRange(BinaryNinja::Function* func, uint64_t start, uint64_t end)
	{
		std::string result;
		auto hlil = func->GetHighLevelIL();
		if (!hlil)
			return result;

		for (size_t i = 0; i < hlil->GetInstructionCount(); ++i)
		{
			auto instr = hlil->GetInstruction(i);
			if (instr.address < start || instr.address > end)
				continue;

			for (const auto& line : hlil->GetInstructionText(i))
			{
				result += LineText(line);
				result += '\n';
			}
		}

		return result;
	}

	bool HasHLILVariableTypeInitializedInRange(
	    BinaryNinja::BinaryView* view, BinaryNinja::Function* func, uint64_t start, uint64_t end,
	    std::string_view expectedType)
	{
		auto hlil = func->GetHighLevelIL();
		if (!hlil)
			return false;

		for (size_t i = 0; i < hlil->GetInstructionCount(); ++i)
		{
			auto instr = hlil->GetInstruction(i);
			if (instr.address < start || instr.address > end || instr.operation != HLIL_VAR_INIT)
				continue;

			auto type = func->GetVariableType(instr.GetDestVariable<HLIL_VAR_INIT>());
			if (!type.IsUnknown() && TypeString(view, type.GetValue()) == expectedType)
				return true;
		}

		return false;
	}

	std::optional<BinaryNinja::Variable> VariableFromHLILExpr(const BinaryNinja::HighLevelILInstruction& expr)
	{
		if (expr.operation == HLIL_VAR)
			return expr.GetVariable<HLIL_VAR>();
		if (expr.operation == HLIL_VAR_SSA)
			return expr.GetSSAVariable<HLIL_VAR_SSA>().var;
		return std::nullopt;
	}

	std::optional<BinaryNinja::Variable> AssignedVariableFromHLILInstruction(
	    const BinaryNinja::HighLevelILInstruction& instr)
	{
		if (instr.operation == HLIL_VAR_INIT)
			return instr.GetDestVariable<HLIL_VAR_INIT>();
		if (instr.operation == HLIL_ASSIGN_UNPACK)
		{
			auto dests = instr.GetDestExprs<HLIL_ASSIGN_UNPACK>();
			if (dests.size() == 0)
				return std::nullopt;
			return VariableFromHLILExpr(dests[0]);
		}
		if (instr.operation == HLIL_ASSIGN)
			return VariableFromHLILExpr(instr.GetDestExpr<HLIL_ASSIGN>());

		return std::nullopt;
	}

	bool HasHLILVariableTypeAssignedInRange(
	    BinaryNinja::BinaryView* view, BinaryNinja::Function* func, uint64_t start, uint64_t end,
	    std::string_view expectedType)
	{
		auto hlil = func->GetHighLevelIL();
		if (!hlil)
			return false;

		for (size_t i = 0; i < hlil->GetInstructionCount(); ++i)
		{
			auto instr = hlil->GetInstruction(i);
			if (instr.address < start || instr.address > end)
				continue;

			auto variable = AssignedVariableFromHLILInstruction(instr);
			if (!variable)
				continue;

			auto type = func->GetVariableType(*variable);
			if (!type.IsUnknown() && TypeString(view, type.GetValue()) == expectedType)
				return true;
		}

		return false;
	}

	bool StructHasMemberAtOffset(BinaryNinja::BinaryView* view, std::string_view typeName, uint64_t offset)
	{
		auto type = view->GetTypeByName(BinaryNinja::QualifiedName(std::string(typeName)));
		if (!type || !type->IsStructure())
			return false;

		BinaryNinja::StructureMember member;
		return type->GetStructure()->GetMemberAtOffset(static_cast<int64_t>(offset), member);
	}

	std::optional<uint64_t> ObjCExternSymbolAddress(BinaryNinja::BinaryView* view, std::string_view name)
	{
		auto section = view->GetSectionByName(".objc-externs");
		if (!section)
			return std::nullopt;

		auto symbol = view->GetSymbolByRawName(std::string(name), BinaryNinja::BinaryView::GetInternalNameSpace());
		if (!symbol || symbol->GetType() != SymbolicFunctionSymbol)
			return std::nullopt;

		uint64_t address = symbol->GetAddress();
		if (address < section->GetStart() || address >= section->GetEnd() || !view->IsOffsetExternSemantics(address))
			return std::nullopt;

		auto symbolAtAddress = view->GetSymbolByAddress(address);
		if (!symbolAtAddress || symbolAtAddress->GetRawName() != name)
			return std::nullopt;

		BinaryNinja::DataVariable dataVariable;
		if (!view->GetDataVariableAtAddress(address, dataVariable))
			return std::nullopt;

		auto type = dataVariable.type.GetValue();
		if (!type || !type->IsFunction())
			return std::nullopt;

		return address;
	}

	bool HasObjCExternSymbol(BinaryNinja::BinaryView* view, std::string_view name)
	{
		return ObjCExternSymbolAddress(view, name).has_value();
	}

	std::optional<std::string> SymbolExternalLibraryName(BinaryNinja::BinaryView* view, std::string_view name)
	{
		auto symbol = view->GetSymbolByRawName(std::string(name), BinaryNinja::BinaryView::GetExternalNameSpace());
		if (symbol)
		{
			auto location = view->GetExternalLocation(symbol);
			if (location)
			{
				auto library = location->GetExternalLibrary();
				if (library)
					return library->GetName();
			}
		}

		auto mapping = view->QueryMetadata("SymbolExternalLibraryMapping");
		if (!mapping || mapping->GetType() != KeyValueDataType)
			return std::nullopt;

		auto value = mapping->Get(std::string(name));
		if (!value || value->GetType() != StringDataType)
			return std::nullopt;

		return value->GetString();
	}

	std::optional<std::string> SymbolExternalLibraryMappingName(BinaryNinja::BinaryView* view, std::string_view name)
	{
		auto mapping = view->QueryMetadata("SymbolExternalLibraryMapping");
		if (!mapping || mapping->GetType() != KeyValueDataType)
			return std::nullopt;

		auto value = mapping->Get(std::string(name));
		if (!value || value->GetType() != StringDataType)
			return std::nullopt;

		return value->GetString();
	}

	std::optional<std::string> ObjCExternExternalLibraryName(BinaryNinja::BinaryView* view, std::string_view name)
	{
		auto symbol = view->GetSymbolByRawName(std::string(name), BinaryNinja::BinaryView::GetInternalNameSpace());
		if (!symbol)
			return std::nullopt;

		auto location = view->GetExternalLocation(symbol);
		if (!location)
			return std::nullopt;

		auto library = location->GetExternalLibrary();
		if (!library)
			return std::nullopt;

		return library->GetName();
	}

	std::string ObjCSymbolDiagnostics(BinaryNinja::BinaryView* view)
	{
		std::ostringstream stream;
		auto section = view->GetSectionByName(".objc-externs");
		if (section)
		{
			stream << ".objc-externs=[0x" << std::hex << section->GetStart() << ",0x" << section->GetEnd()
			       << ") ";
		}

		for (std::string_view expected : {
		         "+[NSString stringWithFormat:]",
		         "+[NSBundle mainBundle]",
		         "-[NSBundle localizedStringForKey:value:table:]",
		         "+[NSArray arrayWithObjects:count:]",
		         "-[NSTextField setTextColor:]",
		         "-[NSWindow isVisible]",
		     })
		{
			auto symbol = view->GetSymbolByRawName(std::string(expected), BinaryNinja::BinaryView::GetInternalNameSpace());
			if (symbol)
			{
				stream << expected << "@0x" << std::hex << symbol->GetAddress()
			       << ":type=" << std::dec << static_cast<int>(symbol->GetType())
			       << ":extern=" << view->IsOffsetExternSemantics(symbol->GetAddress()) << " ";
			}
		}

		stream << "objc-like=";
		size_t count = 0;
		for (const auto& symbol : view->GetSymbols())
		{
			std::string name = symbol->GetRawName();
			if ((name.starts_with("+[") || name.starts_with("-[")) && count++ < 30)
				stream << name << "@0x" << std::hex << symbol->GetAddress() << ",";
		}

		return stream.str();
	}
}

TEST(AddressRange, ContainsUsesHalfOpenRange)
{
	WorkflowObjC::AddressRange range {0x1000, 0x2000};
	EXPECT_FALSE(range.Contains(0x0fff));
	EXPECT_TRUE(range.Contains(0x1000));
	EXPECT_TRUE(range.Contains(0x1800));
	EXPECT_FALSE(range.Contains(0x2000));
}

TEST(AddressRange, EmptyRangeContainsNoValues)
{
	WorkflowObjC::AddressRange range {0x1000, 0x1000};
	EXPECT_FALSE(range.Contains(0x0fff));
	EXPECT_FALSE(range.Contains(0x1000));
	EXPECT_FALSE(range.Contains(0x1001));
}

TEST(ClassNameFromSymbolName, ExtractsObjectiveCClassNames)
{
	EXPECT_TRUE(Equals(WorkflowObjC::ClassNameFromSymbolName("cls_NSString"), "NSString"));
	EXPECT_TRUE(Equals(WorkflowObjC::ClassNameFromSymbolName("clsRef_NSObject"), "NSObject"));
	EXPECT_TRUE(Equals(WorkflowObjC::ClassNameFromSymbolName("superRef_LCDController"), "LCDController"));
	EXPECT_TRUE(Equals(WorkflowObjC::ClassNameFromSymbolName("_OBJC_CLASS_$_MyClass"), "MyClass"));
}

TEST(ClassNameFromSymbolName, RejectsNonClassSymbols)
{
	EXPECT_FALSE(WorkflowObjC::ClassNameFromSymbolName("_OBJC_METACLASS_$_MyClass"));
	EXPECT_FALSE(WorkflowObjC::ClassNameFromSymbolName("sel_init"));
	EXPECT_FALSE(WorkflowObjC::ClassNameFromSymbolName("_objc_msgSend"));
}

TEST(ClassNameFromObjCMethodSymbolName, ExtractsMethodClassNames)
{
	EXPECT_EQ(
	    WorkflowObjC::ClassNameFromObjCMethodSymbolName("-[LCDController init]"),
	    std::optional<std::string>("LCDController"));
	EXPECT_EQ(
	    WorkflowObjC::ClassNameFromObjCMethodSymbolName("+[NSString stringWithFormat:]"),
	    std::optional<std::string>("NSString"));
}

TEST(ClassNameFromObjCMethodSymbolName, RejectsNonMethodSymbols)
{
	EXPECT_FALSE(WorkflowObjC::ClassNameFromObjCMethodSymbolName("_objc_msgSend"));
	EXPECT_FALSE(WorkflowObjC::ClassNameFromObjCMethodSymbolName("cls_LCDController"));
}

TEST(Selector, IdentifiesInitFamilySelectors)
{
	EXPECT_TRUE((WorkflowObjC::Selector {"init", 0}.IsInitFamily()));
	EXPECT_TRUE((WorkflowObjC::Selector {"init:", 0}.IsInitFamily()));
	EXPECT_TRUE((WorkflowObjC::Selector {"initWithFrame:", 0}.IsInitFamily()));
	EXPECT_TRUE((WorkflowObjC::Selector {"initURL:", 0}.IsInitFamily()));
}

TEST(Selector, RejectsNonInitFamilySelectors)
{
	EXPECT_FALSE((WorkflowObjC::Selector {"initialize", 0}.IsInitFamily()));
	EXPECT_FALSE((WorkflowObjC::Selector {"initiate", 0}.IsInitFamily()));
	EXPECT_FALSE((WorkflowObjC::Selector {"init_withValue:", 0}.IsInitFamily()));
	EXPECT_FALSE((WorkflowObjC::Selector {"copy", 0}.IsInitFamily()));
}

TEST(Selector, SplitsArgumentLabels)
{
	WorkflowObjC::Selector noArgs {"description", 0};
	EXPECT_TRUE(noArgs.ArgumentLabels().empty());

	WorkflowObjC::Selector oneArg {"setObject:", 0};
	EXPECT_EQ(oneArg.ArgumentLabels(), (std::vector<std::string> {"setObject"}));

	WorkflowObjC::Selector manyArgs {"initWithFrame:style:reuseIdentifier:", 0};
	EXPECT_EQ(
	    manyArgs.ArgumentLabels(),
	    (std::vector<std::string> {"initWithFrame", "style", "reuseIdentifier"}));
}

TEST(Selector, IgnoresEmptyArgumentSegments)
{
	WorkflowObjC::Selector leadingColon {":setObject:", 0};
	EXPECT_EQ(leadingColon.ArgumentLabels(), (std::vector<std::string> {"setObject"}));

	WorkflowObjC::Selector emptySegments {"performSelector::afterDelay:", 0};
	EXPECT_EQ(emptySegments.ArgumentLabels(), (std::vector<std::string> {"performSelector", "afterDelay"}));
}

TEST(GlobalState, NullViewsAreIgnored)
{
	EXPECT_TRUE(WorkflowObjC::GlobalState::ShouldIgnoreView(nullptr));
	EXPECT_EQ(WorkflowObjC::GlobalState::GetAnalysisInfo(nullptr), nullptr);
	EXPECT_FALSE(WorkflowObjC::AnalysisInfo::HasMetadata(nullptr));
}

TEST_F(BinaryNinjaCoreTest, RegistersObjectiveCWorkflowActivities)
{
	auto baseWorkflow = BinaryNinja::Workflow::Get("core.function.metaAnalysis");
	ASSERT_TRUE(baseWorkflow) << "Binary Ninja did not expose the core function workflow";

	ASSERT_TRUE(WorkflowObjC::RegisterActivities());

	auto workflow = BinaryNinja::Workflow::Get("core.function.metaAnalysis");
	ASSERT_TRUE(workflow);

	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.analyzeMessageSends"));
	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.discoverTypedExterns"));
	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.inlineStubs"));
	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.types.allocInit"));
	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.types.ivarGetter"));
	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.types.retain"));
	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.types.superInit"));
	EXPECT_TRUE(workflow->Contains("core.function.objectiveC.removeMemoryManagement"));

	auto subactivities = workflow->GetSubactivities("", false);
	EXPECT_TRUE(ContainsAll(subactivities, {
	    "core.function.objectiveC.analyzeMessageSends",
	    "core.function.objectiveC.discoverTypedExterns",
	    "core.function.objectiveC.inlineStubs",
	    "core.function.objectiveC.types.allocInit",
	    "core.function.objectiveC.types.ivarGetter",
	    "core.function.objectiveC.types.retain",
	    "core.function.objectiveC.types.superInit",
	    "core.function.objectiveC.removeMemoryManagement",
	}));
}

TEST_F(BinaryNinjaCoreTest, NamedTypeImportsMissingTypeFromViewTypeLibrary)
{
	auto arch = BinaryNinja::Architecture::GetByName("x86_64");
	ASSERT_TRUE(arch);

	BinaryNinja::Ref<BinaryNinja::FileMetadata> file = new BinaryNinja::FileMetadata();
	BinaryNinja::Ref<BinaryNinja::BinaryView> view = new BinaryNinja::BinaryData(file);
	view->SetDefaultArchitecture(arch);

	BinaryNinja::QualifiedName name("WorkflowObjCTestType");
	ASSERT_FALSE(view->GetTypeByName(name));

	BinaryNinja::Ref<BinaryNinja::TypeLibrary> typeLibrary = new BinaryNinja::TypeLibrary(arch, "workflow_objc_test");
	typeLibrary->AddNamedType(name, BinaryNinja::Type::IntegerType(4, BinaryNinja::Confidence<bool>(false)));
	typeLibrary->Finalize();
	view->AddTypeLibrary(typeLibrary);

	auto namedType = WorkflowObjC::NamedType(view, name.GetString());
	ASSERT_TRUE(namedType);
	EXPECT_TRUE(namedType->IsNamedTypeRefer());
	EXPECT_TRUE(view->GetTypeByName(name));
}

TEST_F(BinaryNinjaCoreTest, TypeLibraryObjectImportsMatchingSymbolType)
{
	auto platform = BinaryNinja::Platform::GetByName("mac-x86_64");
	ASSERT_TRUE(platform);
	auto arch = platform->GetArchitecture();
	ASSERT_TRUE(arch);

	BinaryNinja::Ref<BinaryNinja::FileMetadata> file = new BinaryNinja::FileMetadata();
	BinaryNinja::Ref<BinaryNinja::BinaryView> view = new BinaryNinja::BinaryData(file);
	view->SetDefaultPlatform(platform);
	view->SetDefaultArchitecture(arch);

	BinaryNinja::QualifiedName name("-[WorkflowObjCTestClass typedExtern:]");
	BinaryNinja::QualifiedName libraryObjectName("_" + name.GetString());
	auto returnType = BinaryNinja::Type::IntegerType(4, BinaryNinja::Confidence<bool>(false));
	auto objectType = BinaryNinja::Type::FunctionType(
	    BinaryNinja::ReturnValue(BinaryNinja::Confidence<BinaryNinja::Ref<BinaryNinja::Type>>(returnType, BN_FULL_CONFIDENCE)),
	    BinaryNinja::Confidence<BinaryNinja::Ref<BinaryNinja::CallingConvention>>(nullptr, 0), {},
	    BinaryNinja::Confidence<bool>(false));

	BinaryNinja::Ref<BinaryNinja::TypeLibrary> typeLibrary = new BinaryNinja::TypeLibrary(arch, "workflow_objc_objects_test");
	typeLibrary->AddPlatform(platform);
	typeLibrary->AddNamedObject(libraryObjectName, objectType);
	typeLibrary->Finalize();
	view->AddTypeLibrary(typeLibrary);

	constexpr uint64_t address = 0x1000;
	auto importedType = WorkflowObjC::TypeLibraryObjectType(view, name.GetString(), address);
	ASSERT_TRUE(importedType);
	EXPECT_TRUE(importedType->IsFunction());
	EXPECT_TRUE(*importedType == *objectType);

	auto importedObject = view->LookupImportedObjectLibrary(platform, address);
	ASSERT_TRUE(importedObject);
	EXPECT_EQ(importedObject->first->GetName(), typeLibrary->GetName());
	EXPECT_EQ(importedObject->second.GetString(), libraryObjectName.GetString());
}

TEST_F(BinaryNinjaCoreTest, RefinesSimpleObjCIvarGetterReturnType)
{
	ASSERT_TRUE(WorkflowObjC::RegisterActivities());

	auto path = std::filesystem::path(__FILE__).parent_path() / "bin" / "Calculator.test";
	auto view = BinaryNinja::Load(path.string(), true);
	ASSERT_TRUE(view) << "failed to load " << path;
	view->UpdateAnalysisAndWait();

	auto func = view->GetAnalysisFunction(view->GetDefaultPlatform(), 0x100006b2d);
	ASSERT_TRUE(func);

	auto returnType = func->GetReturnType();
	ASSERT_FALSE(returnType.IsUnknown());
	ASSERT_TRUE(returnType.GetValue());
	EXPECT_EQ(returnType.GetValue()->GetString(view->GetDefaultPlatform()), "NSColor*");

	view->GetFile()->Close();
}

TEST_F(BinaryNinjaCoreTest, PropagatesObjCMsgSendReturnTypesThroughRetainCalls)
{
	ASSERT_TRUE(WorkflowObjC::RegisterActivities());

	auto path = std::filesystem::path(__FILE__).parent_path() / "bin" / "Calculator.test";
	auto view = BinaryNinja::Load(path.string(), true);
	ASSERT_TRUE(view) << "failed to load " << path;
	view->UpdateAnalysisAndWait();

	auto func = view->GetAnalysisFunction(view->GetDefaultPlatform(), 0x1000063bf);
	ASSERT_TRUE(func);

	auto textColorReturn = AdjustedCallReturnType(func, 0x10000649f);
	ASSERT_TRUE(textColorReturn);
	EXPECT_EQ(TypeString(view, textColorReturn), "NSColor*");

	view->GetFile()->Close();
}

TEST_F(BinaryNinjaCoreTest, CreatesObjCExternsForCalculatorExternalMessageSends)
{
	ASSERT_TRUE(WorkflowObjC::RegisterActivities());

	auto path = std::filesystem::path(__FILE__).parent_path() / "bin" / "Calculator.test";
	auto view = BinaryNinja::Load(path.string(), true);
	ASSERT_TRUE(view) << "failed to load " << path;
	view->UpdateAnalysisAndWait();

	auto section = view->GetSectionByName(".objc-externs");
	ASSERT_TRUE(section);
	EXPECT_EQ(section->GetSemantics(), ExternalSectionSemantics);
	EXPECT_GT(section->GetEnd(), section->GetStart());

	auto diagnostics = ObjCSymbolDiagnostics(view);
	auto info = WorkflowObjC::GlobalState::GetAnalysisInfo(view);
	ASSERT_TRUE(info);
	EXPECT_EQ(info->GetIvarClassName(view, "LCDController", 0x40), std::optional<std::string>("NSTextField"));
	EXPECT_EQ(
	    WorkflowObjC::SuperclassNameFromClassName(view, "LCDController"),
	    std::optional<std::string>("NSObject"));

	auto superRef = view->GetSymbolByRawName("superRef_LCDController");
	ASSERT_TRUE(superRef);
	EXPECT_EQ(
	    WorkflowObjC::ClassNameFromClassReferenceAddress(view, superRef->GetAddress()),
	    std::optional<std::string>("LCDController"));

	auto initFunc = view->GetAnalysisFunction(view->GetDefaultPlatform(), 0x100006d90);
	ASSERT_TRUE(initFunc);
	auto initReturn = initFunc->GetReturnType();
	ASSERT_FALSE(initReturn.IsUnknown());
	ASSERT_TRUE(initReturn.GetValue());
	EXPECT_EQ(TypeString(view, initReturn.GetValue()), "struct LCDController*");

	auto superInitReturn = AdjustedCallReturnType(initFunc, 0x100006dbd);
	ASSERT_TRUE(superInitReturn);
	EXPECT_EQ(TypeString(view, superInitReturn), "struct LCDController*");
	EXPECT_TRUE(HasHLILVariableTypeAssignedInRange(
	    view, initFunc, 0x100006dbd, 0x100006dbd, "struct LCDController*"));

	EXPECT_TRUE(HasObjCExternSymbol(view, "+[NSString stringWithFormat:]")) << diagnostics;
	EXPECT_TRUE(HasObjCExternSymbol(view, "+[NSBundle mainBundle]")) << diagnostics;
	EXPECT_TRUE(HasObjCExternSymbol(view, "+[NSArray arrayWithObjects:count:]")) << diagnostics;
	EXPECT_TRUE(HasObjCExternSymbol(view, "-[NSTextField setHidden:]")) << diagnostics;
	auto nsArrayLibrary = SymbolExternalLibraryName(view, "_OBJC_CLASS_$_NSArray");
	auto nsBundleLibrary = SymbolExternalLibraryName(view, "_OBJC_CLASS_$_NSBundle");
	auto nsStringLibrary = SymbolExternalLibraryName(view, "_OBJC_CLASS_$_NSString");
	ASSERT_TRUE(nsArrayLibrary) << diagnostics;
	ASSERT_TRUE(nsBundleLibrary) << diagnostics;
	ASSERT_TRUE(nsStringLibrary) << diagnostics;
	ASSERT_LT(*nsArrayLibrary, *nsBundleLibrary);
	ASSERT_EQ(nsBundleLibrary, nsStringLibrary);
	EXPECT_EQ(SymbolExternalLibraryMappingName(view, "+[NSArray arrayWithObjects:count:]"), nsArrayLibrary);
	EXPECT_EQ(SymbolExternalLibraryMappingName(view, "+[NSBundle mainBundle]"), nsBundleLibrary);
	EXPECT_EQ(SymbolExternalLibraryMappingName(view, "+[NSString stringWithFormat:]"), nsStringLibrary);
	EXPECT_EQ(ObjCExternExternalLibraryName(view, "+[NSArray arrayWithObjects:count:]"), nsArrayLibrary);
	EXPECT_EQ(ObjCExternExternalLibraryName(view, "+[NSBundle mainBundle]"), nsBundleLibrary);
	EXPECT_EQ(ObjCExternExternalLibraryName(view, "+[NSString stringWithFormat:]"), nsStringLibrary);
	auto nsArrayExtern = ObjCExternSymbolAddress(view, "+[NSArray arrayWithObjects:count:]");
	auto nsBundleMainExtern = ObjCExternSymbolAddress(view, "+[NSBundle mainBundle]");
	auto nsBundleLocalizedExtern = ObjCExternSymbolAddress(view, "-[NSBundle localizedStringForKey:value:table:]");
	auto nsStringExtern = ObjCExternSymbolAddress(view, "+[NSString stringWithFormat:]");
	ASSERT_TRUE(nsArrayExtern) << diagnostics;
	ASSERT_TRUE(nsBundleMainExtern) << diagnostics;
	ASSERT_TRUE(nsBundleLocalizedExtern) << diagnostics;
	ASSERT_TRUE(nsStringExtern) << diagnostics;
	EXPECT_LT(*nsArrayExtern, *nsBundleMainExtern);
	EXPECT_LT(*nsBundleMainExtern, *nsBundleLocalizedExtern);
	EXPECT_LT(*nsBundleLocalizedExtern, *nsStringExtern);

	view->GetFile()->Close();

	view = BinaryNinja::Load(path.string(), true,
	    R"({"files.universal.architecturePreference":["arm64e","arm64"]})");
	ASSERT_TRUE(view) << "failed to load arm64e " << path;
	view->UpdateAnalysisAndWait();
	ASSERT_EQ(view->GetDefaultArchitecture()->GetName(), "aarch64");
	info = WorkflowObjC::GlobalState::GetAnalysisInfo(view);
	ASSERT_TRUE(info);

	initFunc = view->GetAnalysisFunction(view->GetDefaultPlatform(), 0x100007a18);
	ASSERT_TRUE(initFunc);
	initReturn = initFunc->GetReturnType();
	ASSERT_FALSE(initReturn.IsUnknown());
	ASSERT_TRUE(initReturn.GetValue());
	EXPECT_EQ(TypeString(view, initReturn.GetValue()), "struct LCDController*");

	superInitReturn = AdjustedCallReturnType(initFunc, 0x100007a48);
	ASSERT_TRUE(superInitReturn);
	EXPECT_EQ(TypeString(view, superInitReturn), "struct LCDController*");
	EXPECT_TRUE(HasHLILVariableTypeAssignedInRange(
	    view, initFunc, 0x100007a48, 0x100007a48, "struct LCDController*"))
	    << HLILDiagnostics(view, initFunc, 0x100007a40, 0x100007a58);
	EXPECT_EQ(info->GetIvarClassName(view, "LCDController", 0x30), std::optional<std::string>("NSWindow"));
	EXPECT_TRUE(HasObjCExternSymbol(view, "-[NSWindow isVisible]")) << ObjCSymbolDiagnostics(view);
	auto showRPNViewsFunc = view->GetAnalysisFunction(view->GetDefaultPlatform(), 0x100008f84);
	ASSERT_TRUE(showRPNViewsFunc);
	EXPECT_EQ(
	    DirectCallTargetNameAtAddress(view, showRPNViewsFunc, 0x100008fd4),
	    std::optional<std::string>("-[NSWindow isVisible]"));
	EXPECT_TRUE(HasCodeReferenceToSymbolFromAddress(view, "-[NSWindow isVisible]", 0x100008fd4));
	EXPECT_TRUE(StructHasMemberAtOffset(view, "LCDController", 0xb0));
	EXPECT_TRUE(StructHasMemberAtOffset(view, "LCDController", 0xc0));
	auto fieldAccessText = HLILTextInRange(initFunc, 0x100007a54, 0x100007a60);
	EXPECT_EQ(fieldAccessText.find("+ 0xb0"), std::string::npos) << fieldAccessText;
	EXPECT_EQ(fieldAccessText.find("+ 0xc0"), std::string::npos) << fieldAccessText;
	EXPECT_NE(fieldAccessText.find("->"), std::string::npos) << fieldAccessText;

	auto awakeFromNibFunc = view->GetAnalysisFunction(view->GetDefaultPlatform(), 0x100007b6c);
	ASSERT_TRUE(awakeFromNibFunc);
	auto retainedMainBundleReturn = AdjustedCallReturnType(awakeFromNibFunc, 0x100007bdc);
	ASSERT_TRUE(retainedMainBundleReturn) << HLILDiagnostics(view, awakeFromNibFunc, 0x100007bd0, 0x100007be8);
	EXPECT_EQ(TypeString(view, retainedMainBundleReturn), "NSBundle*");
	EXPECT_TRUE(HasHLILVariableTypeInitializedInRange(
	    view, awakeFromNibFunc, 0x100007bdc, 0x100007bdc, "NSBundle*"))
	    << HLILDiagnostics(view, awakeFromNibFunc, 0x100007bd8, 0x100007be0);
	EXPECT_TRUE(HasObjCExternSymbol(view, "-[NSBundle localizedStringForKey:value:table:]"))
	    << ObjCSymbolDiagnostics(view);
	EXPECT_TRUE(HasDirectCallToSymbol(view, "-[NSBundle localizedStringForKey:value:table:]"))
	    << ObjCSymbolDiagnostics(view);
	EXPECT_TRUE(HasObjCExternSymbol(view, "-[NSTextField setTextColor:]"))
	    << ObjCSymbolDiagnostics(view);
	auto nsTextFieldLibrary = SymbolExternalLibraryName(view, "_OBJC_CLASS_$_NSTextField");
	auto nsWindowLibrary = SymbolExternalLibraryName(view, "_OBJC_CLASS_$_NSWindow");
	ASSERT_TRUE(nsTextFieldLibrary) << ObjCSymbolDiagnostics(view);
	ASSERT_TRUE(nsWindowLibrary) << ObjCSymbolDiagnostics(view);
	ASSERT_EQ(nsTextFieldLibrary, nsWindowLibrary);
	EXPECT_EQ(SymbolExternalLibraryMappingName(view, "-[NSTextField setTextColor:]"), nsTextFieldLibrary);
	EXPECT_EQ(SymbolExternalLibraryMappingName(view, "-[NSWindow isVisible]"), nsWindowLibrary);
	EXPECT_EQ(ObjCExternExternalLibraryName(view, "-[NSTextField setTextColor:]"), nsTextFieldLibrary);
	EXPECT_EQ(ObjCExternExternalLibraryName(view, "-[NSWindow isVisible]"), nsWindowLibrary);
	auto nsTextFieldExtern = ObjCExternSymbolAddress(view, "-[NSTextField setTextColor:]");
	auto nsWindowExtern = ObjCExternSymbolAddress(view, "-[NSWindow isVisible]");
	ASSERT_TRUE(nsTextFieldExtern) << ObjCSymbolDiagnostics(view);
	ASSERT_TRUE(nsWindowExtern) << ObjCSymbolDiagnostics(view);
	EXPECT_LT(*nsTextFieldExtern, *nsWindowExtern);

	view->GetFile()->Close();
}

int main(int argc, char** argv)
{
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

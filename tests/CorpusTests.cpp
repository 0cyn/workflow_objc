//
// LLM Generated Test Boilerplate stuff
// General "here be dragons" warning
//

#include "Workflow.h"

#include "Metadata.h"
#include "activities/ObjCExterns.h"

#include <binaryninjaapi.h>
#include <gtest/gtest.h>
#include <highlevelilinstruction.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

using namespace BinaryNinja;

namespace
{
	bool gOverwriteSnapshots = false;
	std::optional<std::string> gCorpusBinaryFilter;
	thread_local const std::unordered_set<std::string>* gLocalObjCClassNames = nullptr;

	class BinaryNinjaCorpusTest : public testing::Test
	{
	  protected:
		static void SetUpTestSuite()
		{
			static bool initialized = [] {
				auto hardwareThreads = std::thread::hardware_concurrency();
				if (hardwareThreads != 0)
					BinaryNinja::SetWorkerThreadCount(
					    std::max<size_t>(BinaryNinja::GetWorkerThreadCount(), hardwareThreads));

				BinaryNinja::SetBundledPluginDirectory(BinaryNinja::GetBundledPluginDirectory());
				return BinaryNinja::InitPlugins(false);
			}();

			if (!initialized)
				GTEST_SKIP() << "Binary Ninja core plugins could not be initialized";
		}
	};

	struct LoadRequest
	{
		std::string options;
		bool required;
	};

	struct LoadedView
	{
		Ref<BinaryView> view;

		~LoadedView()
		{
			if (view)
				view->GetFile()->Close();
		}
	};

	std::filesystem::path TestRoot()
	{
		return std::filesystem::path(__FILE__).parent_path();
	}

	std::string Hex(uint64_t value)
	{
		std::ostringstream stream;
		stream << "0x" << std::hex << value;
		return stream.str();
	}

	std::string Escape(std::string_view value)
	{
		std::string result;
		result.reserve(value.size());
		for (unsigned char ch : value)
		{
			switch (ch)
			{
			case '\\':
				result += "\\\\";
				break;
			case '\n':
				result += "\\n";
				break;
			case '\r':
				result += "\\r";
				break;
			case '\t':
				result += "\\t";
				break;
			default:
				if (ch < 0x20 || ch == 0x7f)
				{
					std::ostringstream escaped;
					escaped << "\\x" << std::hex << std::setw(2) << std::setfill('0')
					        << static_cast<int>(ch);
					result += escaped.str();
				}
				else
				{
					result += static_cast<char>(ch);
				}
			}
		}

		return result;
	}

	std::string AnalysisStateName(BNAnalysisState state)
	{
		switch (state)
		{
		case InitialState:
			return "InitialState";
		case HoldState:
			return "HoldState";
		case IdleState:
			return "IdleState";
		case DiscoveryState:
			return "DiscoveryState";
		case DisassembleState:
			return "DisassembleState";
		case AnalyzeState:
			return "AnalyzeState";
		case ExtendedAnalyzeState:
			return "ExtendedAnalyzeState";
		}

		return "UnknownState(" + std::to_string(static_cast<int>(state)) + ")";
	}

	std::string ArchName(Architecture* arch)
	{
		return arch ? arch->GetName() : "<none>";
	}

	std::string PlatformName(Platform* platform)
	{
		return platform ? platform->GetName() : "<none>";
	}

	std::string TypeString(BinaryView* view, Type* type)
	{
		return type ? type->GetString(view->GetDefaultPlatform()) : "<null>";
	}

	std::string ConfidenceTypeString(BinaryView* view, const Confidence<Ref<Type>>& type)
	{
		if (type.IsUnknown() || !type.GetValue())
			return "<unknown>@0";

		return Escape(TypeString(view, type.GetValue())) + "@" + std::to_string(type.GetConfidence());
	}

	std::string NormalizeTypeLibraryName(std::string name)
	{
		for (std::string_view platform : {".mac-x86_64", ".mac-aarch64", ".ios-aarch64"})
		{
			auto pos = name.find(platform);
			if (pos != std::string::npos)
				return name.substr(0, pos + platform.size());
		}

		return name;
	}

	bool IsImportedSymbol(Symbol* symbol)
	{
		if (!symbol)
			return false;

		switch (symbol->GetType())
		{
		case ImportAddressSymbol:
		case ImportedFunctionSymbol:
		case ImportedDataSymbol:
		case ExternalSymbol:
		case LibraryFunctionSymbol:
			return true;
		default:
			return false;
		}
	}

	bool ShouldDumpDataVariable(BinaryView* view, uint64_t address)
	{
		auto symbol = view->GetSymbolByAddress(address);
		if (view->IsOffsetExternSemantics(address))
			return false;

		return !IsImportedSymbol(symbol);
	}

	std::string AddressLabel(BinaryView* view, uint64_t address)
	{
		if (!view->IsOffsetExternSemantics(address))
			return Hex(address);

		if (auto symbol = view->GetSymbolByAddress(address))
			return "extern:" + Escape(symbol->GetRawName());

		return "extern";
	}

	std::string FunctionName(Function* func)
	{
		auto symbol = func->GetSymbol();
		return symbol ? symbol->GetRawName() : "<unnamed>";
	}

	Ref<DisassemblySettings> HLILTextSettings()
	{
		auto defaults = DisassemblySettings::GetDefaultSettings();
		Ref<DisassemblySettings> settings = defaults ? defaults->Duplicate() : new DisassemblySettings();
		settings->SetOption(DisableLineFormatting, true);
		settings->SetOption(WaitForIL, true);
		settings->SetWidth(1000000);
		return settings;
	}

	bool IsDecimal(std::string_view text)
	{
		if (text.empty())
			return false;
		return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch); });
	}

	bool HasRegisterPrefix(std::string_view text, std::string_view prefix)
	{
		return text.starts_with(prefix) && IsDecimal(text.substr(prefix.size()));
	}

	bool IsRegisterTemporary(std::string_view text)
	{
		if (text.size() >= 2 && (text[0] == 'x' || text[0] == 'w' || text[0] == 'r' || text[0] == 'v') &&
		    IsDecimal(text.substr(1)))
			return true;
		if (HasRegisterPrefix(text, "xmm") || HasRegisterPrefix(text, "ymm") || HasRegisterPrefix(text, "zmm"))
			return true;

		return text == "rax" || text == "rbx" || text == "rcx" || text == "rdx" || text == "rsi" ||
		       text == "rdi" || text == "rbp" || text == "rsp" || text == "rip" || text == "eax" ||
		       text == "ebx" || text == "ecx" || text == "edx" || text == "esi" || text == "edi" ||
		       text == "ebp" || text == "esp" || text == "ax" || text == "bx" || text == "cx" ||
		       text == "dx" || text == "si" || text == "di" || text == "bp" || text == "sp" ||
		       text == "lr";
	}

	bool IsGenericTemporary(std::string_view text)
	{
		return text == "obj" || text == "string" || text == "theString" || text == "oslog" ||
		       text == "buf" || text == "entry_format" || text == "countByEnumeratingWithState" ||
		       text == "objects" || text == "arg" || text == "log" || text == "format" ||
		       text == "domain" || text == "errorWithDomain" || text == "UUIDBytes" || text == "bytes" ||
		       text == "dataWithBytes" || text == "dataWithBytesNoCopy" || text == "exception_object" ||
		       text == "into" || text == "addCodec" || text == "codecStruct" || text == "repeats" ||
		       text == "yesOrNo" || text == "rep" || text == "i" ||
		       IsRegisterTemporary(text);
	}

	std::string NormalizeVariableToken(std::string_view text)
	{
		if (text.starts_with("var_") || text.starts_with("entry_"))
			return "<tmp>";
		if (text.starts_with("arg") && IsDecimal(text.substr(3)))
			return "<tmp>";

		auto suffix = text.rfind('_');
		std::string normalized(text);
		if (suffix != std::string_view::npos && suffix != 0 && suffix + 1 < text.size() &&
		    IsDecimal(text.substr(suffix + 1)))
		{
			// Drop SSA-like auto suffixes like obj_3 or index_12.
			normalized = text.substr(0, suffix);
		}

		if (IsGenericTemporary(normalized))
			return "<tmp>";
		return normalized;
	}

	bool ShouldNormalizeImportedObjCTypeName(std::string_view text)
	{
		if (text.empty() || !std::isupper(static_cast<unsigned char>(text.front())))
			return false;
		if (text == "Class" || text == "SEL")
			return false;
		if (gLocalObjCClassNames && gLocalObjCClassNames->contains(std::string(text)))
			return false;
		return true;
	}

	std::string LineText(const DisassemblyTextLine& line)
	{
		std::string result;
		for (size_t i = 0; i < line.tokens.size(); ++i)
		{
			const auto& token = line.tokens[i];
			switch (token.type)
			{
			case LocalVariableToken:
			case RegisterToken:
				result += NormalizeVariableToken(token.text);
				break;
			case StackVariableToken:
				result += "<stack>";
				break;
			case TypeNameToken:
				if (i + 1 < line.tokens.size() && line.tokens[i + 1].text == "*" &&
				    ShouldNormalizeImportedObjCTypeName(token.text))
				{
					result += "id";
					++i;
				}
				else
				{
					result += token.text;
				}
				break;
			case EnumerationMemberToken:
				result += "<enum>";
				break;
			default:
				result += token.text;
				break;
			}
		}

		return result;
	}

	std::string CollapseWhitespaceOutsideStrings(std::string_view text)
	{
		std::string result;
		result.reserve(text.size());
		bool inString = false;
		bool escaped = false;
		bool pendingSpace = false;

		for (char ch : text)
		{
			if (!inString && std::isspace(static_cast<unsigned char>(ch)))
			{
				pendingSpace = !result.empty();
				continue;
			}

			if (pendingSpace && !result.empty() && result.back() != ' ')
				result += ' ';
			pendingSpace = false;

			result += ch;
			if (inString)
			{
				if (escaped)
					escaped = false;
				else if (ch == '\\')
					escaped = true;
				else if (ch == '"')
					inString = false;
			}
			else if (ch == '"')
			{
				inString = true;
			}
		}

		if (!result.empty() && result.back() == ' ')
			result.pop_back();
		return result;
	}

	std::string NormalizeTypeMarkers(std::string text)
	{
		constexpr std::string_view marker = "<type>";
		size_t pos = 0;
		while ((pos = text.find(marker, pos)) != std::string::npos)
		{
			pos += marker.size();
			while (pos < text.size() && text[pos] == '*')
				text.erase(pos, 1);
		}

		return text;
	}

	void ReplaceAll(std::string& text, std::string_view needle, std::string_view replacement)
	{
		size_t pos = 0;
		while ((pos = text.find(needle, pos)) != std::string::npos)
		{
			text.replace(pos, needle.size(), replacement);
			pos += replacement.size();
		}
	}

	std::string NormalizeOSLogEnums(std::string text)
	{
		if (text.find("os_log") == std::string::npos)
			return text;

		for (std::string_view value : {"0", "1", "2", "16", "17", "0x10", "0x11"})
		{
			std::string quotedNeedle = ", " + std::string(value) + ", \"";
			ReplaceAll(text, quotedNeedle, ", <enum>, \"");
			std::string closeNeedle = ", " + std::string(value) + ")";
			ReplaceAll(text, closeNeedle, ", <enum>)");
		}

		return text;
	}

	std::string NormalizeOSLogImpl(std::string text)
	{
		auto osLog = text.find("__os_log");
		if (osLog == std::string::npos || text.find("_impl", osLog) == std::string::npos)
			return text;

		auto openParen = text.find('(', osLog);
		if (openParen == std::string::npos)
			return text;

		bool inString = false;
		bool escaped = false;
		int depth = 0;
		size_t closeParen = std::string::npos;
		for (size_t i = openParen; i < text.size(); ++i)
		{
			char ch = text[i];
			if (inString)
			{
				if (escaped)
					escaped = false;
				else if (ch == '\\')
					escaped = true;
				else if (ch == '"')
					inString = false;
				continue;
			}

			if (ch == '"')
			{
				inString = true;
				continue;
			}
			if (ch == '(')
				++depth;
			else if (ch == ')' && --depth == 0)
			{
				closeParen = i;
				break;
			}
		}
		if (closeParen == std::string::npos)
			return text;

		std::string replacement = text.substr(osLog, openParen - osLog) + "(<log>)";

		return text.substr(0, osLog) + replacement + text.substr(closeParen + 1);
	}

	std::string NormalizeExactCallArguments(std::string text, std::string_view name, std::string_view placeholder)
	{
		size_t searchStart = 0;
		while (searchStart < text.size())
		{
			auto start = text.find(name, searchStart);
			if (start == std::string::npos)
				break;
			auto openParen = start + name.size();
			if (openParen >= text.size() || text[openParen] != '(')
			{
				searchStart = openParen;
				continue;
			}

			bool inString = false;
			bool escaped = false;
			int depth = 0;
			size_t closeParen = std::string::npos;
			for (size_t i = openParen; i < text.size(); ++i)
			{
				char ch = text[i];
				if (inString)
				{
					if (escaped)
						escaped = false;
					else if (ch == '\\')
						escaped = true;
					else if (ch == '"')
						inString = false;
					continue;
				}

				if (ch == '"')
				{
					inString = true;
					continue;
				}
				if (ch == '(')
					++depth;
				else if (ch == ')' && --depth == 0)
				{
					closeParen = i;
					break;
				}
			}

			if (closeParen == std::string::npos)
			{
				searchStart = openParen + 1;
				continue;
			}

			std::string replacement(name);
			replacement += "(";
			replacement += placeholder;
			replacement += ")";
			text.replace(start, closeParen - start + 1, replacement);
			searchStart = start + replacement.size();
		}

		return text;
	}

	std::string NormalizeOSSignpostCalls(std::string text)
	{
		text = NormalizeExactCallArguments(std::move(text), "__os_signpost_emit_with_name_impl", "<log>");
		text = NormalizeExactCallArguments(std::move(text), "_os_signpost_enabled", "<log>");
		return NormalizeExactCallArguments(std::move(text), "__Unwind_Resume", "");
	}

	std::string NormalizeTempReceiverObjCDisplayTargets(std::string text)
	{
		size_t searchStart = 0;
		while (searchStart < text.size())
		{
			auto start = text.find("-[", searchStart);
			if (start == std::string::npos)
				break;

			auto targetEnd = text.find("](<tmp>, ", start + 2);
			if (targetEnd == std::string::npos)
			{
				searchStart = start + 2;
				continue;
			}

			text.replace(start, targetEnd - start + 1, "_objc_msgSend");
			searchStart = start + std::string_view("_objc_msgSend").size();
		}

		return text;
	}

	std::string NormalizeSubroutineCalls(std::string text)
	{
		size_t searchStart = 0;
		while (searchStart < text.size())
		{
			auto start = text.find("sub_", searchStart);
			if (start == std::string::npos)
				break;

			size_t nameEnd = start + 4;
			while (nameEnd < text.size() && std::isxdigit(static_cast<unsigned char>(text[nameEnd])))
				++nameEnd;
			if (nameEnd == start + 4 || nameEnd >= text.size() || text[nameEnd] != '(')
			{
				searchStart = nameEnd;
				continue;
			}

			bool inString = false;
			bool escaped = false;
			int depth = 0;
			size_t closeParen = std::string::npos;
			for (size_t i = nameEnd; i < text.size(); ++i)
			{
				char ch = text[i];
				if (inString)
				{
					if (escaped)
						escaped = false;
					else if (ch == '\\')
						escaped = true;
					else if (ch == '"')
						inString = false;
					continue;
				}

				if (ch == '"')
				{
					inString = true;
					continue;
				}
				if (ch == '(')
					++depth;
				else if (ch == ')' && --depth == 0)
				{
					closeParen = i;
					break;
				}
			}

			if (closeParen == std::string::npos)
			{
				searchStart = nameEnd + 1;
				continue;
			}

			text.erase(nameEnd + 1, closeParen - nameEnd - 1);
			searchStart = nameEnd + 2;
		}

		return text;
	}

	std::string LinesText(const std::vector<DisassemblyTextLine>& lines)
	{
		std::string result;
		for (const auto& line : lines)
		{
			if (!result.empty())
				result += ' ';
			result += LineText(line);
		}

		auto normalized = NormalizeOSSignpostCalls(NormalizeOSLogImpl(NormalizeOSLogEnums(
		    NormalizeTempReceiverObjCDisplayTargets(NormalizeTypeMarkers(CollapseWhitespaceOutsideStrings(result))))));
		ReplaceAll(normalized, "<tmp>.q f", "<tmp>");
		ReplaceAll(normalized, "<tmp>==", "<tmp> ==");
		ReplaceAll(normalized, "<tmp><", "<tmp> <");
		ReplaceAll(normalized, "<tmp>>", "<tmp> >");
		return NormalizeSubroutineCalls(std::move(normalized));
	}

	bool IsTemporaryCopySummary(std::string_view text)
	{
		auto assignment = text.find(" = ");
		if (assignment == std::string_view::npos)
			return false;
		auto lhs = text.substr(0, assignment);
		auto rhs = text.substr(assignment + 3);
		return (lhs.find("<tmp>") != std::string_view::npos || lhs.find("<stack>") != std::string_view::npos) &&
		       rhs == "<tmp>";
	}

	bool IsTemporarySetupSummary(std::string_view text)
	{
		auto assignment = text.find(" = ");
		if (assignment == std::string_view::npos)
			return false;
		auto lhs = text.substr(0, assignment);
		auto rhs = text.substr(assignment + 3);
		return lhs.find("<tmp>") != std::string_view::npos && rhs.find("_objc_") == std::string_view::npos &&
		       rhs.find("__os_") == std::string_view::npos && rhs.find("__builtin_") == std::string_view::npos;
	}

	bool IsTemporaryDeclarationSummary(std::string_view text)
	{
		return text.find(" = ") == std::string_view::npos && text.find("<tmp>") != std::string_view::npos;
	}

	std::optional<std::string> AssignmentSummary(std::string_view text)
	{
		auto assignment = text.find(" = ");
		if (assignment == std::string_view::npos)
			return std::nullopt;
		if (IsTemporaryCopySummary(text) || IsTemporarySetupSummary(text))
			return std::nullopt;
		auto lhs = text.substr(0, assignment);
		auto rhs = text.substr(assignment + 3);
		if (lhs.find("<tmp>") != std::string_view::npos && rhs.find('(') != std::string_view::npos)
			return "<tmp> = " + std::string(rhs);

		return std::string(text);
	}

	std::optional<std::string> HLILSummaryLine(const HighLevelILInstruction& instr, std::string text)
	{
		switch (instr.operation)
		{
		case HLIL_BLOCK:
		case HLIL_NOP:
			return std::nullopt;
		case HLIL_VAR_DECLARE:
			if (IsTemporaryDeclarationSummary(text))
				return std::nullopt;
			return text;
		case HLIL_ASSIGN:
		case HLIL_ASSIGN_UNPACK:
		{
			auto summary = AssignmentSummary(text);
			if (summary && summary->find("_os_log_type_enabled") != std::string::npos)
				return std::nullopt;
			return summary;
		}
		case HLIL_VAR_INIT:
			if (auto rhs = AssignmentSummary(text))
			{
				if (rhs->find("_os_log_type_enabled") != std::string::npos)
					return std::nullopt;
				return rhs;
			}
			return std::nullopt;
		case HLIL_CALL:
		case HLIL_TAILCALL:
		case HLIL_RET:
		case HLIL_IF:
		case HLIL_WHILE:
		case HLIL_DO_WHILE:
		case HLIL_FOR:
		case HLIL_SWITCH:
		case HLIL_CASE:
		case HLIL_GOTO:
		case HLIL_LABEL:
		case HLIL_BREAK:
		case HLIL_CONTINUE:
			return text;
		default:
			if (text.find("_objc_msgSend") != std::string::npos)
				return text;
			return std::nullopt;
		}
	}

	bool IsOSLogImplSummary(std::string_view summary)
	{
		return summary.find("__os_log") != std::string_view::npos &&
		       summary.find("_impl(") != std::string_view::npos;
	}

	bool IsLogHelperSummary(std::string_view summary)
	{
		return summary.starts_with("sub_");
	}

	bool IsLoggingSummary(std::string_view summary)
	{
		return IsOSLogImplSummary(summary) || IsLogHelperSummary(summary);
	}

	bool IsLogSetupSummary(const HighLevelILInstruction& instr, std::string_view summary)
	{
		switch (instr.operation)
		{
		case HLIL_VAR_DECLARE:
			return true;
		case HLIL_ASSIGN:
		case HLIL_ASSIGN_UNPACK:
		case HLIL_VAR_INIT:
			return summary.find('(') == std::string_view::npos;
		default:
			return false;
		}
	}

	std::string RenderHLILText(HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings)
	{
		if (instr.instructionIndex < hlil->GetInstructionCount())
			return LinesText(hlil->GetInstructionText(instr.instructionIndex, settings));

		return LinesText(hlil->GetExprText(instr, settings));
	}

	std::string RenderExprText(HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings)
	{
		return LinesText(hlil->GetExprText(instr, settings));
	}

	std::optional<std::string> ControlFlowSummary(HighLevelILFunction* hlil,
	    const HighLevelILInstruction& instr, DisassemblySettings* settings)
	{
		switch (instr.operation)
		{
		case HLIL_IF:
			return "if (" + RenderExprText(hlil, instr.GetConditionExpr<HLIL_IF>(), settings) + ")";
		case HLIL_WHILE:
			return "while (" + RenderExprText(hlil, instr.GetConditionExpr<HLIL_WHILE>(), settings) + ")";
		case HLIL_DO_WHILE:
			return "do";
		case HLIL_FOR:
			return "for (" + RenderExprText(hlil, instr.GetInitExpr<HLIL_FOR>(), settings) + "; " +
			       RenderExprText(hlil, instr.GetConditionExpr<HLIL_FOR>(), settings) + "; " +
			       RenderExprText(hlil, instr.GetUpdateExpr<HLIL_FOR>(), settings) + ")";
		case HLIL_SWITCH:
			return "switch (" + RenderExprText(hlil, instr.GetConditionExpr<HLIL_SWITCH>(), settings) + ")";
		case HLIL_CASE:
		{
			std::string values;
			for (const auto& value : instr.GetValueExprs<HLIL_CASE>())
			{
				if (!values.empty())
					values += ", ";
				values += RenderExprText(hlil, value, settings);
			}
			return "case " + values + ":";
		}
		default:
			return std::nullopt;
		}
	}

	std::optional<std::string> RenderHLILSummary(HighLevelILFunction* hlil,
	    const HighLevelILInstruction& instr, DisassemblySettings* settings)
	{
		if (auto control = ControlFlowSummary(hlil, instr, settings))
			return control;

		return HLILSummaryLine(instr, RenderHLILText(hlil, instr, settings));
	}

	bool ScopeHasOutput(HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings);
	bool ScopeContainsLoggingOutput(HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings);
	std::pair<bool, bool> ScopeOutputIsOnlyOSLogImpl(HighLevelILFunction* hlil,
	    const HighLevelILInstruction& instr, DisassemblySettings* settings);

	void DumpHLILScope(std::ostream& stream, HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings, size_t indent);
	void DumpLoggingBody(std::ostream& stream, HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings, size_t indent);

	std::string Indent(size_t indent)
	{
		return std::string(indent * 2, ' ');
	}

	void DumpLine(std::ostream& stream, size_t indent, std::string_view text)
	{
		stream << Indent(indent) << Escape(text) << "\n";
	}

	void DumpBlockBody(std::ostream& stream, HighLevelILFunction* hlil, const HighLevelILInstruction& body,
	    DisassemblySettings* settings, size_t indent)
	{
		if (body.operation == HLIL_BLOCK)
		{
			for (const auto& expr : body.GetBlockExprs<HLIL_BLOCK>())
				DumpHLILScope(stream, hlil, expr, settings, indent);
			return;
		}

		DumpHLILScope(stream, hlil, body, settings, indent);
	}

	bool ScopeHasOutput(HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings)
	{
		switch (instr.operation)
		{
		case HLIL_NOP:
			return false;
		case HLIL_BLOCK:
			for (const auto& expr : instr.GetBlockExprs<HLIL_BLOCK>())
			{
				if (ScopeHasOutput(hlil, expr, settings))
					return true;
			}
			return false;
		case HLIL_IF:
			return RenderHLILSummary(hlil, instr, settings).has_value() ||
			       ScopeHasOutput(hlil, instr.GetTrueExpr<HLIL_IF>(), settings) ||
			       ScopeHasOutput(hlil, instr.GetFalseExpr<HLIL_IF>(), settings);
		case HLIL_WHILE:
			return RenderHLILSummary(hlil, instr, settings).has_value() ||
			       ScopeHasOutput(hlil, instr.GetLoopExpr<HLIL_WHILE>(), settings);
		case HLIL_DO_WHILE:
			return RenderHLILSummary(hlil, instr, settings).has_value() ||
			       ScopeHasOutput(hlil, instr.GetLoopExpr<HLIL_DO_WHILE>(), settings);
		case HLIL_FOR:
			return RenderHLILSummary(hlil, instr, settings).has_value() ||
			       ScopeHasOutput(hlil, instr.GetLoopExpr<HLIL_FOR>(), settings);
		case HLIL_SWITCH:
			if (RenderHLILSummary(hlil, instr, settings))
				return true;
			for (const auto& caseExpr : instr.GetCases<HLIL_SWITCH>())
			{
				if (ScopeHasOutput(hlil, caseExpr, settings))
					return true;
			}
			return ScopeHasOutput(hlil, instr.GetDefaultExpr<HLIL_SWITCH>(), settings);
		case HLIL_CASE:
			return RenderHLILSummary(hlil, instr, settings).has_value() ||
			       ScopeHasOutput(hlil, instr.GetTrueExpr<HLIL_CASE>(), settings);
		default:
			return RenderHLILSummary(hlil, instr, settings).has_value();
		}
	}

	std::pair<bool, bool> ScopeOutputIsOnlyOSLogImpl(HighLevelILFunction* hlil,
	    const HighLevelILInstruction& instr, DisassemblySettings* settings)
	{
		switch (instr.operation)
		{
		case HLIL_NOP:
			return {false, true};
		case HLIL_BLOCK:
		{
			bool hasOutput = false;
			for (const auto& expr : instr.GetBlockExprs<HLIL_BLOCK>())
			{
				auto [childHasOutput, childOnlyOSLog] = ScopeOutputIsOnlyOSLogImpl(hlil, expr, settings);
				hasOutput = hasOutput || childHasOutput;
				if (!childOnlyOSLog)
					return {hasOutput, false};
			}
			return {hasOutput, true};
		}
		case HLIL_IF:
		{
			auto trueState = ScopeOutputIsOnlyOSLogImpl(hlil, instr.GetTrueExpr<HLIL_IF>(), settings);
			auto falseState = ScopeOutputIsOnlyOSLogImpl(hlil, instr.GetFalseExpr<HLIL_IF>(), settings);
			return {trueState.first || falseState.first, trueState.second && falseState.second};
		}
		default:
			if (auto summary = RenderHLILSummary(hlil, instr, settings))
			{
				if (IsLogSetupSummary(instr, *summary))
					return {false, true};
				return {true, IsLoggingSummary(*summary)};
			}
			return {false, true};
		}
	}

	bool ScopeContainsLoggingOutput(HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings)
	{
		switch (instr.operation)
		{
		case HLIL_NOP:
			return false;
		case HLIL_BLOCK:
			for (const auto& expr : instr.GetBlockExprs<HLIL_BLOCK>())
			{
				if (ScopeContainsLoggingOutput(hlil, expr, settings))
					return true;
			}
			return false;
		case HLIL_IF:
			return ScopeContainsLoggingOutput(hlil, instr.GetTrueExpr<HLIL_IF>(), settings) ||
			       ScopeContainsLoggingOutput(hlil, instr.GetFalseExpr<HLIL_IF>(), settings);
		case HLIL_WHILE:
			return ScopeContainsLoggingOutput(hlil, instr.GetLoopExpr<HLIL_WHILE>(), settings);
		case HLIL_DO_WHILE:
			return ScopeContainsLoggingOutput(hlil, instr.GetLoopExpr<HLIL_DO_WHILE>(), settings);
		case HLIL_FOR:
			return ScopeContainsLoggingOutput(hlil, instr.GetLoopExpr<HLIL_FOR>(), settings);
		case HLIL_SWITCH:
			for (const auto& caseExpr : instr.GetCases<HLIL_SWITCH>())
			{
				if (ScopeContainsLoggingOutput(hlil, caseExpr, settings))
					return true;
			}
			return ScopeContainsLoggingOutput(hlil, instr.GetDefaultExpr<HLIL_SWITCH>(), settings);
		case HLIL_CASE:
			return ScopeContainsLoggingOutput(hlil, instr.GetTrueExpr<HLIL_CASE>(), settings);
		default:
			if (auto summary = RenderHLILSummary(hlil, instr, settings))
				return IsLoggingSummary(*summary);
			return false;
		}
	}

	void DumpLoggingBody(std::ostream& stream, HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings, size_t indent)
	{
		switch (instr.operation)
		{
		case HLIL_NOP:
			return;
		case HLIL_BLOCK:
			for (const auto& expr : instr.GetBlockExprs<HLIL_BLOCK>())
				DumpLoggingBody(stream, hlil, expr, settings, indent);
			return;
		case HLIL_IF:
			DumpLoggingBody(stream, hlil, instr.GetTrueExpr<HLIL_IF>(), settings, indent);
			DumpLoggingBody(stream, hlil, instr.GetFalseExpr<HLIL_IF>(), settings, indent);
			return;
		default:
			if (auto summary = RenderHLILSummary(hlil, instr, settings))
			{
				if (IsOSLogImplSummary(*summary))
					DumpLine(stream, indent, *summary);
				else if (IsLogHelperSummary(*summary))
					DumpLine(stream, indent, "<log helper>()");
			}
			return;
		}
	}

	void DumpScopedConstruct(std::ostream& stream, HighLevelILFunction* hlil,
	    const HighLevelILInstruction& instr, DisassemblySettings* settings, size_t indent,
	    const HighLevelILInstruction& body)
	{
		if (auto summary = RenderHLILSummary(hlil, instr, settings))
			DumpLine(stream, indent, *summary);
		DumpLine(stream, indent, "{");
		DumpBlockBody(stream, hlil, body, settings, indent + 1);
		DumpLine(stream, indent, "}");
	}

	void DumpHLILScope(std::ostream& stream, HighLevelILFunction* hlil, const HighLevelILInstruction& instr,
	    DisassemblySettings* settings, size_t indent)
	{
		switch (instr.operation)
		{
		case HLIL_BLOCK:
			for (const auto& expr : instr.GetBlockExprs<HLIL_BLOCK>())
				DumpHLILScope(stream, hlil, expr, settings, indent);
			break;
		case HLIL_IF:
		{
			auto trueExpr = instr.GetTrueExpr<HLIL_IF>();
			auto [trueHasOutput, trueOnlyOSLog] = ScopeOutputIsOnlyOSLogImpl(hlil, trueExpr, settings);
			auto summary = RenderHLILSummary(hlil, instr, settings);
			if (summary && summary->find("_os_log_type_enabled") != std::string::npos &&
			    ScopeContainsLoggingOutput(hlil, trueExpr, settings))
			{
				DumpLoggingBody(stream, hlil, trueExpr, settings, indent);
				break;
			}
			if (trueHasOutput && trueOnlyOSLog)
			{
				DumpLoggingBody(stream, hlil, trueExpr, settings, indent);
				break;
			}

			DumpScopedConstruct(stream, hlil, instr, settings, indent, trueExpr);
			auto falseExpr = instr.GetFalseExpr<HLIL_IF>();
			if (ScopeHasOutput(hlil, falseExpr, settings))
			{
				DumpLine(stream, indent, "else");
				DumpLine(stream, indent, "{");
				DumpBlockBody(stream, hlil, falseExpr, settings, indent + 1);
				DumpLine(stream, indent, "}");
			}
			break;
		}
		case HLIL_WHILE:
			DumpScopedConstruct(stream, hlil, instr, settings, indent, instr.GetLoopExpr<HLIL_WHILE>());
			break;
		case HLIL_DO_WHILE:
			DumpScopedConstruct(stream, hlil, instr, settings, indent, instr.GetLoopExpr<HLIL_DO_WHILE>());
			break;
		case HLIL_FOR:
			DumpScopedConstruct(stream, hlil, instr, settings, indent, instr.GetLoopExpr<HLIL_FOR>());
			break;
		case HLIL_SWITCH:
			if (auto summary = RenderHLILSummary(hlil, instr, settings))
				DumpLine(stream, indent, *summary);
			DumpLine(stream, indent, "{");
			for (const auto& caseExpr : instr.GetCases<HLIL_SWITCH>())
				DumpHLILScope(stream, hlil, caseExpr, settings, indent + 1);
			if (ScopeHasOutput(hlil, instr.GetDefaultExpr<HLIL_SWITCH>(), settings))
			{
				DumpLine(stream, indent + 1, "default:");
				DumpBlockBody(stream, hlil, instr.GetDefaultExpr<HLIL_SWITCH>(), settings, indent + 2);
			}
			DumpLine(stream, indent, "}");
			break;
		case HLIL_CASE:
			if (auto summary = RenderHLILSummary(hlil, instr, settings))
				DumpLine(stream, indent, *summary);
			DumpBlockBody(stream, hlil, instr.GetTrueExpr<HLIL_CASE>(), settings, indent + 1);
			break;
		default:
			if (auto summary = RenderHLILSummary(hlil, instr, settings))
				DumpLine(stream, indent, *summary);
			break;
		}
	}

	std::vector<std::filesystem::path> CorpusBinaries()
	{
		std::vector<std::filesystem::path> binaries;
		for (const auto& entry : std::filesystem::directory_iterator(TestRoot() / "bin"))
		{
			if (!entry.is_regular_file())
				continue;
			if (gCorpusBinaryFilter && entry.path().filename() != *gCorpusBinaryFilter)
				continue;

			binaries.push_back(entry.path());
		}

		std::sort(binaries.begin(), binaries.end());
		return binaries;
	}

	std::string SnapshotNameFor(BinaryView* view, const std::filesystem::path& binary)
	{
		std::string name = binary.filename().string() + "." + ArchName(view->GetDefaultArchitecture());
		for (char& ch : name)
		{
			if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '_' && ch != '-')
				ch = '_';
		}

		return name + ".txt";
	}

	void DumpHeader(std::ostream& stream, BinaryView* view, const std::filesystem::path& binary)
	{
		auto analysis = view->GetAnalysisInfo();
		stream << "# workflow_objc corpus snapshot\n";
		stream << "binary " << Escape(binary.lexically_relative(TestRoot()).string()) << "\n";
		stream << "view type=" << Escape(view->GetTypeName()) << " arch="
		       << Escape(ArchName(view->GetDefaultArchitecture())) << " platform="
		       << Escape(PlatformName(view->GetDefaultPlatform())) << " address_size=" << view->GetAddressSize()
		       << " start=" << Hex(view->GetStart()) << " entry=" << Hex(view->GetEntryPoint()) << "\n";
		stream << "analysis state=" << AnalysisStateName(analysis.state) << " active=" << analysis.activeInfo.size()
		       << "\n\n";
	}

	void DumpSections(std::ostream& stream, BinaryView* view)
	{
		auto sections = view->GetSections();
		std::sort(sections.begin(), sections.end(), [](const auto& lhs, const auto& rhs) {
			return std::tuple(lhs->GetStart(), lhs->GetName()) < std::tuple(rhs->GetStart(), rhs->GetName());
		});

		stream << "sections\n";
		for (const auto& section : sections)
		{
			stream << "section " << Escape(section->GetName());
			if (section->GetSemantics() == ExternalSectionSemantics)
				stream << " external=1";
			else
				stream << " start=" << Hex(section->GetStart()) << " end=" << Hex(section->GetEnd());
			stream << " semantics=" << static_cast<int>(section->GetSemantics()) << "\n";
		}
		stream << "\n";
	}

	void DumpTypeLibraries(std::ostream& stream, BinaryView* view)
	{
		std::vector<std::string> libraries;
		for (const auto& library : view->GetTypeLibraries())
			libraries.push_back(NormalizeTypeLibraryName(library->GetName()));

		std::sort(libraries.begin(), libraries.end());
		libraries.erase(std::unique(libraries.begin(), libraries.end()), libraries.end());

		stream << "type_libraries\n";
		for (const auto& library : libraries)
			stream << "type_library " << Escape(library) << "\n";
		stream << "\n";
	}

	void DumpTypes(std::ostream& stream, BinaryView* view)
	{
		std::vector<std::pair<QualifiedName, Ref<Type>>> types;
		auto info = WorkflowObjC::GlobalState::GetAnalysisInfo(view);
		for (const auto& [name, type] : view->GetTypes())
		{
			if (!view->LookupImportedTypePlatform(name) &&
			    (!view->IsTypeAutoDefined(name) || (info && info->HasClass(view, name.GetString()))))
				types.emplace_back(name, type);
		}

		stream << "types\n";
		for (const auto& [name, type] : types)
		{
			stream << "type " << Escape(name.GetString()) << " auto=" << view->IsTypeAutoDefined(name)
			       << " value=" << Escape(TypeString(view, type)) << "\n";
		}
		stream << "\n";
	}

	std::unordered_set<std::string> LocalObjCClassNames(BinaryView* view)
	{
		std::unordered_set<std::string> names;
		auto info = WorkflowObjC::GlobalState::GetAnalysisInfo(view);
		if (!info)
			return names;

		for (const auto& [name, type] : view->GetTypes())
		{
			(void)type;
			std::string typeName = name.GetString();
			if (info->HasClass(view, typeName))
				names.insert(std::move(typeName));
		}

		return names;
	}

	void DumpSymbols(std::ostream& stream, BinaryView* view)
	{
		auto symbols = view->GetSymbols();
		symbols.erase(std::remove_if(symbols.begin(), symbols.end(), [&](const auto& symbol) {
			return IsImportedSymbol(symbol) || view->IsOffsetExternSemantics(symbol->GetAddress());
		}), symbols.end());
		std::sort(symbols.begin(), symbols.end(), [&](const auto& lhs, const auto& rhs) {
			return std::tuple(AddressLabel(view, lhs->GetAddress()), lhs->GetRawName(), lhs->GetType()) <
			       std::tuple(AddressLabel(view, rhs->GetAddress()), rhs->GetRawName(), rhs->GetType());
		});

		stream << "symbols\n";
		for (const auto& symbol : symbols)
		{
			stream << "symbol addr=" << AddressLabel(view, symbol->GetAddress()) << " type="
			       << static_cast<int>(symbol->GetType()) << " raw=" << Escape(symbol->GetRawName()) << "\n";
		}
		stream << "\n";
	}

	void DumpDataVariables(std::ostream& stream, BinaryView* view)
	{
		std::vector<std::pair<uint64_t, DataVariable>> dataVariables;
		for (const auto& [address, variable] : view->GetDataVariables())
		{
			if (ShouldDumpDataVariable(view, address))
				dataVariables.emplace_back(address, variable);
		}

		std::sort(dataVariables.begin(), dataVariables.end(), [&](const auto& lhs, const auto& rhs) {
			return std::tuple(AddressLabel(view, lhs.first), lhs.first) < std::tuple(AddressLabel(view, rhs.first), rhs.first);
		});

		stream << "data_variables\n";
		for (const auto& [address, variable] : dataVariables)
		{
			auto symbol = view->GetSymbolByAddress(address);
			auto type = view->IsOffsetExternSemantics(address) ? std::string("<objc-extern>") :
			    ConfidenceTypeString(view, variable.type);
			stream << "data_variable addr=" << AddressLabel(view, address) << " name="
			       << Escape(symbol ? symbol->GetRawName() : "") << " auto=" << variable.autoDiscovered
			       << " type=" << type << "\n";
		}
		stream << "\n";
	}

	void DumpFunctions(std::ostream& stream, BinaryView* view)
	{
		auto settings = HLILTextSettings();
		auto localObjCClassNames = LocalObjCClassNames(view);
		auto previousLocalObjCClassNames = gLocalObjCClassNames;
		gLocalObjCClassNames = &localObjCClassNames;
		auto functions = view->GetAnalysisFunctionList();
		std::sort(functions.begin(), functions.end(), [](const auto& lhs, const auto& rhs) {
			return std::tuple(lhs->GetStart(), ArchName(lhs->GetArchitecture()), FunctionName(lhs)) <
			       std::tuple(rhs->GetStart(), ArchName(rhs->GetArchitecture()), FunctionName(rhs));
		});

		stream << "functions\n";
		for (const auto& func : functions)
		{
			stream << "function " << Hex(func->GetStart()) << " " << Escape(FunctionName(func)) << "\n{\n";

			auto hlil = func->GetHighLevelIL();
			if (!hlil)
			{
				stream << "  <missing hlil>\n}\n";
				continue;
			}

			DumpHLILScope(stream, hlil, hlil->GetRootExpr(), settings, 1);
			stream << "}\n";
		}
		gLocalObjCClassNames = previousLocalObjCClassNames;
		stream << "\n";
	}

	std::string DumpView(BinaryView* view, const std::filesystem::path& binary)
	{
		std::ostringstream stream;
		DumpHeader(stream, view, binary);
		DumpSections(stream, view);
		DumpTypeLibraries(stream, view);
		DumpTypes(stream, view);
		DumpSymbols(stream, view);
		DumpDataVariables(stream, view);
		DumpFunctions(stream, view);
		return stream.str();
	}

	std::vector<std::string_view> SplitLines(std::string_view text)
	{
		std::vector<std::string_view> lines;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t end = text.find('\n', start);
			if (end == std::string_view::npos)
				end = text.size();
			lines.push_back(text.substr(start, end - start));
			if (end == text.size())
				break;
			start = end + 1;
		}

		return lines;
	}

	std::string NearestFunctionContext(const std::vector<std::string_view>& lines, size_t index)
	{
		if (lines.empty())
			return {};

		for (size_t i = std::min(index, lines.size() - 1) + 1; i-- > 0;)
		{
			if (lines[i].starts_with("function "))
				return std::string(lines[i]);
			if (i == 0)
				break;
		}

		return {};
	}

	struct DiffRow
	{
		char op;
		std::string_view text;
		size_t expectedLine;
		size_t actualLine;
	};

	constexpr size_t kNoLine = static_cast<size_t>(-1);

	std::vector<DiffRow> BuildLineDiff(const std::vector<std::string_view>& expected,
	    const std::vector<std::string_view>& actual)
	{
		constexpr size_t kLookahead = 64;
		std::vector<DiffRow> rows;
		size_t expectedIndex = 0;
		size_t actualIndex = 0;

		auto findSync = [&]() -> std::pair<size_t, size_t> {
			for (size_t distance = 1; distance <= kLookahead; ++distance)
			{
				for (size_t expectedAdvance = 0; expectedAdvance <= distance; ++expectedAdvance)
				{
					size_t actualAdvance = distance - expectedAdvance;
					if (expectedIndex + expectedAdvance >= expected.size() ||
					    actualIndex + actualAdvance >= actual.size())
						continue;

					auto syncLine = expected[expectedIndex + expectedAdvance];
					if (syncLine.empty())
						continue;
					if (syncLine == actual[actualIndex + actualAdvance])
						return {expectedAdvance, actualAdvance};
				}
			}

			return {1, 1};
		};

		while (expectedIndex < expected.size() || actualIndex < actual.size())
		{
			if (expectedIndex < expected.size() && actualIndex < actual.size() &&
			    expected[expectedIndex] == actual[actualIndex])
			{
				rows.push_back({' ', expected[expectedIndex], expectedIndex, actualIndex});
				++expectedIndex;
				++actualIndex;
				continue;
			}

			if (expectedIndex >= expected.size())
			{
				rows.push_back({'+', actual[actualIndex], kNoLine, actualIndex});
				++actualIndex;
				continue;
			}
			if (actualIndex >= actual.size())
			{
				rows.push_back({'-', expected[expectedIndex], expectedIndex, kNoLine});
				++expectedIndex;
				continue;
			}

			auto [expectedAdvance, actualAdvance] = findSync();
			expectedAdvance = std::max<size_t>(expectedAdvance, expectedAdvance == 0 && actualAdvance == 0 ? 1 : expectedAdvance);
			actualAdvance = std::max<size_t>(actualAdvance, expectedAdvance == 0 && actualAdvance == 0 ? 1 : actualAdvance);
			for (size_t i = 0; i < expectedAdvance && expectedIndex < expected.size(); ++i, ++expectedIndex)
				rows.push_back({'-', expected[expectedIndex], expectedIndex, kNoLine});
			for (size_t i = 0; i < actualAdvance && actualIndex < actual.size(); ++i, ++actualIndex)
				rows.push_back({'+', actual[actualIndex], kNoLine, actualIndex});
		}

		return rows;
	}

	std::string LineRange(const std::vector<DiffRow>& rows, size_t begin, size_t end, bool expected)
	{
		size_t first = kNoLine;
		size_t last = kNoLine;
		for (size_t i = begin; i < end; ++i)
		{
			size_t line = expected ? rows[i].expectedLine : rows[i].actualLine;
			if (line == kNoLine)
				continue;
			if (first == kNoLine)
				first = line;
			last = line;
		}

		if (first == kNoLine)
			return "none";
		return std::to_string(first + 1) + "-" + std::to_string(last + 1);
	}

	std::string HunkFunctionContext(const std::vector<DiffRow>& rows, size_t begin, size_t end,
	    const std::vector<std::string_view>& expected, const std::vector<std::string_view>& actual)
	{
		for (size_t i = begin; i < end; ++i)
		{
			if (rows[i].actualLine != kNoLine)
			{
				auto context = NearestFunctionContext(actual, rows[i].actualLine);
				if (!context.empty())
					return context;
			}
		}
		for (size_t i = begin; i < end; ++i)
		{
			if (rows[i].expectedLine != kNoLine)
			{
				auto context = NearestFunctionContext(expected, rows[i].expectedLine);
				if (!context.empty())
					return context;
			}
		}

		return {};
	}

	std::string SnapshotDiff(std::string_view expectedText, std::string_view actualText)
	{
		constexpr size_t kContextRows = 4;
		auto expected = SplitLines(expectedText);
		auto actual = SplitLines(actualText);
		auto diff = BuildLineDiff(expected, actual);
		std::vector<std::pair<size_t, size_t>> hunks;
		size_t differingRows = 0;

		for (size_t i = 0; i < diff.size(); ++i)
		{
			if (diff[i].op == ' ')
				continue;
			++differingRows;
			size_t begin = i > kContextRows ? i - kContextRows : 0;
			size_t end = std::min(diff.size(), i + kContextRows + 1);
			if (!hunks.empty() && begin <= hunks.back().second)
				hunks.back().second = std::max(hunks.back().second, end);
			else
				hunks.emplace_back(begin, end);
		}

		std::ostringstream stream;
		stream << "differing hunks " << hunks.size() << ", differing rows " << differingRows << "\n";
		stream << "--- expected\n+++ actual\n";
		for (const auto& [begin, end] : hunks)
		{
			auto functionContext = HunkFunctionContext(diff, begin, end, expected, actual);
			stream << "@@ expected " << LineRange(diff, begin, end, true) << " actual "
			       << LineRange(diff, begin, end, false) << " @@";
			if (!functionContext.empty())
				stream << " " << functionContext;
			stream << "\n";

			for (size_t i = begin; i < end; ++i)
				stream << diff[i].op << " " << diff[i].text << "\n";
		}

		return stream.str();
	}

	std::optional<std::string> ReadText(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return std::nullopt;

		std::ostringstream contents;
		contents << stream.rdbuf();
		return contents.str();
	}

	void WriteText(const std::filesystem::path& path, std::string_view contents)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(stream) << "failed to write " << path;
		stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	}

	void CheckSnapshot(const std::filesystem::path& snapshot, const std::string& actual)
	{
		if (gOverwriteSnapshots)
		{
			WriteText(snapshot, actual);
			return;
		}

		auto expected = ReadText(snapshot);
		if (!expected)
		{
			ADD_FAILURE() << "missing corpus snapshot " << snapshot << "\nrerun with --overwrite to create it";
			return;
		}

		if (*expected != actual)
		{
			ADD_FAILURE() << "corpus snapshot mismatch for " << snapshot
			              << "\nrerun with --overwrite to update it\n" << SnapshotDiff(*expected, actual);
		}
	}

	std::vector<LoadRequest> LoadRequests()
	{
		return {
			{"{}", true},
			{R"({"files.universal.architecturePreference":["x86_64"]})", false},
			{R"({"files.universal.architecturePreference":["arm64e","arm64"]})", false},
		};
	}

	void MaterializeObjCExternFixedPoint(BinaryView* view)
	{
		for (int pass = 0; pass < 6; ++pass)
		{
			view->UpdateAnalysisAndWait();
			if (WorkflowObjC::Activities::MaterializePendingObjCExterns(view) == 0)
				break;
		}
		view->UpdateAnalysisAndWait();
	}

	void ConvergeAnalysisForSnapshot(BinaryView* view)
	{
		MaterializeObjCExternFixedPoint(view);
		view->Reanalyze();
		MaterializeObjCExternFixedPoint(view);
	}

	void CheckBinary(const std::filesystem::path& binary, const std::filesystem::path& snapshotDir)
	{
		std::vector<std::string> seenViews;
		for (const auto& request : LoadRequests())
		{
			LoadedView loaded {BinaryNinja::Load(binary.string(), true, request.options)};
			if (!loaded.view)
			{
				if (request.required)
					ADD_FAILURE() << "failed to load corpus binary " << binary;
				continue;
			}

			ConvergeAnalysisForSnapshot(loaded.view);
			auto viewKey = loaded.view->GetTypeName() + ":" + ArchName(loaded.view->GetDefaultArchitecture()) +
			               ":" + PlatformName(loaded.view->GetDefaultPlatform());
			if (std::find(seenViews.begin(), seenViews.end(), viewKey) != seenViews.end())
				continue;
			seenViews.push_back(viewKey);

			SCOPED_TRACE(binary.string() + " " + viewKey);
			auto analysis = loaded.view->GetAnalysisInfo();
			EXPECT_EQ(analysis.state, IdleState) << "analysis did not complete";
			EXPECT_TRUE(analysis.activeInfo.empty()) << "analysis still has active functions";
			if (analysis.state != IdleState || !analysis.activeInfo.empty())
				continue;

			CheckSnapshot(snapshotDir / SnapshotNameFor(loaded.view, binary), DumpView(loaded.view, binary));
		}
	}
}

TEST_F(BinaryNinjaCorpusTest, BinariesMatchSnapshots)
{
	ASSERT_TRUE(WorkflowObjC::RegisterActivities());

	auto binaries = CorpusBinaries();
	ASSERT_FALSE(binaries.empty()) << "no corpus binaries found in " << (TestRoot() / "bin");

	for (const auto& binary : binaries)
		CheckBinary(binary, TestRoot() / "corpus");
}

int main(int argc, char** argv)
{
	int writeIndex = 1;
	for (int readIndex = 1; readIndex < argc; ++readIndex)
	{
		std::string_view arg(argv[readIndex]);
		if (arg == "--overwrite")
		{
			gOverwriteSnapshots = true;
			continue;
		}
		if (arg.starts_with("--corpus-binary="))
		{
			gCorpusBinaryFilter = std::string(arg.substr(std::string_view("--corpus-binary=").size()));
			continue;
		}

		argv[writeIndex++] = argv[readIndex];
	}
	argc = writeIndex;

	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

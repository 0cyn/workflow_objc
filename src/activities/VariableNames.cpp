#include "../Metadata.h"
#include "../Util.h"
#include "../Workflow.h"

#include <mediumlevelilinstruction.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace BinaryNinja;

namespace WorkflowObjC::Activities
{
	namespace
	{
		struct MethodInfo
		{
			std::string className;
			std::string selectorName;
			ObjCMethodKind methodKind = ObjCMethodKind::Instance;
		};

		struct Candidate
		{
			std::string name;
			int score = 0;
		};

		struct AliasEdge
		{
			Variable source;
			Variable dest;
			int score = 0;
			bool allowParameterPropagation = false;
		};

		struct CallInfo
		{
			std::string targetName;
			std::vector<MediumLevelILInstruction> params;
			std::vector<SSAVariable> outputs;
		};

		struct ObjCCallInfo
		{
			std::optional<MethodInfo> method;
			Selector selector;
			std::vector<MediumLevelILInstruction> params;
			std::vector<SSAVariable> outputs;
		};

		using CandidateMap = std::map<Variable, std::vector<Candidate>>;

		class VariableNamePass;

		class VariableNamingRule
		{
		public:
			virtual ~VariableNamingRule() = default;
			virtual void ApplyToFunction(VariableNamePass&) {}
			virtual void ApplyToInstruction(VariableNamePass&, const MediumLevelILInstruction&) {}
			virtual void ApplyToExpression(VariableNamePass&, const MediumLevelILInstruction&, bool) {}
			virtual void Finish(VariableNamePass&) {}
		};

		class VariableNamePass
		{
		public:
			VariableNamePass(BinaryView* view, MediumLevelILFunction* mlil) : m_view(view), m_mlil(mlil)
			{
				if (m_mlil)
					m_function = m_mlil->GetFunction();
			}

			void Run();

			static constexpr int kParameterScore = 100;
			static constexpr int kIvarScore = 95;
			static constexpr int kGetterScore = 90;
			static constexpr int kResultSelectorScore = 85;
			static constexpr int kFactoryScore = 85;
			static constexpr int kArgumentScore = 80;
			static constexpr int kCopyPropagationScore = kArgumentScore + 1;
			static constexpr int kTransparentARCPropagationScore = 100;
			static constexpr int kPropagationScore = 75;

			Function* GetFunction() const { return m_function.GetPtr(); }
			MediumLevelILFunction* GetSSAFunction() const { return m_mlilSSA.GetPtr(); }
			const std::optional<MethodInfo>& Method() const { return m_method; }
			const std::vector<AliasEdge>& Aliases() const { return m_aliases; }
			const CandidateMap& Candidates() const { return m_candidates; }

			size_t CandidateCount(const Variable& var) const
			{
				auto it = m_candidates.find(var);
				return it == m_candidates.end() ? 0 : it->second.size();
			}

			// Shared support for the rule objects, kept inside the pass to avoid file-scope helper sprawl.
			static bool IsDecimal(std::string_view text)
			{
				return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
					return std::isdigit(ch) != 0;
				});
			}

			static std::string_view StripNumericSuffix(std::string_view text)
			{
				auto suffix = text.rfind('_');
				if (suffix == std::string_view::npos || suffix == 0 || suffix + 1 >= text.size())
					return text;
				return IsDecimal(text.substr(suffix + 1)) ? text.substr(0, suffix) : text;
			}

			static bool IsRegisterTemporary(std::string_view text)
			{
				if (text.size() >= 2 && (text[0] == 'x' || text[0] == 'w' || text[0] == 'r' || text[0] == 'v') &&
				    IsDecimal(text.substr(1)))
				{
					return true;
				}

				for (auto prefix : {std::string_view("xmm"), std::string_view("ymm"), std::string_view("zmm")})
				{
					if (text.starts_with(prefix) && IsDecimal(text.substr(prefix.size())))
						return true;
				}

				static constexpr std::string_view kRegisters[] = {
					"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip", "eax", "ebx", "ecx", "edx",
					"esi", "edi", "ebp", "esp", "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "lr",
				};

				for (auto reg : kRegisters)
				{
					if (text == reg || (text.starts_with(reg) && IsDecimal(text.substr(reg.size()))))
						return true;
				}
				return false;
			}

			static bool IsReservedName(std::string_view name)
			{
				static constexpr std::string_view kReserved[] = {
					"auto", "break", "case", "char", "class", "const", "continue", "default", "do", "double", "else",
					"enum", "extern", "float", "for", "goto", "id", "if", "inline", "int", "long", "register",
					"restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
					"unsigned", "void", "volatile", "while",
				};

				for (auto reserved : kReserved)
				{
					if (name == reserved)
						return true;
				}
				return false;
			}

			static bool IsGenericAutoName(std::string_view name)
			{
				for (;;)
				{
					auto baseName = StripNumericSuffix(name);
					if (baseName == name)
						break;
					name = baseName;
				}

				if (name.empty() || name.starts_with("var_") || name.starts_with("temp") || name.starts_with("tmp"))
					return true;
				if (name.starts_with("arg") && IsDecimal(name.substr(3)))
					return true;
				return IsRegisterTemporary(name) || name == "obj" || name == "object" || name == "result" ||
				       name == "retval" || name == "ret";
			}

			static bool IsWeakName(std::string_view name)
			{
				for (;;)
				{
					if (name.empty() || name == "object" || name == "objects" || name == "obj" || name == "value" ||
					    name == "result" || name == "tmp" || name == "temp" || name == "isa" || IsReservedName(name) ||
					    IsGenericAutoName(name))
					{
						return true;
					}

					auto baseName = StripNumericSuffix(name);
					if (baseName == name)
						return false;
					name = baseName;
				}
			}

			static std::string LowerInitialAcronym(std::string name)
			{
				if (name.empty())
					return name;

				size_t uppercaseRun = 0;
				while (uppercaseRun < name.size() && std::isupper(static_cast<unsigned char>(name[uppercaseRun])))
					++uppercaseRun;

				if (uppercaseRun > 1 && uppercaseRun < name.size() &&
				    std::islower(static_cast<unsigned char>(name[uppercaseRun])))
				{
					--uppercaseRun;
				}
				for (size_t i = 0; i < uppercaseRun; ++i)
					name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
				return name;
			}

			static std::optional<std::string> SanitizeName(std::string_view rawName, bool stripIvarPrefixes = true)
			{
				std::string name(rawName);
				if (stripIvarPrefixes)
				{
					while (!name.empty() && name.front() == '_')
						name.erase(name.begin());
					if (name.starts_with("ivar_"))
						name.erase(0, 5);
					if (name.starts_with("m_") && name.size() > 2)
						name.erase(0, 2);
				}

				std::string result;
				result.reserve(name.size());
				bool uppercaseNext = false;
				for (unsigned char ch : name)
				{
					if (std::isalnum(ch))
					{
						if (result.empty() && std::isdigit(ch))
							return std::nullopt;
						result += uppercaseNext ? static_cast<char>(std::toupper(ch)) : static_cast<char>(ch);
						uppercaseNext = false;
					}
					else if (ch == '_' || ch == '-' || std::isspace(ch))
					{
						uppercaseNext = !result.empty();
					}
					else
					{
						return std::nullopt;
					}
				}

				if (result == "class")
					return "cls";
				if (result.empty() || !std::isalpha(static_cast<unsigned char>(result.front())) || IsWeakName(result))
					return std::nullopt;
				return result;
			}

			static std::optional<std::string> SelectorLabelWithoutPrefix(
			    std::string_view prefix, std::string_view label)
			{
				if (label.size() <= prefix.size() || !label.starts_with(prefix))
					return std::nullopt;

				std::string afterPrefix(label.substr(prefix.size()));
				if (afterPrefix.empty() || std::islower(static_cast<unsigned char>(afterPrefix[0])))
					return std::nullopt;
				return LowerInitialAcronym(std::move(afterPrefix));
			}

			static std::optional<std::string> SelectorLabelAfterSeparator(std::string_view label)
			{
				static constexpr std::string_view kSeparators[] = {"With", "For", "From", "To", "In", "Of", "By", "At", "Using"};
				static constexpr std::string_view kActionPrefixes[] = {"Appending", "Replacing", "Removing", "Adding"};

				for (auto separator : kSeparators)
				{
					size_t pos = label.find(separator);
					if (pos == std::string_view::npos || pos + separator.size() >= label.size())
						continue;

					std::string suffix(label.substr(pos + separator.size()));
					if (suffix.empty() || !std::isupper(static_cast<unsigned char>(suffix.front())))
						continue;

					for (auto prefix : kActionPrefixes)
					{
						if (auto stripped = SelectorLabelWithoutPrefix(prefix, suffix))
						{
							suffix = *stripped;
							break;
						}
					}

					return LowerInitialAcronym(std::move(suffix));
				}
				return std::nullopt;
			}

			static std::optional<std::string> NameFromSelectorArgumentLabel(
			    const std::vector<std::string>& labels, size_t index, bool allowWeakFallback)
			{
				if (index >= labels.size())
					return std::nullopt;

				const auto& label = labels[index];
				std::optional<std::string> candidate;
				if (index == 0)
					candidate = SelectorLabelAfterSeparator(label);

				static constexpr std::string_view kPrefixes[] = {
					"initWith", "with", "and", "using", "set", "read", "write", "to", "for", "from", "in", "at",
					"of", "by", "add", "remove", "insert", "append", "drawIn",
				};

				for (auto prefix : kPrefixes)
				{
					if (!candidate)
						candidate = SelectorLabelWithoutPrefix(prefix, label);
				}

				if (!candidate)
					candidate = LowerInitialAcronym(label);

				if (auto sanitized = SanitizeName(*candidate, false))
					return sanitized;
				if (!allowWeakFallback)
					return std::nullopt;

				std::string fallback = GenerateArgumentNames({label}).front();
				if (fallback != *candidate)
					return SanitizeName(fallback, false);
				return std::nullopt;
			}

			static std::string StripFrameworkPrefix(std::string className)
			{
				static constexpr std::string_view kPrefixes[] = {
					"NS", "UI", "CF", "CG", "CA", "AV", "WK", "MK", "CI", "CL", "SK", "SCN", "MTL",
				};

				for (auto prefix : kPrefixes)
				{
					if (className.size() > prefix.size() + 1 && className.starts_with(prefix) &&
					    std::isupper(static_cast<unsigned char>(className[prefix.size()])))
					{
						className.erase(0, prefix.size());
						break;
					}
				}
				return className;
			}

			static std::optional<std::string> NameFromClassName(std::string_view className)
			{
				if (className.empty() || className.front() == '_' || className.find("Constant") != std::string_view::npos)
					return std::nullopt;

				auto base = LowerInitialAcronym(StripFrameworkPrefix(std::string(className)));
				return SanitizeName(base, false);
			}

			static std::optional<std::string> NameFromType(Type* type)
			{
				if (auto className = ClassNameFromType(type))
					return NameFromClassName(*className);
				return std::nullopt;
			}

			static std::optional<MethodInfo> ParseObjCMethodSymbolName(std::string_view symbolName)
			{
				if (symbolName.size() < 5 || (symbolName.front() != '-' && symbolName.front() != '+') ||
				    symbolName[1] != '[' || symbolName.back() != ']')
				{
					return std::nullopt;
				}

				size_t separator = symbolName.rfind(' ', symbolName.size() - 2);
				if (separator == std::string_view::npos || separator <= 2 || separator + 1 >= symbolName.size() - 1)
					return std::nullopt;

				MethodInfo method;
				method.methodKind = symbolName.front() == '+' ? ObjCMethodKind::Class : ObjCMethodKind::Instance;
				method.className = std::string(symbolName.substr(2, separator - 2));
				if (size_t category = method.className.find(" ("); category != std::string::npos)
					method.className.resize(category);
				method.selectorName = std::string(symbolName.substr(separator + 1, symbolName.size() - separator - 2));
				if (method.className.empty() || method.selectorName.empty())
					return std::nullopt;
				return method;
			}

			void AddCandidate(const Variable& var, std::optional<std::string> name, int score)
			{
				if (!name || IsWeakName(*name))
					return;
				m_candidates[var].push_back({std::move(*name), score});
			}

			void AddBidirectionalAlias(
			    const Variable& first, const Variable& second, int score, bool allowParameterPropagation)
			{
				if (first == second)
					return;
				m_aliases.push_back({first, second, score, allowParameterPropagation});
				m_aliases.push_back({second, first, score, allowParameterPropagation});
			}

			static std::optional<Candidate> BestCandidate(const std::vector<Candidate>& varCandidates)
			{
				std::map<std::string, std::pair<int, size_t>> bestByName;
				for (size_t i = 0; i < varCandidates.size(); ++i)
				{
					const auto& candidate = varCandidates[i];
					if (IsWeakName(candidate.name))
						continue;

					auto [it, inserted] = bestByName.emplace(candidate.name, std::pair(candidate.score, i));
					if (!inserted && candidate.score > it->second.first)
						it->second = {candidate.score, i};
				}

				if (bestByName.empty())
					return std::nullopt;

				int bestScore = 0;
				for (const auto& [name, summary] : bestByName)
				{
					(void)name;
					bestScore = std::max(bestScore, summary.first);
				}

				std::optional<std::string> bestName;
				size_t bestIndex = varCandidates.size();
				for (const auto& [name, summary] : bestByName)
				{
					if (summary.first != bestScore)
						continue;
					if (bestName && bestScore < kResultSelectorScore)
						return std::nullopt;
					if (!bestName || summary.second < bestIndex)
					{
						bestName = name;
						bestIndex = summary.second;
					}
				}

				if (!bestName)
					return std::nullopt;
				return Candidate {*bestName, bestScore};
			}

			std::optional<Candidate> BestCandidateForVar(const Variable& var) const
			{
				auto it = m_candidates.find(var);
				if (it == m_candidates.end())
					return std::nullopt;
				return BestCandidate(it->second);
			}

			static int BestScoreForCandidateName(const std::vector<Candidate>& varCandidates, std::string_view name)
			{
				auto baseName = StripNumericSuffix(name);
				int bestScore = 0;
				for (const auto& candidate : varCandidates)
				{
					std::string_view candidateName(candidate.name);
					if (candidateName == name || (baseName != name && candidateName == baseName))
						bestScore = std::max(bestScore, candidate.score);
				}
				return bestScore;
			}

			static std::vector<SSAVariable> CallOutputs(const MediumLevelILInstruction& instr)
			{
				switch (instr.operation)
				{
				case MLIL_CALL_SSA:
					return instr.GetOutputSSAVariables<MLIL_CALL_SSA>();
				case MLIL_TAILCALL_SSA:
					return instr.GetOutputSSAVariables<MLIL_TAILCALL_SSA>();
				default:
					return {};
				}
			}

			static std::optional<Variable> VariableFromExpr(const MediumLevelILInstruction& expr)
			{
				switch (expr.operation)
				{
				case MLIL_VAR_SSA:
					return expr.GetSourceSSAVariable<MLIL_VAR_SSA>().var;
				case MLIL_ADDRESS_OF:
					return expr.GetSourceVariable<MLIL_ADDRESS_OF>();
				case MLIL_ADDRESS_OF_FIELD:
					return expr.GetSourceVariable<MLIL_ADDRESS_OF_FIELD>();
				case MLIL_CALL_SSA:
				case MLIL_TAILCALL_SSA:
				{
					auto outputs = CallOutputs(expr);
					return outputs.size() == 1 ? std::optional<Variable>(outputs[0].var) : std::nullopt;
				}
				default:
					return std::nullopt;
				}
			}

			static std::optional<Variable> DirectAddressedVariableFromExpr(const MediumLevelILInstruction& expr)
			{
				if (expr.operation == MLIL_ADDRESS_OF)
					return expr.GetSourceVariable<MLIL_ADDRESS_OF>();
				return std::nullopt;
			}

			static std::optional<Variable> ArgumentVariableFromExpr(const MediumLevelILInstruction& expr)
			{
				switch (expr.operation)
				{
				case MLIL_VAR_SSA:
					return expr.GetSourceSSAVariable<MLIL_VAR_SSA>().var;
				case MLIL_CALL_SSA:
				case MLIL_TAILCALL_SSA:
				{
					auto outputs = CallOutputs(expr);
					return outputs.size() == 1 ? std::optional<Variable>(outputs[0].var) : std::nullopt;
				}
				default:
					return std::nullopt;
				}
			}

			Ref<Type> ResolveNamedType(Type* type) const
			{
				if (!m_view || !type || !type->IsNamedTypeRefer())
					return type;

				auto ref = type->GetNamedTypeReference();
				if (!ref)
					return type;
				if (auto resolved = m_view->GetTypeByRef(ref))
					return resolved;
				if (auto resolved = m_view->GetTypeByName(ref->GetName()))
					return resolved;
				return type;
			}

			std::optional<std::string> MemberNameFromTypeAtOffset(
			    Confidence<Ref<Type>> type, uint64_t offset) const
			{
				if (type.IsUnknown() || !type.GetValue())
					return std::nullopt;

				Ref<Type> memberBase = type.GetValue();
				if (memberBase->IsPointer())
				{
					auto child = memberBase->GetChildType();
					if (child.IsUnknown() || !child.GetValue())
						return std::nullopt;
					memberBase = child.GetValue();
				}

				memberBase = ResolveNamedType(memberBase);
				if (!memberBase || !memberBase->IsStructure())
					return std::nullopt;

				InheritedStructureMember inheritedMember;
				if (memberBase->GetStructure()->GetMemberIncludingInheritedAtOffset(
				        m_view, static_cast<int64_t>(offset), inheritedMember))
				{
					return SanitizeName(inheritedMember.member.name, true);
				}

				StructureMember member;
				if (memberBase->GetStructure()->GetMemberAtOffset(static_cast<int64_t>(offset), member))
					return SanitizeName(member.name, true);
				return std::nullopt;
			}

			std::optional<std::string> MemberNameAtOffset(const MediumLevelILInstruction& base, uint64_t offset) const
			{
				std::optional<Confidence<Ref<Type>>> type;
				if (base.operation == MLIL_VAR_SSA)
				{
					auto varType = m_function->GetVariableType(base.GetSourceSSAVariable<MLIL_VAR_SSA>().var);
					if (!varType.IsUnknown() && varType.GetValue())
						type = varType;
				}

				if (!type)
				{
					auto exprType = base.GetType();
					if (!exprType.IsUnknown() && exprType.GetValue())
						type = exprType;
				}

				return type ? MemberNameFromTypeAtOffset(*type, offset) : std::nullopt;
			}

			std::optional<std::string> MemberNameFromAddressOfField(const MediumLevelILInstruction& expr) const
			{
				if (expr.operation != MLIL_ADDRESS_OF_FIELD)
					return std::nullopt;
				return MemberNameFromTypeAtOffset(m_function->GetVariableType(expr.GetSourceVariable<MLIL_ADDRESS_OF_FIELD>()),
				    expr.GetOffset<MLIL_ADDRESS_OF_FIELD>());
			}

			static std::string FirstSelectorLabel(std::string_view selectorName)
			{
				size_t colon = selectorName.find(':');
				return colon == std::string_view::npos ? std::string(selectorName) : std::string(selectorName.substr(0, colon));
			}

			static bool IsWeakSelectorResultName(std::string_view name)
			{
				static constexpr std::string_view kWeakResultNames[] = {
					"alloc", "bool", "boolean", "comparisonResult", "copy", "count", "double", "float", "hash", "index",
					"init", "integer", "length", "location", "mutableCopy", "new", "range", "unsignedInteger",
				};

				auto hasPredicatePrefix = [&](std::string_view prefix) {
					return name.size() > prefix.size() && name.starts_with(prefix) &&
					       std::isupper(static_cast<unsigned char>(name[prefix.size()]));
				};

				if (IsWeakName(name) || hasPredicatePrefix("can") || hasPredicatePrefix("has") || hasPredicatePrefix("is") ||
				    hasPredicatePrefix("should"))
				{
					return true;
				}

				for (auto weakName : kWeakResultNames)
				{
					if (name == weakName)
						return true;
				}
				return false;
			}

			static std::optional<std::string> NameFromSelectorResultLabel(std::string_view selectorName)
			{
				if (selectorName.find(':') == std::string_view::npos)
					return std::nullopt;

				std::string firstLabel = FirstSelectorLabel(selectorName);
				static constexpr std::string_view kSeparators[] = {"With", "For", "From", "To", "In", "Of", "By", "At", "Using"};

				size_t bestSeparator = std::string::npos;
				for (auto separator : kSeparators)
				{
					size_t pos = firstLabel.find(separator);
					if (pos == std::string::npos || pos == 0 || pos + separator.size() >= firstLabel.size())
						continue;
					if (!std::isupper(static_cast<unsigned char>(firstLabel[pos + separator.size()])))
						continue;
					bestSeparator = std::min(bestSeparator, pos);
				}

				if (bestSeparator == std::string::npos)
					return std::nullopt;

				auto name = SanitizeName(LowerInitialAcronym(firstLabel.substr(0, bestSeparator)), false);
				if (!name || IsWeakSelectorResultName(*name))
					return std::nullopt;
				return name;
			}

			static bool StartsWithSetFamily(std::string_view selectorName)
			{
				return selectorName.starts_with("set") && selectorName.size() > 3 &&
				       (std::isupper(static_cast<unsigned char>(selectorName[3])) != 0 || selectorName[3] == ':');
			}

			static bool IsOwnershipSelector(std::string_view selectorName)
			{
				Selector selector {std::string(selectorName), 0};
				return selector.IsInitFamily() || selectorName == "alloc" || selectorName == "new" || selectorName == "copy" ||
				       selectorName == "mutableCopy" || selectorName.starts_with("allocWith") || selectorName.starts_with("newWith");
			}

			static bool IsFactorySelector(const MethodInfo& method)
			{
				if (method.methodKind != ObjCMethodKind::Class)
					return false;

				auto className = NameFromClassName(method.className);
				if (!className)
					return false;

				std::string firstLabel = LowerInitialAcronym(FirstSelectorLabel(method.selectorName));
				if (firstLabel.starts_with(*className))
					return true;

				std::string classComponent = StripFrameworkPrefix(method.className);
				if (!classComponent.empty() && firstLabel.find(classComponent) != std::string::npos)
					return true;

				classComponent = LowerInitialAcronym(std::move(classComponent));
				return !classComponent.empty() && firstLabel.find(classComponent) != std::string::npos;
			}

			static bool IsSingletonFactorySelector(std::string_view selectorName)
			{
				if (selectorName.find(':') != std::string_view::npos)
					return false;

				for (auto prefix : {std::string_view("shared"), std::string_view("default"), std::string_view("standard"),
				         std::string_view("current"), std::string_view("main")})
				{
					if (selectorName.size() > prefix.size() && selectorName.starts_with(prefix) &&
					    std::isupper(static_cast<unsigned char>(selectorName[prefix.size()])))
					{
						return true;
					}
				}
				return false;
			}

			static std::optional<std::string> NameFromClassMethodResult(const MethodInfo& method, Type* outputType)
			{
				if (method.methodKind != ObjCMethodKind::Class)
					return std::nullopt;
				if (auto name = NameFromType(outputType))
					return name;
				return NameFromClassName(method.className);
			}

			static std::optional<Candidate> NameForCallResult(
			    const MethodInfo* method, std::string_view selectorName, Type* outputType)
			{
				if (selectorName.empty() || StartsWithSetFamily(selectorName))
					return std::nullopt;

				if (IsOwnershipSelector(selectorName))
				{
					Selector selector {std::string(selectorName), 0};
					if (selector.IsInitFamily() && !method)
						return std::nullopt;
					if (auto name = NameFromType(outputType))
						return Candidate {*name, kFactoryScore};
					if (method)
					{
						if (auto name = NameFromClassName(method->className))
							return Candidate {*name, kFactoryScore};
					}
				}

				if (method && (IsFactorySelector(*method) || IsSingletonFactorySelector(selectorName)))
				{
					if (auto name = NameFromClassMethodResult(*method, outputType))
						return Candidate {*name, kFactoryScore};
				}

				if (auto name = NameFromSelectorResultLabel(selectorName))
					return Candidate {*name, kResultSelectorScore};

				if (selectorName.find(':') == std::string_view::npos)
				{
					auto getterName = SanitizeName(LowerInitialAcronym(std::string(selectorName)), false);
					if (getterName)
						return Candidate {*getterName, kGetterScore};
				}
				return std::nullopt;
			}

			static std::string_view StripJumpThunkPrefix(std::string_view name)
			{
				return name.starts_with("j__") ? name.substr(3) : name;
			}

			static bool IsARCRegisterSuffix(std::string_view suffix)
			{
				return suffix.size() > 2 && suffix.front() == '_' &&
				       (suffix[1] == 'x' || suffix[1] == 'w' || suffix[1] == 'r') && IsDecimal(suffix.substr(2));
			}

			static bool IsARCFunctionName(std::string_view name, std::span<const std::string_view> baseNames)
			{
				name = StripJumpThunkPrefix(name);
				for (auto candidate : baseNames)
				{
					auto baseName = StripJumpThunkPrefix(candidate);
					if (name == baseName || (name.starts_with(baseName) && IsARCRegisterSuffix(name.substr(baseName.size()))))
						return true;
				}
				return false;
			}

			static bool IsObjCMsgSendName(std::string_view name)
			{
				static constexpr std::string_view kMsgSendFunctions[] = {
					"_objc_msgSend", "_objc_msgSendSuper", "_objc_msgSendSuper2", "j__objc_msgSend",
					"j__objc_msgSendSuper", "j__objc_msgSendSuper2",
				};

				for (auto msgSend : kMsgSendFunctions)
				{
					if (name == msgSend)
						return true;
				}
				return false;
			}

			std::optional<uint64_t> ConstantPointerFromExpr(const MediumLevelILInstruction& expr) const
			{
				if (auto value = MatchConstantPointerOrLoadOfConstantPointer(expr))
					return value;

				if (expr.operation == MLIL_CONST || expr.operation == MLIL_CONST_PTR)
					return static_cast<uint64_t>(expr.GetConstant());
				if (expr.operation == MLIL_EXTERN_PTR)
					return static_cast<uint64_t>(expr.GetConstant<MLIL_EXTERN_PTR>());
				if (expr.operation == MLIL_IMPORT)
					return static_cast<uint64_t>(expr.GetConstant<MLIL_IMPORT>());

				if (expr.operation == MLIL_VAR_SSA && expr.function)
				{
					auto value = expr.function->GetSSAVarValue(expr.GetSourceSSAVariable<MLIL_VAR_SSA>());
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

			std::optional<Selector> SelectorFromCallParams(const std::vector<MediumLevelILInstruction>& params) const
			{
				if (params.size() < 2)
					return std::nullopt;

				auto selectorValue = ConstantPointerFromExpr(params[1]);
				if (!selectorValue || *selectorValue == 0)
					return std::nullopt;
				return Selector::FromAddress(m_view, *selectorValue);
			}

			std::optional<CallInfo> CallInfoForInstruction(const MediumLevelILInstruction& instr) const
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

				if (!m_view)
					return std::nullopt;

				auto target = ConstantPointerFromExpr(dest);
				if (!target)
					return std::nullopt;

				auto targetSymbol = m_view->GetSymbolByAddress(*target);
				if (!targetSymbol)
					return std::nullopt;

				std::string targetName = targetSymbol->GetRawName();
				if (targetName.empty())
					targetName = targetSymbol->GetFullName();
				if (targetName.empty())
					return std::nullopt;

				return CallInfo {std::move(targetName), std::move(params), CallOutputs(instr)};
			}

			std::optional<ObjCCallInfo> ObjCCallInfoFromCallInfo(const CallInfo& call) const
			{
				if (auto method = ParseObjCMethodSymbolName(call.targetName))
					return ObjCCallInfo {*method, Selector {method->selectorName, 0}, call.params, call.outputs};

				if (!IsObjCMsgSendName(call.targetName))
					return std::nullopt;

				auto selector = SelectorFromCallParams(call.params);
				if (!selector)
					return std::nullopt;
				return ObjCCallInfo {std::nullopt, *selector, call.params, call.outputs};
			}

			static constexpr std::string_view kTransparentARCReturnFunctions[] = {
				"_objc_retain", "_objc_retainBlock", "_objc_retainAutoreleasedReturnValue", "_objc_retainAutorelease",
				"_objc_retainAutoreleaseReturnValue", "_objc_autorelease", "_objc_autoreleaseReturnValue",
				"_objc_claimAutoreleasedReturnValue", "_objc_unsafeClaimAutoreleasedReturnValue", "j__objc_retain",
				"j__objc_retainBlock", "j__objc_retainAutoreleasedReturnValue", "j__objc_retainAutorelease",
				"j__objc_retainAutoreleaseReturnValue", "j__objc_autorelease", "j__objc_autoreleaseReturnValue",
				"j__objc_claimAutoreleasedReturnValue", "j__objc_unsafeClaimAutoreleasedReturnValue",
			};

			static constexpr std::string_view kARCLoadFunctions[] = {
				"_objc_loadWeak", "_objc_loadWeakRetained", "j__objc_loadWeak", "j__objc_loadWeakRetained",
			};

			static constexpr std::string_view kARCStoreFunctions[] = {
				"_objc_initWeak", "_objc_storeStrong", "_objc_storeWeak", "j__objc_initWeak", "j__objc_storeStrong",
				"j__objc_storeWeak",
			};

			static constexpr std::string_view kARCMoveFunctions[] = {
				"_objc_copyWeak", "_objc_moveWeak", "j__objc_copyWeak", "j__objc_moveWeak",
			};

		private:
			Ref<BinaryView> m_view;
			Ref<MediumLevelILFunction> m_mlil;
			Ref<Function> m_function;
			Ref<MediumLevelILFunction> m_mlilSSA;
			std::optional<MethodInfo> m_method;
			CandidateMap m_candidates;
			std::vector<AliasEdge> m_aliases;
		};

		class MethodParameterNameRule final : public VariableNamingRule
		{
		public:
			void ApplyToFunction(VariableNamePass& pass) override
			{
				auto function = pass.GetFunction();
				const auto& method = pass.Method();
				auto params = function->GetParameterVariables();
				if (!method || params.IsUnknown())
					return;

				Selector selector {method->selectorName, 0};
				const auto& variables = params.GetValue();
				if (!variables.empty())
					pass.AddCandidate(
					    variables[0], VariableNamePass::SanitizeName("self", false), VariableNamePass::kParameterScore);
				if (variables.size() > 1)
					pass.AddCandidate(
					    variables[1], VariableNamePass::SanitizeName("sel", false), VariableNamePass::kParameterScore);

				auto labels = selector.ArgumentLabels();
				for (size_t i = 0; i < labels.size() && i + 2 < variables.size(); ++i)
				{
					pass.AddCandidate(variables[i + 2], VariableNamePass::NameFromSelectorArgumentLabel(labels, i, true),
					    VariableNamePass::kParameterScore);
				}
			}
		};

		class IvarMemberNameRule final : public VariableNamingRule
		{
		public:
			void ApplyToInstruction(VariableNamePass& pass, const MediumLevelILInstruction& instr) override
			{
				if (instr.operation == MLIL_SET_VAR_SSA)
				{
					auto source = instr.GetSourceExpr<MLIL_SET_VAR_SSA>();
					if (source.operation == MLIL_LOAD_STRUCT_SSA)
					{
						pass.AddCandidate(instr.GetDestSSAVariable<MLIL_SET_VAR_SSA>().var,
						    pass.MemberNameAtOffset(source.GetSourceExpr<MLIL_LOAD_STRUCT_SSA>(),
						        source.GetOffset<MLIL_LOAD_STRUCT_SSA>()),
						    VariableNamePass::kIvarScore);
					}
				}

				if (instr.operation != MLIL_STORE_STRUCT_SSA)
					return;

				auto sourceVariable = VariableNamePass::VariableFromExpr(instr.GetSourceExpr<MLIL_STORE_STRUCT_SSA>());
				if (!sourceVariable)
					return;

				pass.AddCandidate(*sourceVariable,
				    pass.MemberNameAtOffset(instr.GetDestExpr<MLIL_STORE_STRUCT_SSA>(), instr.GetOffset<MLIL_STORE_STRUCT_SSA>()),
				    VariableNamePass::kIvarScore);
			}
		};

		class CopyAliasRule final : public VariableNamingRule
		{
		public:
			void ApplyToInstruction(VariableNamePass& pass, const MediumLevelILInstruction& instr) override
			{
				if (instr.operation != MLIL_SET_VAR_SSA)
					return;

				auto dest = instr.GetDestSSAVariable<MLIL_SET_VAR_SSA>().var;
				auto source = VariableNamePass::VariableFromExpr(instr.GetSourceExpr<MLIL_SET_VAR_SSA>());
				if (source && *source != dest)
				{
					pass.AddBidirectionalAlias(
					    *source, dest, VariableNamePass::kCopyPropagationScore, false);
				}
			}
		};

		class MessageSendAndARCRule final : public VariableNamingRule
		{
		public:
			void ApplyToExpression(
			    VariableNamePass& pass, const MediumLevelILInstruction& instr, bool allowResultCandidate) override
			{
				auto function = pass.GetFunction();
				auto call = pass.CallInfoForInstruction(instr);
				if (!call)
					return;

				if (auto objcCall = pass.ObjCCallInfoFromCallInfo(*call))
				{
					if (allowResultCandidate && objcCall->outputs.size() == 1)
					{
						auto output = objcCall->outputs[0].var;
						auto outputType = function->GetVariableType(output);
						Type* type = outputType.IsUnknown() ? nullptr : outputType.GetValue().GetPtr();
						if (auto name = VariableNamePass::NameForCallResult(
						        objcCall->method ? &*objcCall->method : nullptr, objcCall->selector.name, type))
						{
							pass.AddCandidate(output, name->name, name->score);
						}
					}

					auto labels = objcCall->selector.ArgumentLabels();
					for (size_t i = 0; i < labels.size(); ++i)
					{
						size_t paramIndex = i + 2;
						if (paramIndex >= objcCall->params.size())
							break;

						auto variable = VariableNamePass::ArgumentVariableFromExpr(objcCall->params[paramIndex]);
						if (variable)
						{
							pass.AddCandidate(*variable,
							    VariableNamePass::NameFromSelectorArgumentLabel(labels, i, false),
							    VariableNamePass::kArgumentScore);
						}
					}
				}

				if (VariableNamePass::IsARCFunctionName(call->targetName, VariableNamePass::kTransparentARCReturnFunctions))
				{
					if (call->params.empty() || call->outputs.size() != 1)
						return;

					if (auto sourceCall = pass.CallInfoForInstruction(call->params[0]))
					{
						if (auto sourceObjCCall = pass.ObjCCallInfoFromCallInfo(*sourceCall))
						{
							auto output = call->outputs[0].var;
							auto outputType = function->GetVariableType(output);
							Type* type = outputType.IsUnknown() ? nullptr : outputType.GetValue().GetPtr();
							if (auto name = VariableNamePass::NameForCallResult(sourceObjCCall->method ? &*sourceObjCCall->method : nullptr,
							        sourceObjCCall->selector.name, type))
							{
								pass.AddCandidate(output, name->name, name->score);
							}
						}
					}

					auto sourceVariable = VariableNamePass::VariableFromExpr(call->params[0]);
					if (sourceVariable)
					{
						pass.AddBidirectionalAlias(*sourceVariable, call->outputs[0].var,
						    VariableNamePass::kTransparentARCPropagationScore, true);
					}
					return;
				}

				if (VariableNamePass::IsARCFunctionName(call->targetName, VariableNamePass::kARCLoadFunctions))
				{
					if (call->params.empty() || call->outputs.size() != 1)
						return;

					if (auto sourceVariable = VariableNamePass::DirectAddressedVariableFromExpr(call->params[0]))
					{
						pass.AddBidirectionalAlias(
						    *sourceVariable, call->outputs[0].var, VariableNamePass::kPropagationScore, true);
					}
					else
					{
						pass.AddCandidate(
						    call->outputs[0].var, pass.MemberNameFromAddressOfField(call->params[0]), VariableNamePass::kIvarScore);
					}
					return;
				}

				if (VariableNamePass::IsARCFunctionName(call->targetName, VariableNamePass::kARCStoreFunctions))
				{
					if (call->params.size() < 2)
						return;

					auto sourceVariable = VariableNamePass::VariableFromExpr(call->params[1]);
					if (!sourceVariable)
						return;

					if (auto destVariable = VariableNamePass::DirectAddressedVariableFromExpr(call->params[0]))
					{
						pass.AddBidirectionalAlias(
						    *sourceVariable, *destVariable, VariableNamePass::kPropagationScore, true);
					}
					else
					{
						pass.AddCandidate(
						    *sourceVariable, pass.MemberNameFromAddressOfField(call->params[0]), VariableNamePass::kIvarScore);
					}
					return;
				}

				if (VariableNamePass::IsARCFunctionName(call->targetName, VariableNamePass::kARCMoveFunctions))
				{
					if (call->params.size() < 2)
						return;

					auto destVariable = VariableNamePass::DirectAddressedVariableFromExpr(call->params[0]);
					auto sourceVariable = VariableNamePass::DirectAddressedVariableFromExpr(call->params[1]);
					if (destVariable && sourceVariable)
					{
						pass.AddBidirectionalAlias(
						    *sourceVariable, *destVariable, VariableNamePass::kPropagationScore, true);
						return;
					}

					if (destVariable)
						pass.AddCandidate(*destVariable, pass.MemberNameFromAddressOfField(call->params[1]), VariableNamePass::kIvarScore);
				}
			}
		};

		class AliasPropagationRule final : public VariableNamingRule
		{
		public:
			void Finish(VariableNamePass& pass) override
			{
				for (size_t passIndex = 0; passIndex < 4; ++passIndex)
				{
					bool changed = false;
					for (const auto& edge : pass.Aliases())
					{
						auto source = pass.BestCandidateForVar(edge.source);
						if (!source || VariableNamePass::IsWeakName(source->name) || source->name == "self" ||
						    source->name == "sel" || source->name == "super")
						{
							continue;
						}
						if (source->score == VariableNamePass::kParameterScore && !edge.allowParameterPropagation)
							continue;

						size_t before = pass.CandidateCount(edge.dest);
						pass.AddCandidate(edge.dest, source->name, std::min(source->score, edge.score));
						changed |= pass.CandidateCount(edge.dest) != before;
					}

					if (!changed)
						break;
				}
			}
		};

		class CandidateRenameRule final : public VariableNamingRule
		{
		public:
			void Finish(VariableNamePass& pass) override
			{
				auto function = pass.GetFunction();
				auto mlilSSA = pass.GetSSAFunction();
				std::map<uint64_t, Confidence<Ref<Type>>> callTypeAdjustments;
				for (const auto& block : mlilSSA->GetBasicBlocks())
				{
					for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
					{
						auto instr = mlilSSA->GetInstruction(i);
						instr.VisitExprs([&](const MediumLevelILInstruction& expr) {
							if ((expr.operation != MLIL_CALL_SSA && expr.operation != MLIL_TAILCALL_SSA) || expr.address == 0 ||
							    callTypeAdjustments.contains(expr.address))
							{
								return true;
							}

							auto adjustment = function->GetCallTypeAdjustment(function->GetArchitecture(), expr.address);
							if (!adjustment.IsUnknown() && adjustment.GetValue())
								callTypeAdjustments.emplace(expr.address, adjustment);
							return true;
						});
					}
				}

				auto variables = function->GetVariables();
				std::set<std::string> protectedNames;
				for (const auto& [var, info] : variables)
				{
					(void)info;
					std::string currentName = function->GetVariableNameOrDefault(var);
					if (function->IsVariableUserDefinded(var) && !VariableNamePass::IsGenericAutoName(currentName))
						protectedNames.insert(std::move(currentName));
				}

				std::set<Variable> parameterVariables;
				auto params = function->GetParameterVariables();
				if (!params.IsUnknown())
				{
					const auto& values = params.GetValue();
					parameterVariables.insert(values.begin(), values.end());
				}

				std::map<Variable, Variable> aliasParents;
				auto ensureAliasRoot = [&](const Variable& var) {
					aliasParents.try_emplace(var, var);
				};
				auto aliasRoot = [&](Variable var) {
					for (;;)
					{
						auto it = aliasParents.find(var);
						if (it == aliasParents.end() || it->second == var)
							return var;
						var = it->second;
					}
				};
				for (const auto& edge : pass.Aliases())
				{
					ensureAliasRoot(edge.source);
					ensureAliasRoot(edge.dest);
					auto sourceRoot = aliasRoot(edge.source);
					auto destRoot = aliasRoot(edge.dest);
					if (sourceRoot == destRoot)
						continue;
					if (aliasParents.key_comp()(destRoot, sourceRoot))
						std::swap(sourceRoot, destRoot);
					aliasParents[destRoot] = sourceRoot;
				}

				std::map<std::string, Variable> usedNameOwners;
				auto uniqueNameForOwner = [&](std::string_view baseName, const Variable& owner) {
					auto canUse = [&](const std::string& name) {
						if (protectedNames.contains(name))
							return false;
						auto it = usedNameOwners.find(name);
						return it == usedNameOwners.end() || it->second == owner;
					};

					std::string name(baseName);
					if (canUse(name))
						return name;

					for (size_t suffix = 1;; ++suffix)
					{
						name = std::string(baseName) + "_" + std::to_string(suffix);
						if (canUse(name))
							return name;
					}
				};

				for (const auto& [var, varCandidates] : pass.Candidates())
				{
					if (function->IsVariableUserDefinded(var))
						continue;

					std::string currentName = function->GetVariableNameOrDefault(var);
					auto best = VariableNamePass::BestCandidate(varCandidates);
					if (!best)
						continue;

					bool shouldReplace = VariableNamePass::StripNumericSuffix(currentName) == best->name ||
					                     VariableNamePass::IsGenericAutoName(currentName);
					if (!shouldReplace)
					{
						int currentScore = VariableNamePass::BestScoreForCandidateName(varCandidates, currentName);
						shouldReplace = currentScore != 0 && best->score > currentScore;
					}
					if (!shouldReplace)
						continue;
					if (best->score == VariableNamePass::kParameterScore && !parameterVariables.contains(var))
						continue;
					if (protectedNames.contains(best->name))
						continue;

					auto varInfo = variables.find(var);
					if (varInfo == variables.end())
						continue;

					auto type = function->GetVariableType(var);
					if (type.IsUnknown())
						type = varInfo->second.type;
					if (type.IsUnknown() || !type.GetValue())
						continue;

					bool canRenameWithType = type.GetValue()->IsPointer();
					if (!canRenameWithType)
					{
						auto typeName = type.GetValue()->GetString();
						canRenameWithType = typeName == "id" || typeName == "Class";
					}

					if (!canRenameWithType)
					{
						if (best->score == VariableNamePass::kArgumentScore && var.type == RegisterVariableSourceType)
						{
							auto arch = function->GetArchitecture();
							if (arch)
							{
								auto defaultName = arch->GetRegisterName(static_cast<uint32_t>(var.storage));
								if (!defaultName.empty() && currentName != defaultName)
									function->CreateAutoVariable(var, type, defaultName);
							}
						}
						continue;
					}

					auto owner = aliasRoot(var);
					std::string targetName = uniqueNameForOwner(best->name, owner);
					usedNameOwners.emplace(targetName, owner);
					if (targetName != currentName)
						function->CreateAutoVariable(var, type, targetName);
				}

				auto arch = function->GetArchitecture();
				for (const auto& [address, adjustment] : callTypeAdjustments)
					function->SetAutoCallTypeAdjustment(arch, address, adjustment);
			}
		};

		void VariableNamePass::Run()
		{
			if (!m_mlil || !m_function)
				return;

			auto symbol = m_function->GetSymbol();
			if (!symbol)
				return;

			m_method = ParseObjCMethodSymbolName(symbol->GetRawName());
			if (!m_method)
				return;

			m_mlilSSA = m_mlil->GetSSAForm();
			if (!m_mlilSSA)
				return;

			MethodParameterNameRule methodParameterNames;
			IvarMemberNameRule ivarMemberNames;
			CopyAliasRule copyAliases;
			MessageSendAndARCRule messageSendAndARC;
			AliasPropagationRule aliasPropagation;
			CandidateRenameRule candidateRenames;
			VariableNamingRule* rules[] = {
				&methodParameterNames,
				&ivarMemberNames,
				&copyAliases,
				&messageSendAndARC,
				&aliasPropagation,
				&candidateRenames,
			};

			for (auto* rule : rules)
				rule->ApplyToFunction(*this);

			for (const auto& block : m_mlilSSA->GetBasicBlocks())
			{
				for (size_t i = block->GetStart(); i < block->GetEnd(); ++i)
				{
					auto instr = m_mlilSSA->GetInstruction(i);
					for (auto* rule : rules)
						rule->ApplyToInstruction(*this, instr);

					instr.VisitExprs([&](const MediumLevelILInstruction& expr) {
						for (auto* rule : rules)
							rule->ApplyToExpression(*this, expr, expr.exprIndex == instr.exprIndex);
						return true;
					});
				}
			}

			for (auto* rule : rules)
				rule->Finish(*this);
		}
	}

	void ProcessVariableNames(Ref<AnalysisContext> ac)
	{
		auto view = ac->GetBinaryView();
		if (GlobalState::ShouldIgnoreView(view))
			return;

		VariableNamePass(view, ac->GetMediumLevelILFunction()).Run();
	}
}

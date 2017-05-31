//------------------------------------------------------------------------------
// Symbol.h
// Symbols for semantic analysis.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "diagnostics/Diagnostics.h"
#include "numeric/SVInt.h"
#include "parsing/AllSyntax.h"
#include "text/SourceLocation.h"
#include "util/ArrayRef.h"
#include "util/HashMap.h"
#include "util/StringRef.h"

#include "ConstantValue.h"

namespace slang {

class BoundExpression;
class BoundStatement;
class BoundStatementList;
class SyntaxTree;
class Symbol;
class ScopeSymbol;
class DesignRootSymbol;
class TypeSymbol;

using SymbolList = ArrayRef<const Symbol*>;
using SymbolMap = std::unordered_map<StringRef, const Symbol*>;

using Dimensions = ArrayRef<ConstantRange>;

enum class SymbolKind {
    Unknown,
	Root,
    IntegralType,
    RealType,
    StringType,
    CHandleType,
    VoidType,
    EventType,
    EnumType,
    TypeAlias,
    Parameter,
    EnumValue,
    Module,
    Interface, // TODO: decouple interfaces from modules
    Modport,   // TODO: decouple interfaces from modules
    Program,
    Attribute,
    Genvar,
    GenerateBlock,
    ProceduralBlock,
    Variable,
    Instance,
    FormalArgument,
    Subroutine
};

/// Specifies the storage lifetime of a variable.
enum class VariableLifetime {
    Automatic,
    Static
};

/// Specifies behavior of an argument passed to a subroutine.
enum class FormalArgumentDirection {
    In,
    Out,
    InOut,
    Ref,
    ConstRef
};

/// Indicates which built-in system function is represented by a symbol.
enum class SystemFunction {
    Unknown,
    clog2,
    bits,
    left,
    right,
    low,
    high,
    size,
    increment
};

/// Names (and therefore symbols) are separated into a few different namespaces according to the spec. See �3.13.
/// Note that a bunch of the namespaces listed in the spec aren't really applicable to the lookup process;
/// for example, attribute names and macro names.
enum class LookupNamespace {
    /// Definitions encompass all non-nested modules, primitives, programs, and interfaces.
    Definitions,

    /// Namespace for all packages.
    Package,

    /// Namespace for members, which includes functions, tasks, parameters, variables, blocks, etc.
    Members
};

/// Base class for all symbols (logical code constructs) such as modules, types,
/// functions, variables, etc.
class Symbol {
public:
	/// The type of symbol.
    SymbolKind kind;

	/// The name of the symbol; if the symbol does not have a name,
	/// this will be an empty string.
    StringRef name;

	/// The declared location of the symbol in the source code, or an empty location
	/// if it was not explicitly declared in the source text. This is mainly used
	/// for reporting errors.
    SourceLocation location;

	/// The symbol that contains this symbol in the source text. All symbols have a containing
    /// symbol except for the design root, which has itself as the containing symbol. Keep that
    /// in mind when traversing the parent links.
	const Symbol& containingSymbol;

	/// Finds the first ancestor symbol of the given kind. If this symbol is already of
	/// the given kind, returns this symbol.
	const Symbol* findAncestor(SymbolKind searchKind) const;

	/// Gets the first containing parent symbol that is also a scope. If this is
    /// the design root, returns itself.
	const ScopeSymbol& containingScope() const;

	/// Gets the symbol for the root of the design.
	const DesignRootSymbol& getRoot() const;

	template<typename T>
	const T& as() const { return *static_cast<const T*>(this); }

protected:
    explicit Symbol(SymbolKind kind, const Symbol& containingSymbol, StringRef name = nullptr, SourceLocation location = SourceLocation()) :
        kind(kind), name(name), location(location),
		containingSymbol(containingSymbol) {}

	Symbol(SymbolKind kind, Token token, const Symbol& containingSymbol) :
		kind(kind), name(token.valueText()), location(token.location()),
		containingSymbol(containingSymbol) {}

	Diagnostic& addError(DiagCode code, SourceLocation location) const;

	template<typename T, typename... Args>
	T& allocate(Args&&... args) const {
		return getRoot().allocate<T>(std::forward<Args>(args)...);
	}
};

/// Base class for symbols that also act as scopes, which means they contain
/// child symbols that can be looked up by name.
class ScopeSymbol : public Symbol {
public:
	/// Look up a symbol in the current scope. Returns null if no symbol is found.
	const Symbol* lookup(StringRef name, LookupNamespace ns = LookupNamespace::Members) const;

	/// Look up a symbol in the current scope, expecting it to exist and be of the
	/// given type. If those conditions do not hold, this will assert.
	template<typename T>
	const T& lookup(StringRef name) const {
		const Symbol* sym = lookup(name);
		ASSERT(sym);
		return sym->as<T>();
	}

	/// A helper method to evaluate a constant in the current scope.
	ConstantValue evaluateConstant(const ExpressionSyntax& expr) const;

	/// A helper method to evaluate a constant in the current scope and then
	/// convert it to the given destination type. If the conversion fails, the
	/// returned value will be marked bad.
	ConstantValue evaluateConstantAndConvert(const ExpressionSyntax& expr, const TypeSymbol& targetType, SourceLocation errorLocation) const;

	/// A helper method to get a type symbol, using the current scope as context.
	const TypeSymbol& getType(const DataTypeSyntax& syntax) const;

protected:
	using Symbol::Symbol;
    
    // Adds a symbol to the scope. This is const because child classes will call this during
    // lazy initialization. It's up to them to not abuse this and maintain logical constness.
    void addSymbol(const Symbol& symbol) const;

private:
    // For now, there is one hash table here for the normal members namespace. The other
    // namespaces are specific to certain symbol types so I don't want to have extra overhead
    // on every kind of scope symbol.
    mutable std::unordered_map<StringRef, const Symbol*> memberMap;
};

/// Base class for all data types.
class TypeSymbol : public Symbol {
public:
	TypeSymbol(SymbolKind kind, StringRef name, const Symbol& parent) : Symbol(kind, parent, name) {}

    // SystemVerilog defines various levels of type compatibility, which are used
    // in different scenarios. See the spec, section 6.22.
    bool isMatching(const TypeSymbol& rhs) const;
    bool isEquivalent(const TypeSymbol& rhs) const;
    bool isAssignmentCompatible(const TypeSymbol& rhs) const;
    bool isCastCompatible(const TypeSymbol& rhs) const;

    // Helpers to get the following pieces of information for any type symbol,
    // though the information is stored differently for different types
    bool isSigned() const;
    bool isReal() const;
    bool isFourState() const;
    int width() const;

    std::string toString() const;

protected:
	using Symbol::Symbol;
};

class IntegralTypeSymbol : public TypeSymbol {
public:
    // a negative lower bound is actually an upper bound specified in the opposite order
    ArrayRef<int> lowerBounds;
    ArrayRef<int> widths;
    int width;
    TokenKind keywordType;
    bool isSigned;
    bool isFourState;

    IntegralTypeSymbol(TokenKind keywordType, int width, bool isSigned, bool isFourState, const Symbol& parent) :
        IntegralTypeSymbol( keywordType, width, isSigned, isFourState, EmptyLowerBound, ArrayRef<int>(&width, 1), parent) {}

    IntegralTypeSymbol(TokenKind keywordType, int width, bool isSigned, bool isFourState, ArrayRef<int> lowerBounds, ArrayRef<int> widths, const Symbol& parent) :
        TypeSymbol(SymbolKind::IntegralType, parent, getTokenKindText(keywordType), SourceLocation()),
        lowerBounds(lowerBounds), widths(widths),
        width(width), keywordType(keywordType), isSigned(isSigned), isFourState(isFourState) {}

private:
    static ArrayRef<int> EmptyLowerBound;
};

class RealTypeSymbol : public TypeSymbol {
public:
    int width;
    TokenKind keywordType;

    RealTypeSymbol(TokenKind keywordType, int width, const Symbol& parent) :
        TypeSymbol(SymbolKind::RealType, parent, getTokenKindText(keywordType), SourceLocation()),
        width(width), keywordType(keywordType) {}
};

//class EnumValueSymbol : public ConstValueSymbol {
//public:
//    EnumValueSymbol(StringRef name, SourceLocation location, const TypeSymbol *type, ConstantValue val) :
//        ConstValueSymbol(SymbolKind::EnumValue, name, location, type, val) {}
//};
//
//class EnumTypeSymbol : public TypeSymbol {
//public:
//    const IntegralTypeSymbol* baseType;
//    ArrayRef<EnumValueSymbol *> values;
//
//    EnumTypeSymbol(const IntegralTypeSymbol *baseType, SourceLocation location, ArrayRef<EnumValueSymbol *> values) :
//        TypeSymbol(SymbolKind::EnumType, "", location),
//        baseType(baseType), values(values) {}
//};

class StructTypeSymbol : public TypeSymbol {
public:
    bool isPacked;
    bool isSigned;
};

/// An empty type symbol that indicates an error occurred while trying to
/// resolve the type of some expression or declaration.
class ErrorTypeSymbol : public TypeSymbol {
public:
    explicit ErrorTypeSymbol(const Symbol& parent) : TypeSymbol(SymbolKind::Unknown, parent) {}
};

class TypeAliasSymbol : public TypeSymbol {
public:
    const SyntaxNode& syntax;
    const TypeSymbol* underlying;

    TypeAliasSymbol(const SyntaxNode& syntax, SourceLocation location, const TypeSymbol* underlying, StringRef alias, const Symbol& parent) :
        TypeSymbol(SymbolKind::TypeAlias, parent, alias, location),
        syntax(syntax), underlying(underlying) {}
};

class CompilationUnitSymbol;
class PackageSymbol;
class ParameterizedModuleSymbol;
class ModuleInstanceSymbol;

/// Represents the entirety of a design, along with all contained compilation units.
/// It also contains most of the machinery for creating and retrieving type symbols.
class DesignRootSymbol : public ScopeSymbol {
public:
	explicit DesignRootSymbol(const SyntaxTree& tree);
	explicit DesignRootSymbol(ArrayRef<const SyntaxTree*> syntaxTrees = nullptr);

	/// Adds a syntax tree to the design.
	void addTree(const SyntaxTree& tree);
	void addTrees(ArrayRef<const SyntaxTree*> syntaxTrees);

	/// Adds a precreated symbol to the root scope.
	void addSymbol(const Symbol& symbol);

	/// Gets all of the compilation units in the design.
	ArrayRef<const CompilationUnitSymbol*> units() const { return unitList; }

	/// Gets all of the top-level module instances in the design.
	/// These form the roots of the actual design hierarchy.
	ArrayRef<const ModuleInstanceSymbol*> tops() const { return topList; }

	/// Finds a package in the design with the given name, or returns null if none is found.
	const PackageSymbol* findPackage(StringRef name) const;

	/// Finds a module, interface, or program with the given name, or returns null if none is found.
	const Symbol* findDefinition(StringRef name) const;

	/// Methods for getting various type symbols.
	const TypeSymbol& getType(const DataTypeSyntax& syntax) const;
	const TypeSymbol& getType(const DataTypeSyntax& syntax, const ScopeSymbol& scope) const;
	const TypeSymbol& getKnownType(SyntaxKind kind) const;
	const TypeSymbol& getIntegralType(int width, bool isSigned, bool isFourState = true, bool isReg = false) const;
    const TypeSymbol& getErrorType() const;

	/// Report an error at the specified location.
	Diagnostic& addError(DiagCode code, SourceLocation location) const {
		return diags.add(code, location);
	}

	/// Allocate an object using the design's shared bump allocator.
	template<typename T, typename... Args>
	T& allocate(Args&&... args) const {
		return *alloc.emplace<T>(std::forward<Args>(args)...);
	}

	BumpAllocator& allocator() const { return alloc; }
	Diagnostics& diagnostics() const { return diags; }

private:
	// Gets a type symbol for the given integer type syntax node.
	const TypeSymbol& getIntegralType(const IntegerTypeSyntax& syntax, const ScopeSymbol& scope) const;

	// Evalutes variable dimensions that are expected to be compile-time constant.
	// Returns true if evaluation was successful; returns false and reports errors if not.
	bool evaluateConstantDims(const SyntaxList<VariableDimensionSyntax>& dimensions, SmallVector<ConstantRange>& results, const ScopeSymbol& scope) const;

	// Tries to convert a ConstantValue to a simple integer. Sets bad to true if the conversion fails.
	int coerceInteger(const ConstantValue& cv, int maxRangeBits, bool& bad) const;

	// Constructs symbols for the given syntax node. A single node might expand to more than one symbol;
	// for example, a variable declaration that has multiple declarators.
	void createSymbols(const SyntaxNode& node, SmallVector<const Symbol*>& results);

	// top level scope maps, list of roots, list of compilation units
    SymbolMap packageMap;
    SymbolMap definitionsMap;
	std::vector<const CompilationUnitSymbol*> unitList;
	std::vector<const ModuleInstanceSymbol*> topList;

	// preallocated type symbols for known types
	std::unordered_map<SyntaxKind, const TypeSymbol*> knownTypes;

	// These are mutable so that the design root can be logically const, observing
	// members lazily but allocating them on demand and reporting errors when asked.
	mutable BumpAllocator alloc;
	mutable Diagnostics diags;

	// cache of simple integral types keyed by {width, signedness, 4-state, isReg}
	mutable std::unordered_map<uint64_t, const TypeSymbol*> integralTypeCache;
};

/// The root of a single compilation unit. 
class CompilationUnitSymbol : public ScopeSymbol {
public:
	CompilationUnitSymbol(const CompilationUnitSyntax& syntax, const Symbol& parent);

	SymbolList members() const { return nullptr; }
};

/// A SystemVerilog package construct.
class PackageSymbol : public ScopeSymbol {
public:
	PackageSymbol(const ModuleDeclarationSyntax& package, const Symbol& parent);

	SymbolList members() const;
};

/// Represents a module declaration, with its parameters still unresolved.
class ModuleSymbol : public Symbol {
public:
	ModuleSymbol(const ModuleDeclarationSyntax& decl, const Symbol& container);

	/// Parameterizes the module with the given set of parameter assignments.
	const ParameterizedModuleSymbol& parameterize(const ParameterValueAssignmentSyntax* assignments = nullptr,
												  const ScopeSymbol* instanceScope = nullptr) const;

private:
	// Small collection of info extracted from a parameter definition
	struct ParameterInfo {
		const ParameterDeclarationSyntax& paramDecl;
		const VariableDeclaratorSyntax& declarator;
		StringRef name;
		SourceLocation location;
		ExpressionSyntax* initializer;
		bool local;
		bool bodyParam;
	};

	const std::vector<ParameterInfo>& getDeclaredParams() const;
	ConstantValue evaluate(const ParameterDeclarationSyntax& paramDecl, const ScopeSymbol& scope, const ExpressionSyntax& expr, SourceLocation declLocation) const;

	// Helper function used by getModuleParams to convert a single parameter declaration into
	// one or more ParameterInfo instances.
	bool getParamDecls(const ParameterDeclarationSyntax& syntax, std::vector<ParameterInfo>& buffer,
	                   HashMapBase<StringRef, SourceLocation>& nameDupMap,
	                   bool lastLocal, bool overrideLocal, bool bodyParam) const;

	const ModuleDeclarationSyntax& decl;
	mutable std::optional<std::vector<ParameterInfo>> paramInfoCache;
};

/// Represents a module that has had its parameters resolved to a specific set of values.
class ParameterizedModuleSymbol : public ScopeSymbol {
public:
	ParameterizedModuleSymbol(const ModuleSymbol& module, const Symbol& parent, const HashMapBase<StringRef, ConstantValue>& parameterAssignments);

	SymbolList members() const;
	
	template<typename T>
	const T& member(uint32_t index) const { return members()[index]->as<T>(); }

private:
	const ModuleSymbol& module;
};

class ModuleInstanceSymbol : public Symbol {
public:
	ParameterizedModuleSymbol& module;

	/// A helper method to access a specific member of the module (of which this is an instance).
	template<typename T>
	const T& member(uint32_t index) const { return module.members()[index]->as<T>(); }
};

//class GenvarSymbol : public Symbol {
//public:
//    GenvarSymbol(StringRef name, SourceLocation location, ) :
//        Symbol(SymbolKind::Genvar, nullptr, name, location) {}
//};

class ParameterSymbol : public Symbol {
public:
	ConstantValue value;
};

class GenerateBlockSymbol : public ScopeSymbol {
public:

	SymbolList members() const { return nullptr; }

	template<typename T>
	const T& member(uint32_t index) const { return members()[index]->as<T>(); }
};

class ProceduralBlockSymbol : public ScopeSymbol {
public:

	SymbolList members() const { return nullptr; }

	template<typename T>
	const T& member(uint32_t index) const { return members()[index]->as<T>(); }
};

/// Represents a variable declaration (which does not include nets).
class VariableSymbol : public Symbol {
public:
	VariableLifetime lifetime;
	bool isConst;

	VariableSymbol(Token name, const DataTypeSyntax& type, const Symbol& parent, VariableLifetime lifetime,
				   bool isConst, const ExpressionSyntax* initializer);

	VariableSymbol(StringRef name, SourceLocation location, const TypeSymbol& type, const Symbol& parent,
				   VariableLifetime lifetime = VariableLifetime::Automatic, bool isConst = false,
				   const BoundExpression* initializer = nullptr);

    const TypeSymbol& type() const;
    const BoundExpression* initializer() const;

protected:
	VariableSymbol(SymbolKind childKind, StringRef name, SourceLocation location, const TypeSymbol& type, const Symbol& parent,
				   VariableLifetime lifetime = VariableLifetime::Automatic, bool isConst = false,
				   const BoundExpression* initializer = nullptr);

private:
	// To allow lazy binding, save pointers to the raw syntax nodes. When we eventually bind,
	// we will fill in the type symbol and bound initializer. Also a user can fill in those
	// manually for synthetically constructed symbols.
	const DataTypeSyntax* typeSyntax = nullptr;
	const ExpressionSyntax* initializerSyntax = nullptr;
	mutable const TypeSymbol* typeSymbol = nullptr;
	mutable const BoundExpression* initializerBound = nullptr;
};

/// Represents a formal argument in subroutine (task or function).
class FormalArgumentSymbol : public VariableSymbol {
public:
    FormalArgumentDirection direction = FormalArgumentDirection::In;

	FormalArgumentSymbol(const TypeSymbol& type, const Symbol& parent);

	FormalArgumentSymbol(StringRef name, SourceLocation location, const TypeSymbol& type, const Symbol& parent,
						 const BoundExpression* initializer = nullptr,
						 FormalArgumentDirection direction = FormalArgumentDirection::In);
};

/// Represents a subroutine (task or function.
class SubroutineSymbol : public ScopeSymbol {
public:
	const FunctionDeclarationSyntax* syntax = nullptr;
	VariableLifetime defaultLifetime = VariableLifetime::Automatic;
	SystemFunction systemFunctionKind = SystemFunction::Unknown;
	bool isTask = false;

	SubroutineSymbol(const FunctionDeclarationSyntax& syntax, const Symbol& parent);
	SubroutineSymbol(StringRef name, const TypeSymbol& returnType, ArrayRef<const FormalArgumentSymbol*> arguments,
					 SystemFunction systemFunction, const Symbol& parent);

	const TypeSymbol& returnType() const { init(); return *returnType_; }
	const BoundStatementList& body() const { init(); return *body_; }
	ArrayRef<const FormalArgumentSymbol*> arguments() const { init(); return arguments_; }

	bool isSystemFunction() const { return systemFunctionKind != SystemFunction::Unknown; }

private:
	void init() const;

	mutable const TypeSymbol* returnType_ = nullptr;
	mutable const BoundStatementList* body_ = nullptr;
	mutable ArrayRef<const FormalArgumentSymbol*> arguments_;
	mutable bool initialized = false;
};

}
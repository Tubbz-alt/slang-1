//------------------------------------------------------------------------------
// MiscExpressions.cpp
// Definitions for miscellaneous expressions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/binding/MiscExpressions.h"

#include "slang/binding/SelectExpressions.h"
#include "slang/binding/SystemSubroutine.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/ConstEvalDiags.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/symbols/ASTSerializer.h"
#include "slang/symbols/ParameterSymbols.h"
#include "slang/symbols/SubroutineSymbols.h"
#include "slang/symbols/VariableSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/StackContainer.h"

namespace slang {

static std::pair<const Symbol*, bool> getParentClass(const Scope& scope) {
    // Find the class that is the source of the lookup.
    const Symbol* parent = &scope.asSymbol();
    bool inStatic = false;
    while (true) {
        if (parent->kind == SymbolKind::Subroutine) {
            // Remember whether this was a static class method.
            if (parent->as<SubroutineSymbol>().flags & MethodFlags::Static)
                inStatic = true;
        }
        else if (parent->kind == SymbolKind::ClassType) {
            // We found our parent class, so break out.
            return { parent, inStatic };
        }
        else if (parent->kind != SymbolKind::StatementBlock) {
            // We're not in a class, so there's nothing to check.
            // This is probably not actually reachable.
            return { nullptr, false };
        }

        auto parentScope = parent->getParentScope();
        ASSERT(parentScope);
        parent = &parentScope->asSymbol();
    }
}

// Returns true if the target symbol is accessible from the class scope given by `parentClass`.
static bool isAccessibleFrom(const Symbol& target, const Symbol& sourceScope) {
    auto& parentScope = target.getParentScope()->asSymbol();
    if (&sourceScope == &parentScope)
        return true;

    if (parentScope.kind != SymbolKind::ClassType)
        return false;

    auto& sourceType = sourceScope.as<Type>();
    auto& targetType = parentScope.as<Type>();
    return targetType.isAssignmentCompatible(sourceType);
}

Expression& ValueExpressionBase::fromSymbol(const BindContext& context, const Symbol& symbol,
                                            bool isHierarchical, SourceRange sourceRange) {
    Compilation& compilation = context.getCompilation();
    if (!symbol.isValue()) {
        context.addDiag(diag::NotAValue, sourceRange) << symbol.name;
        return badExpr(compilation, nullptr);
    }

    // Automatic variables have additional restrictions.
    if (VariableSymbol::isKind(symbol.kind) &&
        symbol.as<VariableSymbol>().lifetime == VariableLifetime::Automatic) {

        // If this is actually a class property, check that no static methods,
        // initializers, or nested classes are accessing it.
        if (symbol.kind == SymbolKind::ClassProperty) {
            auto [parent, inStatic] = getParentClass(context.scope);
            if (parent && !isAccessibleFrom(symbol, *parent)) {
                auto& diag = context.addDiag(diag::NestedNonStaticClassProperty, sourceRange);
                diag << symbol.name << parent->name;
                return badExpr(compilation, nullptr);
            }
            else if (!parent || inStatic || (context.flags & BindFlags::StaticInitializer) != 0) {
                context.addDiag(diag::NonStaticClassProperty, sourceRange) << symbol.name;
                return badExpr(compilation, nullptr);
            }
        }
        else if ((context.flags & BindFlags::StaticInitializer) != 0) {
            context.addDiag(diag::AutoFromStaticInit, sourceRange) << symbol.name;
            return badExpr(compilation, nullptr);
        }
    }

    auto& value = symbol.as<ValueSymbol>();
    if (isHierarchical)
        return *compilation.emplace<HierarchicalValueExpression>(value, sourceRange);
    else
        return *compilation.emplace<NamedValueExpression>(value, sourceRange);
}

bool ValueExpressionBase::verifyAssignableImpl(const BindContext& context, bool isNonBlocking,
                                               SourceLocation location) const {
    if (symbol.kind == SymbolKind::Parameter || symbol.kind == SymbolKind::EnumValue) {
        auto& diag = context.addDiag(diag::ExpressionNotAssignable, location);
        diag.addNote(diag::NoteDeclarationHere, symbol.location);
        diag << sourceRange;
        return false;
    }

    if (context.flags.has(BindFlags::ProceduralStatement)) {
        // Nets can't be assigned in procedural contexts.
        if (symbol.kind == SymbolKind::Net) {
            context.addDiag(diag::AssignToNet, sourceRange);
            return false;
        }
    }
    else {
        // chandles can only be assigned in procedural contexts.
        if (symbol.getType().isCHandle()) {
            context.addDiag(diag::AssignToCHandle, sourceRange);
            return false;
        }
    }

    if (VariableSymbol::isKind(symbol.kind)) {
        return context.requireAssignable(symbol.as<VariableSymbol>(), isNonBlocking, location,
                                         sourceRange);
    }

    return true;
}

optional<bitwidth_t> ValueExpressionBase::getEffectiveWidthImpl() const {
    auto cvToWidth = [this](const ConstantValue& cv) -> optional<bitwidth_t> {
        if (!cv.isInteger())
            return std::nullopt;

        auto& sv = cv.integer();
        if (sv.hasUnknown())
            return type->getBitWidth();

        if (sv.isNegative())
            return sv.getMinRepresentedBits();

        return sv.getActiveBits();
    };

    switch (symbol.kind) {
        case SymbolKind::Parameter:
            return cvToWidth(symbol.as<ParameterSymbol>().getValue());
        case SymbolKind::EnumValue:
            return cvToWidth(symbol.as<EnumValueSymbol>().getValue());
        default:
            return type->getBitWidth();
    }
}

void ValueExpressionBase::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("symbol", symbol);
}

ConstantValue NamedValueExpression::evalImpl(EvalContext& context) const {
    if (!verifyConstantImpl(context))
        return nullptr;

    switch (symbol.kind) {
        case SymbolKind::Parameter:
            return symbol.as<ParameterSymbol>().getValue();
        case SymbolKind::EnumValue:
            return symbol.as<EnumValueSymbol>().getValue();
        default:
            ConstantValue* v = context.findLocal(&symbol);
            if (v)
                return *v;
            break;
    }

    // If we reach this point, the variable was not found, which should mean that
    // it's not actually constant.
    auto& diag = context.addDiag(diag::ConstEvalNonConstVariable, sourceRange) << symbol.name;
    diag.addNote(diag::NoteDeclarationHere, symbol.location);
    return nullptr;
}

LValue NamedValueExpression::evalLValueImpl(EvalContext& context) const {
    if (!verifyConstantImpl(context))
        return nullptr;

    auto cv = context.findLocal(&symbol);
    if (!cv) {
        auto& diag = context.addDiag(diag::ConstEvalNonConstVariable, sourceRange) << symbol.name;
        diag.addNote(diag::NoteDeclarationHere, symbol.location);
        return nullptr;
    }

    return LValue(*cv);
}

bool NamedValueExpression::verifyConstantImpl(EvalContext& context) const {
    if (context.isScriptEval())
        return true;

    // Class types are disallowed in constant expressions. Note that I don't see anything
    // in the spec that would explicitly disallow them, but literally every tool issues
    // an error so for now we will follow suit.
    if (type->isClass()) {
        context.addDiag(diag::ConstEvalClassType, sourceRange);
        return false;
    }

    const EvalContext::Frame& frame = context.topFrame();
    const SubroutineSymbol* subroutine = frame.subroutine;
    if (!subroutine)
        return true;

    // Constant functions have a bunch of additional restrictions. See [13.4.4]:
    // - All parameter values used within the function shall be defined before the use of
    //   the invoking constant function call.
    // - All identifiers that are not parameters or functions shall be declared locally to
    //   the current function.
    if (symbol.kind != SymbolKind::Parameter && symbol.kind != SymbolKind::EnumValue) {
        const Scope* scope = symbol.getParentScope();
        while (scope && scope != subroutine)
            scope = scope->asSymbol().getParentScope();

        if (scope != subroutine) {
            auto& diag =
                context.addDiag(diag::ConstEvalFunctionIdentifiersMustBeLocal, sourceRange);
            diag.addNote(diag::NoteDeclarationHere, symbol.location);
            return false;
        }
    }
    else {
        // If the two locations are not in the same compilation unit, assume that it's ok.
        auto compare = symbol.isDeclaredBefore(frame.lookupLocation);
        if (!compare.value_or(true)) {
            auto& diag = context.addDiag(diag::ConstEvalIdUsedInCEBeforeDecl, sourceRange)
                         << symbol.name;
            diag.addNote(diag::NoteDeclarationHere, symbol.location);
            return false;
        }
    }

    return true;
}

ConstantValue HierarchicalValueExpression::evalImpl(EvalContext&) const {
    return nullptr;
}

bool HierarchicalValueExpression::verifyConstantImpl(EvalContext& context) const {
    context.addDiag(diag::ConstEvalHierarchicalNameInCE, sourceRange) << symbol.name;
    return false;
}

Expression& CallExpression::fromSyntax(Compilation& compilation,
                                       const InvocationExpressionSyntax& syntax,
                                       const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                       const BindContext& context) {
    return fromSyntaxImpl(compilation, *syntax.left, &syntax, withClause, context);
}

Expression& CallExpression::fromSyntax(Compilation& compilation,
                                       const ArrayOrRandomizeMethodExpressionSyntax& syntax,
                                       const BindContext& context) {
    if (syntax.method->kind == SyntaxKind::InvocationExpression) {
        auto& invoc = syntax.method->as<InvocationExpressionSyntax>();
        return fromSyntax(compilation, invoc, &syntax, context);
    }

    return fromSyntaxImpl(compilation, *syntax.method, nullptr, &syntax, context);
}

Expression& CallExpression::fromSyntaxImpl(Compilation& compilation, const ExpressionSyntax& left,
                                           const InvocationExpressionSyntax* invocation,
                                           const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                           const BindContext& context) {
    if (left.kind == SyntaxKind::MemberAccessExpression) {
        return MemberAccessExpression::fromSyntax(
            compilation, left.as<MemberAccessExpressionSyntax>(), invocation, withClause, context);
    }

    if (!NameSyntax::isKind(left.kind)) {
        SourceLocation loc = (invocation && invocation->arguments)
                                 ? invocation->arguments->openParen.location()
                                 : left.getFirstToken().location();
        auto& diag = context.addDiag(diag::ExpressionNotCallable, loc);
        diag << left.sourceRange();
        return badExpr(compilation, nullptr);
    }

    return bindName(compilation, left.as<NameSyntax>(), invocation, withClause, context);
}

Expression& CallExpression::fromLookup(Compilation& compilation, const Subroutine& subroutine,
                                       const Expression* thisClass,
                                       const InvocationExpressionSyntax* syntax,
                                       const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                       SourceRange range, const BindContext& context) {
    if (subroutine.index() == 1) {
        const SystemCallInfo& info = std::get<1>(subroutine);
        return createSystemCall(compilation, *info.subroutine, nullptr, syntax, withClause, range,
                                context);
    }

    // If this is a non-static class method make sure we're allowed to call it.
    // If we're being called through a class handle (thisClass is non-null) that's fine,
    // otherwise we need to be called by a non-static member within the same class.
    auto sub = std::get<0>(subroutine);
    ASSERT(sub->getParentScope());
    auto& subroutineParent = sub->getParentScope()->asSymbol();
    if ((sub->flags & MethodFlags::Static) == 0 && !thisClass &&
        subroutineParent.kind == SymbolKind::ClassType) {

        auto [parent, inStatic] = getParentClass(context.scope);
        if (parent && !isAccessibleFrom(*sub, *parent)) {
            auto& diag = context.addDiag(diag::NestedNonStaticClassMethod, range);
            diag << parent->name;
            return badExpr(compilation, nullptr);
        }
        else if (!parent || inStatic || (context.flags & BindFlags::StaticInitializer) != 0) {
            context.addDiag(diag::NonStaticClassMethod, range);
            return badExpr(compilation, nullptr);
        }
    }

    if (withClause) {
        context.addDiag(diag::WithClauseNotAllowed, withClause->with.range()) << sub->name;
        return badExpr(compilation, nullptr);
    }

    // Can only omit the parentheses for invocation if the subroutine is a task,
    // void function, or class method.
    if (!syntax && subroutineParent.kind != SymbolKind::ClassType) {
        if (!sub->getReturnType().isVoid()) {
            context.addDiag(diag::MissingInvocationParens, range) << sub->name;
            return badExpr(compilation, nullptr);
        }
    }

    auto& result = fromArgs(compilation, subroutine, thisClass,
                            syntax ? syntax->arguments : nullptr, range, context);
    if (syntax)
        context.setAttributes(result, syntax->attributes);

    return result;
}

Expression& CallExpression::fromArgs(Compilation& compilation, const Subroutine& subroutine,
                                     const Expression* thisClass,
                                     const ArgumentListSyntax* argSyntax, SourceRange range,
                                     const BindContext& context) {
    // Collect all arguments into a list of ordered expressions (which can
    // optionally be nullptr to indicate an empty argument) and a map of
    // named argument assignments.
    SmallVectorSized<const SyntaxNode*, 8> orderedArgs;
    SmallMap<string_view, std::pair<const NamedArgumentSyntax*, bool>, 8> namedArgs;

    if (argSyntax) {
        for (auto arg : argSyntax->parameters) {
            if (arg->kind == SyntaxKind::NamedArgument) {
                const NamedArgumentSyntax& nas = arg->as<NamedArgumentSyntax>();
                auto name = nas.name.valueText();
                if (!name.empty()) {
                    auto pair = namedArgs.emplace(name, std::make_pair(&nas, false));
                    if (!pair.second) {
                        auto& diag =
                            context.addDiag(diag::DuplicateArgAssignment, nas.name.location());
                        diag << name;
                        diag.addNote(diag::NotePreviousUsage,
                                     pair.first->second.first->name.location());
                    }
                }
            }
            else {
                // Once a named argument has been seen, no more ordered arguments are allowed.
                if (!namedArgs.empty()) {
                    context.addDiag(diag::MixingOrderedAndNamedArgs,
                                    arg->getFirstToken().location());
                    return badExpr(compilation, nullptr);
                }

                if (arg->kind == SyntaxKind::EmptyArgument)
                    orderedArgs.append(arg);
                else
                    orderedArgs.append(arg->as<OrderedArgumentSyntax>().expr);
            }
        }
    }

    // Now bind all arguments.
    bool bad = false;
    uint32_t orderedIndex = 0;
    SmallVectorSized<const Expression*, 8> boundArgs;
    const SubroutineSymbol& symbol = *std::get<0>(subroutine);

    for (auto formal : symbol.getArguments()) {
        const Expression* expr = nullptr;
        if (orderedIndex < orderedArgs.size()) {
            auto arg = orderedArgs[orderedIndex++];
            if (arg->kind == SyntaxKind::EmptyArgument) {
                // Empty arguments are allowed as long as a default is provided.
                expr = formal->getInitializer();
                if (!expr)
                    context.addDiag(diag::ArgCannotBeEmpty, arg->sourceRange()) << formal->name;
            }
            else {
                expr = &Expression::bindArgument(formal->getType(), formal->direction,
                                                 arg->as<ExpressionSyntax>(), context,
                                                 formal->isConstant);
            }

            // Make sure there isn't also a named value for this argument.
            if (auto it = namedArgs.find(formal->name); it != namedArgs.end()) {
                auto& diag = context.addDiag(diag::DuplicateArgAssignment,
                                             it->second.first->name.location());
                diag << formal->name;
                diag.addNote(diag::NotePreviousUsage, arg->getFirstToken().location());
                it->second.second = true;
                bad = true;
            }
        }
        else if (auto it = namedArgs.find(formal->name); it != namedArgs.end()) {
            // Mark this argument as used so that we can later detect if
            // any were unused.
            it->second.second = true;

            auto arg = it->second.first->expr;
            if (!arg) {
                // Empty arguments are allowed as long as a default is provided.
                expr = formal->getInitializer();
                if (!expr) {
                    context.addDiag(diag::ArgCannotBeEmpty, it->second.first->sourceRange())
                        << formal->name;
                }
            }
            else {
                expr = &Expression::bindArgument(formal->getType(), formal->direction, *arg,
                                                 context, formal->isConstant);
            }
        }
        else {
            expr = formal->getInitializer();
            if (!expr) {
                if (namedArgs.empty()) {
                    auto& diag = context.addDiag(diag::TooFewArguments, range);
                    diag << symbol.getArguments().size() << orderedArgs.size();
                    bad = true;
                    break;
                }
                else {
                    context.addDiag(diag::UnconnectedArg, range) << formal->name;
                }
            }
        }

        if (!expr) {
            bad = true;
        }
        else {
            bad |= expr->bad();
            boundArgs.append(expr);
        }
    }

    // Make sure there weren't too many ordered arguments provided.
    if (orderedIndex < orderedArgs.size()) {
        auto& diag = context.addDiag(diag::TooManyArguments, range);
        diag << symbol.getArguments().size();
        diag << orderedArgs.size();
        bad = true;
    }

    for (const auto& pair : namedArgs) {
        // We marked all the args that we used, so anything left over is an arg assignment
        // for a non-existent arg.
        if (!pair.second.second) {
            auto& diag = context.addDiag(diag::ArgDoesNotExist, pair.second.first->name.location());
            diag << pair.second.first->name.valueText();
            diag << symbol.name;
            bad = true;
        }
    }

    auto result = compilation.emplace<CallExpression>(&symbol, symbol.getReturnType(), thisClass,
                                                      boundArgs.copy(compilation),
                                                      context.lookupLocation, range);
    if (bad)
        return badExpr(compilation, result);

    return *result;
}

Expression& CallExpression::fromSystemMethod(
    Compilation& compilation, const Expression& expr, const LookupResult::MemberSelector& selector,
    const InvocationExpressionSyntax* syntax,
    const ArrayOrRandomizeMethodExpressionSyntax* withClause, const BindContext& context) {

    const Type& type = expr.type->getCanonicalType();
    auto subroutine = compilation.getSystemMethod(type.kind, selector.name);
    if (!subroutine) {
        if (syntax) {
            context.addDiag(diag::UnknownSystemMethod, selector.nameRange)
                << selector.name << *expr.type;
        }
        else {
            auto& diag = context.addDiag(diag::InvalidMemberAccess, selector.dotLocation);
            diag << expr.sourceRange;
            diag << selector.nameRange;
            diag << *expr.type;
        }
        return badExpr(compilation, &expr);
    }

    return createSystemCall(compilation, *subroutine, &expr, syntax, withClause,
                            syntax ? syntax->sourceRange() : expr.sourceRange, context);
}

Expression* CallExpression::fromBuiltInMethod(
    Compilation& compilation, SymbolKind rootKind, const Expression& expr,
    const LookupResult::MemberSelector& selector, const InvocationExpressionSyntax* syntax,
    const ArrayOrRandomizeMethodExpressionSyntax* withClause, const BindContext& context) {

    auto subroutine = compilation.getSystemMethod(rootKind, selector.name);
    if (!subroutine)
        return nullptr;

    return &createSystemCall(compilation, *subroutine, &expr, syntax, withClause,
                             syntax ? syntax->sourceRange() : expr.sourceRange, context);
}

static const Expression* bindIteratorExpr(Compilation& compilation,
                                          const InvocationExpressionSyntax* invocation,
                                          const ArrayOrRandomizeMethodExpressionSyntax& withClause,
                                          const Type& arrayType, const BindContext& context,
                                          const ValueSymbol*& iterVar) {
    // Can't be a constraint block here.
    if (withClause.constraints) {
        context.addDiag(diag::UnexpectedConstraintBlock, withClause.constraints->sourceRange());
        return nullptr;
    }

    if (!withClause.args) {
        context.addDiag(diag::ExpectedIterationExpression, withClause.with.range());
        return nullptr;
    }

    if (withClause.args->expressions.size() != 1) {
        context.addDiag(diag::ExpectedIterationExpression, withClause.args->sourceRange());
        return nullptr;
    }

    // If arguments are provided, there should be only one and it should
    // be the name of the iterator symbol. Otherwise, we need to automatically
    // generate an iterator symbol named 'item'.
    SourceLocation iteratorLoc = SourceLocation::NoLocation;
    string_view iteratorName;
    if (invocation && invocation->arguments) {
        auto actualArgs = invocation->arguments->parameters;
        if (actualArgs.size() == 1 && actualArgs[0]->kind == SyntaxKind::OrderedArgument) {
            auto& arg = actualArgs[0]->as<OrderedArgumentSyntax>();
            if (arg.expr->kind == SyntaxKind::IdentifierName) {
                auto id = arg.expr->as<IdentifierNameSyntax>().identifier;
                iteratorLoc = id.location();
                iteratorName = id.valueText();
                if (iteratorName.empty())
                    return nullptr;
            }
        }

        if (iteratorName.empty() && !actualArgs.empty()) {
            context.addDiag(diag::ExpectedIteratorName, invocation->arguments->sourceRange());
            return nullptr;
        }
    }

    if (iteratorName.empty())
        iteratorName = "item"sv;

    // Create the iterator variable and set it up with a bind context so that it
    // can be found by the iteration expression.
    auto it =
        compilation.emplace<IteratorSymbol>(context.scope, iteratorName, iteratorLoc, arrayType);
    iterVar = it;

    BindContext iterCtx = context;
    it->nextIterator = std::exchange(iterCtx.firstIterator, it);
    iterCtx.flags &= ~BindFlags::StaticInitializer;

    return &Expression::bind(*withClause.args->expressions[0], iterCtx);
}

Expression& CallExpression::createSystemCall(
    Compilation& compilation, const SystemSubroutine& subroutine, const Expression* firstArg,
    const InvocationExpressionSyntax* syntax,
    const ArrayOrRandomizeMethodExpressionSyntax* withClause, SourceRange range,
    const BindContext& context) {

    SmallVectorSized<const Expression*, 8> buffer;
    if (firstArg)
        buffer.append(firstArg);

    const Expression* iterExpr = nullptr;
    const ValueSymbol* iterVar = nullptr;
    using WithClauseMode = SystemSubroutine::WithClauseMode;
    if (subroutine.withClauseMode == WithClauseMode::Iterator) {
        // 'with' clause is not required. If it's not there, no arguments
        // can be provided.
        if (!withClause) {
            if (syntax && syntax->arguments && !syntax->arguments->parameters.empty()) {
                context.addDiag(diag::IteratorArgsWithoutWithClause,
                                syntax->arguments->sourceRange())
                    << subroutine.name;
                return badExpr(compilation, nullptr);
            }
        }
        else if (firstArg) {
            iterExpr = bindIteratorExpr(compilation, syntax, *withClause, *firstArg->type, context,
                                        iterVar);
            if (!iterExpr || iterExpr->bad())
                return badExpr(compilation, iterExpr);
        }
    }
    else if (subroutine.withClauseMode == WithClauseMode::Randomize) {
        // TODO: support randomize calls
    }
    else {
        if (withClause) {
            context.addDiag(diag::WithClauseNotAllowed, withClause->with.range())
                << subroutine.name;
            return badExpr(compilation, nullptr);
        }

        // Bind arguments as we would for any ordinary method.
        if (syntax && syntax->arguments) {
            auto actualArgs = syntax->arguments->parameters;
            for (size_t i = 0; i < actualArgs.size(); i++) {
                size_t index = i + (firstArg ? 1 : 0);
                switch (actualArgs[i]->kind) {
                    case SyntaxKind::OrderedArgument: {
                        const auto& arg = actualArgs[i]->as<OrderedArgumentSyntax>();
                        buffer.append(&subroutine.bindArgument(index, context, *arg.expr, buffer));
                        break;
                    }
                    case SyntaxKind::NamedArgument:
                        context.addDiag(diag::NamedArgNotAllowed, actualArgs[i]->sourceRange());
                        return badExpr(compilation, nullptr);
                    case SyntaxKind::EmptyArgument:
                        if (subroutine.allowEmptyArgument(index)) {
                            buffer.append(compilation.emplace<EmptyArgumentExpression>(
                                compilation.getVoidType(), actualArgs[i]->sourceRange()));
                        }
                        else {
                            context.addDiag(diag::EmptyArgNotAllowed, actualArgs[i]->sourceRange());
                            return badExpr(compilation, nullptr);
                        }
                        break;
                    default:
                        THROW_UNREACHABLE;
                }
            }
        }
    }

    SystemCallInfo callInfo{ &subroutine, &context.scope, iterExpr, iterVar };
    const Type& type = subroutine.checkArguments(context, buffer, range, iterExpr);
    auto expr = compilation.emplace<CallExpression>(
        callInfo, type, nullptr, buffer.copy(compilation), context.lookupLocation, range);

    if (type.isError())
        return badExpr(compilation, expr);

    for (auto arg : expr->arguments()) {
        if (arg->bad())
            return badExpr(compilation, expr);
    }

    if (syntax)
        context.setAttributes(*expr, syntax->attributes);

    return *expr;
}

ConstantValue CallExpression::evalImpl(EvalContext& context) const {
    // Delegate system calls to their appropriate handler.
    if (isSystemCall()) {
        auto& callInfo = std::get<1>(subroutine);
        return callInfo.subroutine->eval(context, arguments(), callInfo);
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    if (!checkConstant(context, symbol, sourceRange))
        return nullptr;

    // If thisClass() is set, we will already have issued an error when
    // verifying constant-ness. Just fail silently here.
    if (thisClass())
        return nullptr;

    // Evaluate all argument in the current stack frame.
    SmallVectorSized<ConstantValue, 8> args;
    for (auto arg : arguments()) {
        ConstantValue v = arg->eval(context);
        if (!v)
            return nullptr;
        args.emplace(std::move(v));
    }

    // Push a new stack frame, push argument values as locals.
    if (!context.pushFrame(symbol, sourceRange.start(), lookupLocation))
        return nullptr;

    span<const FormalArgumentSymbol* const> formals = symbol.getArguments();
    for (size_t i = 0; i < formals.size(); i++)
        context.createLocal(formals[i], args[i]);

    ASSERT(symbol.returnValVar);
    context.createLocal(symbol.returnValVar);

    using ER = Statement::EvalResult;
    ER er = symbol.getBody(&context).eval(context);

    // If we got a disable result, it means a disable statement was evaluated that
    // targeted a block that wasn't executing. This isn't allowed in a constant expression.
    // Handle this before popping the frame so that we get the stack reported.
    if (er == ER::Disable)
        context.addDiag(diag::ConstEvalDisableTarget, context.getDisableRange());

    ConstantValue result = std::move(*context.findLocal(symbol.returnValVar));
    context.popFrame();

    if (er == ER::Fail || er == ER::Disable)
        return nullptr;

    ASSERT(er == ER::Success || er == ER::Return);
    return result;
}

bool CallExpression::verifyConstantImpl(EvalContext& context) const {
    if (thisClass() && !thisClass()->verifyConstant(context))
        return false;

    for (auto arg : arguments()) {
        if (!arg->verifyConstant(context))
            return false;
    }

    if (isSystemCall()) {
        auto& callInfo = std::get<1>(subroutine);
        if (callInfo.iterExpr && !callInfo.iterExpr->verifyConstant(context))
            return false;

        return callInfo.subroutine->verifyConstant(context, arguments(), sourceRange);
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    if (!checkConstant(context, symbol, sourceRange))
        return false;

    // Recursive function calls check body only once
    // otherwise never finish until exceeding depth limit.
    if (inRecursion)
        return true;

    inRecursion = true;
    auto guard = ScopeGuard([this] { inRecursion = false; });

    if (!context.pushFrame(symbol, sourceRange.start(), lookupLocation))
        return false;

    bool result = symbol.getBody(&context).verifyConstant(context);
    context.popFrame();
    return result;
}

bool CallExpression::checkConstant(EvalContext& context, const SubroutineSymbol& subroutine,
                                   SourceRange range) {
    if (context.isScriptEval())
        return true;

    if (subroutine.subroutineKind == SubroutineKind::Task) {
        context.addDiag(diag::ConstEvalTaskNotConstant, range);
        return false;
    }

    if (subroutine.flags.has(MethodFlags::DPIImport)) {
        context.addDiag(diag::ConstEvalDPINotConstant, range);
        return false;
    }

    if (subroutine.flags.has(MethodFlags::Virtual | MethodFlags::Pure | MethodFlags::Constructor)) {
        context.addDiag(diag::ConstEvalMethodNotConstant, range);
        return false;
    }

    if (subroutine.flags.has(MethodFlags::NotConst)) {
        context.addDiag(diag::ConstEvalSubroutineNotConstant, range) << subroutine.name;
        return false;
    }

    if (subroutine.getReturnType().isVoid()) {
        context.addDiag(diag::ConstEvalVoidNotConstant, range);
        return false;
    }

    for (auto arg : subroutine.getArguments()) {
        if (arg->direction != ArgumentDirection::In) {
            context.addDiag(diag::ConstEvalFunctionArgDirection, range);
            return false;
        }
    }

    auto scope = subroutine.getParentScope();
    ASSERT(scope);
    if (scope->asSymbol().kind == SymbolKind::GenerateBlock) {
        context.addDiag(diag::ConstEvalFunctionInsideGenerate, range);
        return false;
    }

    return true;
}

string_view CallExpression::getSubroutineName() const {
    if (subroutine.index() == 1) {
        auto& callInfo = std::get<1>(subroutine);
        return callInfo.subroutine->name;
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    return symbol.name;
}

SubroutineKind CallExpression::getSubroutineKind() const {
    if (subroutine.index() == 1) {
        auto& callInfo = std::get<1>(subroutine);
        return callInfo.subroutine->kind;
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    return symbol.subroutineKind;
}

void CallExpression::serializeTo(ASTSerializer& serializer) const {
    if (subroutine.index() == 1) {
        auto& callInfo = std::get<1>(subroutine);
        serializer.write("subroutine", callInfo.subroutine->name);
    }
    else {
        const SubroutineSymbol& symbol = *std::get<0>(subroutine);
        serializer.writeLink("subroutine", symbol);
    }

    if (thisClass())
        serializer.write("thisClass", *thisClass());

    if (!arguments().empty()) {
        serializer.startArray("arguments");
        for (auto arg : arguments())
            serializer.serialize(*arg);
        serializer.endArray();
    }
}

Expression& DataTypeExpression::fromSyntax(Compilation& compilation, const DataTypeSyntax& syntax,
                                           const BindContext& context) {
    if ((context.flags & BindFlags::AllowDataType) == 0) {
        context.addDiag(diag::ExpectedExpression, syntax.sourceRange());
        return badExpr(compilation, nullptr);
    }

    const Type& type = compilation.getType(syntax, context.lookupLocation, context.scope);
    return *compilation.emplace<DataTypeExpression>(type, syntax.sourceRange());
}

Expression& HierarchicalReferenceExpression::fromSyntax(Compilation& compilation,
                                                        const NameSyntax& syntax,
                                                        const BindContext& context) {
    LookupResult result;
    Lookup::name(context.scope, syntax, context.lookupLocation, LookupFlags::AllowDeclaredAfter,
                 result);

    if (result.hasError())
        compilation.addDiagnostics(result.getDiagnostics());

    const Symbol* symbol = result.found;
    if (!symbol)
        return badExpr(compilation, nullptr);

    return *compilation.emplace<HierarchicalReferenceExpression>(*symbol, compilation.getVoidType(),
                                                                 syntax.sourceRange());
}

void HierarchicalReferenceExpression::serializeTo(ASTSerializer& serializer) const {
    if (symbol)
        serializer.writeLink("symbol", *symbol);
}

ConstantValue LValueReferenceExpression::evalImpl(EvalContext& context) const {
    auto lvalue = context.getTopLValue();
    if (!lvalue)
        return nullptr;

    return lvalue->load();
}

Expression& MinTypMaxExpression::fromSyntax(Compilation& compilation,
                                            const MinTypMaxExpressionSyntax& syntax,
                                            const BindContext& context,
                                            const Type* assignmentTarget) {
    // Only one of the expressions will be considered evaluated.
    auto minFlags = BindFlags::UnevaluatedBranch;
    auto typFlags = BindFlags::UnevaluatedBranch;
    auto maxFlags = BindFlags::UnevaluatedBranch;
    switch (compilation.getOptions().minTypMax) {
        case MinTypMax::Min:
            minFlags = BindFlags::None;
            break;
        case MinTypMax::Typ:
            typFlags = BindFlags::None;
            break;
        case MinTypMax::Max:
            maxFlags = BindFlags::None;
            break;
    }

    auto& min = create(compilation, *syntax.min, context, minFlags, assignmentTarget);
    auto& typ = create(compilation, *syntax.typ, context, typFlags, assignmentTarget);
    auto& max = create(compilation, *syntax.max, context, maxFlags, assignmentTarget);

    Expression* selected = nullptr;
    switch (compilation.getOptions().minTypMax) {
        case MinTypMax::Min:
            selected = &min;
            break;
        case MinTypMax::Typ:
            selected = &typ;
            break;
        case MinTypMax::Max:
            selected = &max;
            break;
    }

    auto result = compilation.emplace<MinTypMaxExpression>(*selected->type, min, typ, max, selected,
                                                           syntax.sourceRange());
    if (min.bad() || typ.bad() || max.bad())
        return badExpr(compilation, result);

    return *result;
}

bool MinTypMaxExpression::propagateType(const BindContext& context, const Type& newType) {
    // Only the selected expression gets a propagated type.
    type = &newType;
    contextDetermined(context, selected_, newType);
    return true;
}

ConstantValue MinTypMaxExpression::evalImpl(EvalContext& context) const {
    return selected().eval(context);
}

bool MinTypMaxExpression::verifyConstantImpl(EvalContext& context) const {
    return selected().verifyConstant(context);
}

optional<bitwidth_t> MinTypMaxExpression::getEffectiveWidthImpl() const {
    return selected().getEffectiveWidth();
}

void MinTypMaxExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("selected", selected());
}

Expression& CopyClassExpression::fromSyntax(Compilation& compilation,
                                            const CopyClassExpressionSyntax& syntax,
                                            const BindContext& context) {
    auto& source = selfDetermined(compilation, *syntax.expr, context);
    auto result =
        compilation.emplace<CopyClassExpression>(*source.type, source, syntax.sourceRange());
    if (source.bad())
        return badExpr(compilation, result);

    if (!source.type->isClass()) {
        context.addDiag(diag::CopyClassTarget, source.sourceRange) << *source.type;
        return badExpr(compilation, result);
    }

    return *result;
}

ConstantValue CopyClassExpression::evalImpl(EvalContext&) const {
    return nullptr;
}

bool CopyClassExpression::verifyConstantImpl(EvalContext& context) const {
    context.addDiag(diag::ConstEvalClassType, sourceRange);
    return false;
}

void CopyClassExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("sourceExpr", sourceExpr());
}

} // namespace slang

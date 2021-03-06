//------------------------------------------------------------------------------
//! @file Constraints.h
//! @brief Constraint creation and analysis
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#pragma once

#include "slang/symbols/ASTSerializer.h"
#include "slang/util/Util.h"

namespace slang {

// clang-format off
#define CONSTRAINT(x) \
    x(Invalid) \
    x(List)
ENUM(ConstraintKind, CONSTRAINT);
#undef CONTROL
// clang-format on

class BindContext;
class Compilation;
struct ConstraintItemSyntax;

class Constraint {
public:
    ConstraintKind kind;

    const ConstraintItemSyntax* syntax = nullptr;

    bool bad() const { return kind == ConstraintKind::Invalid; }

    static const Constraint& bind(const ConstraintItemSyntax& syntax, const BindContext& context);

    template<typename T>
    T& as() {
        ASSERT(T::isKind(kind));
        return *static_cast<T*>(this);
    }

    template<typename T>
    const T& as() const {
        ASSERT(T::isKind(kind));
        return *static_cast<const T*>(this);
    }

    template<typename TVisitor, typename... Args>
    decltype(auto) visit(TVisitor& visitor, Args&&... args) const;

protected:
    explicit Constraint(ConstraintKind kind) : kind(kind) {}

    static Constraint& badConstraint(Compilation& compilation, const Constraint* ctrl);
};

class InvalidConstraint : public Constraint {
public:
    const Constraint* child;

    explicit InvalidConstraint(const Constraint* child) :
        Constraint(ConstraintKind::Invalid), child(child) {}

    static bool isKind(ConstraintKind kind) { return kind == ConstraintKind::Invalid; }

    void serializeTo(ASTSerializer& serializer) const;
};

struct ConstraintBlockSyntax;

/// Represents a list of constraints.
class ConstraintList : public Constraint {
public:
    span<const Constraint* const> list;

    explicit ConstraintList(span<const Constraint* const> list) :
        Constraint(ConstraintKind::List), list(list) {}

    static Constraint& fromSyntax(const ConstraintBlockSyntax& syntax, const BindContext& context);

    void serializeTo(ASTSerializer& serializer) const;

    static bool isKind(ConstraintKind kind) { return kind == ConstraintKind::List; }
};

} // namespace slang

//===--- RawSyntax.h - Swift Raw Syntax Interface ---------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the RawSyntax type.
//
// These are the "backbone or "skeleton" of the Syntax tree, providing
// the recursive structure, child relationships, kind of node, etc.
//
// They are  strictly immutable, so can be shared freely among Syntax nodes and
// have no specific identity. They could even in theory be shared for
// expressions like 1 + 1 + 1 + 1 - you don't need 7 syntax nodes to express
// that at this layer.
//
// These are internal implementation ONLY - do not expose anything involving
// RawSyntax publicly. Clients of lib/Syntax should not be aware that they
// exist.
//
// RawSyntax nodes always live in a SyntaxArena. The user of the RawSyntax nodes
// is responsible to ensure that the SyntaxArena stays alive while the RawSyntax
// nodes are being accessed. During tree cration this is done by the
// SyntaxTreeCreator holding on to the arena. In lib/Syntax, the root SyntaxData
// node retains the syntax arena. Should a RawSyntaxNode A reference a node B
// from a different arena, it automatically adds B's arena as a child arena of
// A's arena, thereby keeping B's arena alive as long as A's arena is alive.
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SYNTAX_RAWSYNTAX_H
#define SWIFT_SYNTAX_RAWSYNTAX_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Syntax/References.h"
#include "swift/Syntax/SyntaxArena.h"
#include "swift/Syntax/SyntaxKind.h"
#include "swift/Syntax/TokenKinds.h"
#include "swift/Syntax/Trivia.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/TrailingObjects.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

using llvm::cast;
using llvm::StringRef;

#ifndef NDEBUG
#define syntax_assert_child_kind(Raw, Cursor, ExpectedKind)                    \
  do {                                                                         \
    if (auto __Child = Raw->getChild(Cursor))                                  \
      assert(__Child->getKind() == ExpectedKind);                              \
  } while (false)
#else
#define syntax_assert_child_kind(Raw, Cursor, ExpectedKind)
#endif

#ifndef NDEBUG
#define syntax_assert_child_token(Raw, CursorName, ...)                        \
  do {                                                                         \
    bool __Found = false;                                                      \
    if (auto __Token = Raw->getChild(Cursor::CursorName)) {                    \
      assert(__Token->isToken());                                              \
      if (__Token->isPresent()) {                                              \
        for (auto Token : {__VA_ARGS__}) {                                     \
          if (__Token->getTokenKind() == Token) {                              \
            __Found = true;                                                    \
            break;                                                             \
          }                                                                    \
        }                                                                      \
        assert(__Found && "invalid token supplied for " #CursorName            \
                          ", expected one of {" #__VA_ARGS__ "}");             \
      }                                                                        \
    }                                                                          \
  } while (false)
#else
#define syntax_assert_child_token(Raw, CursorName, ...)
#endif

#ifndef NDEBUG
#define syntax_assert_child_token_text(Raw, CursorName, TokenKind, ...)        \
  do {                                                                         \
    bool __Found = false;                                                      \
    if (auto __Child = Raw->getChild(Cursor::CursorName)) {                    \
      assert(__Child->isToken());                                              \
      if (__Child->isPresent()) {                                              \
        assert(__Child->getTokenKind() == TokenKind);                          \
        for (auto __Text : {__VA_ARGS__}) {                                    \
          if (__Child->getTokenText() == __Text) {                             \
            __Found = true;                                                    \
            break;                                                             \
          }                                                                    \
        }                                                                      \
        assert(__Found && "invalid text supplied for " #CursorName             \
                          ", expected one of {" #__VA_ARGS__ "}");             \
      }                                                                        \
    }                                                                          \
  } while (false)
#else
#define syntax_assert_child_token_text(Raw, CursorName, TokenKind, ...)
#endif

#ifndef NDEBUG
#define syntax_assert_token_is(Tok, Kind, Text)                                \
  do {                                                                         \
    assert(Tok.getTokenKind() == Kind);                                        \
    assert(Tok.getText() == Text);                                             \
  } while (false)
#else
#define syntax_assert_token_is(Tok, Kind, Text)
#endif

namespace swift {
namespace syntax {

class SyntaxArena;

using CursorIndex = size_t;

/// Get a numeric index suitable for array/vector indexing
/// from a syntax node's Cursor enum value.
template <typename CursorType> constexpr CursorIndex cursorIndex(CursorType C) {
  return static_cast<CursorIndex>(C);
}

/// An indicator of whether a Syntax node was found or written in the source.
///
/// This is not an 'implicit' bit.
enum class SourcePresence {
  /// The syntax was authored by a human and found, or was generated.
  Present,

  /// The syntax was expected or optional, but not found in the source.
  Missing,
};

/// The print option to specify when printing a raw syntax node.
struct SyntaxPrintOptions {
  bool Visual = false;
  bool PrintSyntaxKind = false;
  bool PrintTrivialNodeKind = false;
};

typedef unsigned SyntaxNodeId;

/// RawSyntax - the strictly immutable, shared backing nodes for all syntax.
///
/// This is implementation detail - do not expose it in public API.
class RawSyntax final
    : private llvm::TrailingObjects<RawSyntax, const RawSyntax *> {
  friend TrailingObjects;

  /// The ID that shall be used for the next node that is created and does not
  /// have a manually specified id
  static SyntaxNodeId NextFreeNodeId;

  /// An ID of this node that is stable across incremental parses
  SyntaxNodeId NodeId;

  /// The \c SyntaxArena in which this node was allocated.
  SyntaxArena *Arena;

  union {
    struct {
      /// Number of bytes this node takes up spelled out in the source code.
      /// Always 0 if the node is missing.
      unsigned TextLength : 32;
      /// Whether this piece of syntax was actually present in the source.
      unsigned Presence : 1;
      unsigned IsToken : 1;
    } Common;
    enum { NumRawSyntaxBits = 32 + 1 + 1 };

    // For "layout" nodes.
    struct {
      static_assert(NumRawSyntaxBits <= 64,
                    "Only 64 bits reserved for standard syntax bits");
      uint64_t : bitmax(NumRawSyntaxBits, 64); // align to 32 bits
      /// Number of children this "layout" node has.
      unsigned NumChildren : 32;
      /// Total number of sub nodes, i.e. number of transitive children of this
      /// node. This does not include the node itself.
      unsigned TotalSubNodeCount : 32;
      /// The kind of syntax this node represents.
      unsigned Kind : bitmax(NumSyntaxKindBits, 8);
    } Layout;

    // For "token" nodes.
    struct {
      static_assert(NumRawSyntaxBits <= 64,
                    "Only 64 bits reserved for standard syntax bits");
      uint64_t : bitmax(NumRawSyntaxBits, 64); // align to 16 bits
      /// The kind of token this "token" node represents.
      const char *LeadingTrivia;
      const char *TokenText;
      const char *TrailingTrivia;
      unsigned LeadingTriviaLength : 32;
      unsigned TokenLength : 32;
      unsigned TrailingTriviaLength : 32;
      unsigned TokenKind : 16;
    } Token;
  } Bits;

  size_t numTrailingObjects(OverloadToken<const RawSyntax *>) const {
    return isToken() ? 0 : Bits.Layout.NumChildren;
  }

  /// Constructor for creating layout nodes.
  /// If the node has been allocated inside the bump allocator of a
  /// \c SyntaxArena, that arena must be passed as \p Arena to retain the node's
  /// underlying storage.
  /// If \p NodeId is \c None, the next free NodeId is used, if it is passed,
  /// the caller needs to assure that the node ID has not been used yet.
  RawSyntax(SyntaxKind Kind, ArrayRef<const RawSyntax *> Layout,
            size_t TextLength, SourcePresence Presence,
            const RC<SyntaxArena> &Arena, llvm::Optional<SyntaxNodeId> NodeId)
      : Arena(Arena.get()),
        Bits({{unsigned(TextLength), unsigned(Presence), false}}) {
    assert(Arena && "RawSyntax nodes must always be allocated in an arena");
    assert(
        Kind != SyntaxKind::Token &&
        "'token' syntax node must be constructed with dedicated constructor");

    size_t TotalSubNodeCount = 0;
    for (auto Child : Layout) {
      if (Child) {
        TotalSubNodeCount += Child->getTotalSubNodeCount() + 1;
        // If the child is stored in a different arena, it needs to stay alive
        // as long as this node's arena is alive.
        Arena->addChildArena(Child->Arena);
      }
    }

    if (NodeId.hasValue()) {
      this->NodeId = NodeId.getValue();
      NextFreeNodeId = std::max(this->NodeId + 1, NextFreeNodeId);
    } else {
      this->NodeId = NextFreeNodeId++;
    }
    Bits.Layout.NumChildren = Layout.size();
    Bits.Layout.TotalSubNodeCount = TotalSubNodeCount;
    Bits.Layout.Kind = unsigned(Kind);

    // Initialize layout data.
    std::uninitialized_copy(Layout.begin(), Layout.end(),
                            getTrailingObjects<const RawSyntax *>());
  }

  /// Constructor for creating token nodes
  /// \c SyntaxArena, that arena must be passed as \p Arena to retain the node's
  /// underlying storage.
  /// If \p NodeId is \c None, the next free NodeId is used, if it is passed,
  /// the caller needs to assure that the NodeId has not been used yet.
  RawSyntax(tok TokKind, StringRef Text, size_t TextLength,
            StringRef LeadingTrivia, StringRef TrailingTrivia,
            SourcePresence Presence, const RC<SyntaxArena> &Arena,
            llvm::Optional<SyntaxNodeId> NodeId)
      : Arena(Arena.get()),
        Bits({{unsigned(TextLength), unsigned(Presence), true}}) {
    assert(Arena && "RawSyntax nodes must always be allocated in an arena");

    if (Presence == SourcePresence::Missing) {
      assert(TextLength == 0);
    } else {
      assert(TextLength ==
             LeadingTrivia.size() + Text.size() + TrailingTrivia.size());
    }

    if (NodeId.hasValue()) {
      this->NodeId = NodeId.getValue();
      NextFreeNodeId = std::max(this->NodeId + 1, NextFreeNodeId);
    } else {
      this->NodeId = NextFreeNodeId++;
    }
    Bits.Token.LeadingTrivia = LeadingTrivia.data();
    Bits.Token.TokenText = Text.data();
    Bits.Token.TrailingTrivia = TrailingTrivia.data();
    Bits.Token.LeadingTriviaLength = LeadingTrivia.size();
    Bits.Token.TokenLength = Text.size();
    Bits.Token.TrailingTriviaLength = TrailingTrivia.size();
    Bits.Token.TokenKind = unsigned(TokKind);

    Arena->copyStringToArenaIfNecessary(Bits.Token.LeadingTrivia,
                                        Bits.Token.LeadingTriviaLength);
    Arena->copyStringToArenaIfNecessary(Bits.Token.TokenText,
                                        Bits.Token.TokenLength);
    Arena->copyStringToArenaIfNecessary(Bits.Token.TrailingTrivia,
                                        Bits.Token.TrailingTriviaLength);
  }

  /// Compute the node's text length by summing up the length of its childern
  size_t computeTextLength() {
    size_t TextLength = 0;
    for (size_t I = 0, NumChildren = getNumChildren(); I < NumChildren; ++I) {
      auto ChildNode = getChild(I);
      if (ChildNode && !ChildNode->isMissing()) {
        TextLength += ChildNode->getTextLength();
      }
    }
    return TextLength;
  }

public:
  /// \name Factory methods.
  /// @{

  /// Make a raw "layout" syntax node.
  static const RawSyntax *
  make(SyntaxKind Kind, ArrayRef<const RawSyntax *> Layout, size_t TextLength,
       SourcePresence Presence, const RC<SyntaxArena> &Arena,
       llvm::Optional<SyntaxNodeId> NodeId = llvm::None) {
    assert(Arena && "RawSyntax nodes must always be allocated in an arena");
    auto size = totalSizeToAlloc<const RawSyntax *>(Layout.size());
    void *data = Arena->Allocate(size, alignof(RawSyntax));
    return new (data)
        RawSyntax(Kind, Layout, TextLength, Presence, Arena, NodeId);
  }

  static const RawSyntax *
  makeAndCalcLength(SyntaxKind Kind, ArrayRef<const RawSyntax *> Layout,
                    SourcePresence Presence, const RC<SyntaxArena> &Arena,
                    llvm::Optional<SyntaxNodeId> NodeId = llvm::None) {
    size_t TextLength = 0;
    for (auto Child : Layout) {
      if (Child) {
        TextLength += Child->getTextLength();
      }
    }
    return make(Kind, Layout, TextLength, Presence, Arena, NodeId);
  }

  /// Make a raw "token" syntax node.
  static const RawSyntax *
  make(tok TokKind, StringRef Text, size_t TextLength, StringRef LeadingTrivia,
       StringRef TrailingTrivia, SourcePresence Presence,
       const RC<SyntaxArena> &Arena,
       llvm::Optional<SyntaxNodeId> NodeId = llvm::None) {
    assert(Arena && "RawSyntax nodes must always be allocated in an arena");
    auto size = totalSizeToAlloc<const RawSyntax *>(0);
    void *data = Arena->Allocate(size, alignof(RawSyntax));
    return new (data) RawSyntax(TokKind, Text, TextLength, LeadingTrivia,
                                TrailingTrivia, Presence, Arena, NodeId);
  }

  /// Make a raw "token" syntax node that was allocated in \p Arena.
  static const RawSyntax *
  makeAndCalcLength(tok TokKind, StringRef Text, StringRef LeadingTrivia,
                    StringRef TrailingTrivia, SourcePresence Presence,
                    const RC<SyntaxArena> &Arena,
                    llvm::Optional<SyntaxNodeId> NodeId = llvm::None) {
    size_t TextLength = 0;
    if (Presence != SourcePresence::Missing) {
      TextLength += LeadingTrivia.size();
      TextLength += Text.size();
      TextLength += TrailingTrivia.size();
    }
    return make(TokKind, Text, TextLength, LeadingTrivia, TrailingTrivia,
                Presence, Arena, NodeId);
  }

  /// Make a missing raw "layout" syntax node.
  static const RawSyntax *missing(SyntaxKind Kind,
                                  const RC<SyntaxArena> &Arena) {
    return make(Kind, {}, /*TextLength=*/0, SourcePresence::Missing, Arena);
  }

  /// Make a missing raw "token" syntax node.
  static const RawSyntax *missing(tok TokKind, StringRef Text,
                                  const RC<SyntaxArena> &Arena) {
    return make(TokKind, Text, /*TextLength=*/0, {}, {},
                SourcePresence::Missing, Arena);
  }

  /// @}

  /// Return the arena in which this \c RawSyntax node has been allocated.
  /// Keep in mind that the \c RawSyntax node *does not* retain the arena.
  RC<SyntaxArena> getArena() const { return RC<SyntaxArena>(Arena); }

  SourcePresence getPresence() const {
    return static_cast<SourcePresence>(Bits.Common.Presence);
  }

  SyntaxKind getKind() const {
    if (Bits.Common.IsToken) {
      return SyntaxKind::Token;
    } else {
      return static_cast<SyntaxKind>(Bits.Layout.Kind);
    }
  }

  /// Get the number of nodes included in the subtree spanned by this node.
  /// This includes all transitive children and this node itself.
  size_t getTotalNodes() const { return getTotalSubNodeCount() + 1; }

  /// Get the number of transitive children of this node. This does not include
  /// the node itself.
  size_t getTotalSubNodeCount() const {
    if (isToken()) {
      return 0;
    } else {
      return Bits.Layout.TotalSubNodeCount;
    }
  }

  /// Get an ID for this node that is stable across incremental parses
  SyntaxNodeId getId() const { return NodeId; }

  /// Returns true if the node is "missing" in the source (i.e. it was
  /// expected (or optional) but not written.
  bool isMissing() const { return getPresence() == SourcePresence::Missing; }

  /// Returns true if the node is "present" in the source.
  bool isPresent() const { return getPresence() == SourcePresence::Present; }

  /// Returns true if this raw syntax node is some kind of declaration.
  bool isDecl() const { return isDeclKind(getKind()); }

  /// Returns true if this raw syntax node is some kind of type syntax.
  bool isType() const { return isTypeKind(getKind()); }

  /// Returns true if this raw syntax node is some kind of statement.
  bool isStmt() const { return isStmtKind(getKind()); }

  /// Returns true if this raw syntax node is some kind of expression.
  bool isExpr() const { return isExprKind(getKind()); }

  /// Returns true if this raw syntax node is some kind of pattern.
  bool isPattern() const { return isPatternKind(getKind()); }

  /// Return true is this raw syntax node is a unknown node.
  bool isUnknown() const { return isUnknownKind(getKind()); }

  /// Return true if this raw syntax node is a token.
  bool isToken() const { return Bits.Common.IsToken; }

  /// \name Getter routines for SyntaxKind::Token.
  /// @{

  /// Get the kind of the token.
  tok getTokenKind() const {
    assert(isToken());
    return static_cast<tok>(Bits.Token.TokenKind);
  }

  /// Return the text of the token as a reference. The referenced buffer may
  /// disappear when the syntax node gets freed.
  StringRef getTokenText() const {
    assert(isToken());
    return StringRef(Bits.Token.TokenText, Bits.Token.TokenLength);
  }

  /// Return the unparsed leading trivia of the token.
  StringRef getLeadingTrivia() const {
    assert(isToken());
    return StringRef(Bits.Token.LeadingTrivia, Bits.Token.LeadingTriviaLength);
  }

  /// Return the unparsed trailing trivia of the token.
  StringRef getTrailingTrivia() const {
    assert(isToken());
    return StringRef(Bits.Token.TrailingTrivia,
                     Bits.Token.TrailingTriviaLength);
  }

  /// Return pieces that make up the leading trivia of the token.
  /// Note that this triggers trivia parsing which may be expensive. If the
  /// trivia pieces are required multiple times, consider caching them.
  Trivia getLeadingTriviaPieces() const;

  /// Return the trailing trivia list of the token.
  /// Note that this triggers trivia parsing which may be expensive. If the
  /// trivia pieces are required multiple times, consider caching them.
  Trivia getTrailingTriviaPieces() const;

  /// Return \c true if this is the given kind of token.
  bool isToken(tok K) const { return isToken() && getTokenKind() == K; }

  /// @}

  /// \name Transform routines for "token" nodes.
  /// @{

  /// Return a new token like this one, but with the given leading
  /// trivia instead.
  const RawSyntax *withLeadingTrivia(StringRef NewLeadingTrivia) const {
    return makeAndCalcLength(getTokenKind(), getTokenText(), NewLeadingTrivia,
                             getTrailingTrivia(), getPresence(), Arena);
  }

  /// Return a new token like this one, but with the given trailing
  /// trivia instead.
  const RawSyntax *withTrailingTrivia(StringRef NewTrailingTrivia) const {
    return makeAndCalcLength(getTokenKind(), getTokenText(), getLeadingTrivia(),
                             NewTrailingTrivia, getPresence(), Arena);
  }

  /// @}

  /// \name Getter routines for "layout" nodes.
  /// @{

  /// Get the child nodes.
  ArrayRef<const RawSyntax *> getLayout() const {
    if (isToken())
      return {};
    return {getTrailingObjects<const RawSyntax *>(), Bits.Layout.NumChildren};
  }

  size_t getNumChildren() const {
    if (isToken())
      return 0;
    return Bits.Layout.NumChildren;
  }

  /// Get a child based on a particular node's "Cursor", indicating
  /// the position of the terms in the production of the Swift grammar.
  const RawSyntax *getChild(CursorIndex Index) const {
    assert(Index < getNumChildren() && "Index out of bounds");
    return getLayout()[Index];
  }

  /// Return the number of bytes this node takes when spelled out in the source
  /// including trivia.
  size_t getTextLength() const { return Bits.Common.TextLength; }

  /// @}

  /// \name Transform routines for "layout" nodes.
  /// @{

  /// Return a new raw syntax node with the given new layout element appended
  /// to the end of the node's layout.
  const RawSyntax *append(const RawSyntax *NewLayoutElement) const;

  /// Return a new raw syntax node with the given new layout element replacing
  /// another at some cursor position.
  const RawSyntax *replacingChild(CursorIndex Index,
                                  const RawSyntax *NewLayoutElement) const;

  /// @}

  size_t getLeadingTriviaLength() const { return getLeadingTrivia().size(); }

  size_t getTrailingTriviaLength() const { return getTrailingTrivia().size(); }

  /// Print this piece of syntax recursively.
  void print(llvm::raw_ostream &OS, SyntaxPrintOptions Opts) const;

  /// Dump this piece of syntax recursively for debugging or testing.
  SWIFT_DEBUG_DUMP;

  /// Dump this piece of syntax recursively.
  void dump(llvm::raw_ostream &OS, unsigned Indent = 0) const;

  static void Profile(llvm::FoldingSetNodeID &ID, tok TokKind, StringRef Text,
                      StringRef LeadingTrivia, StringRef TrailingTrivia);
};

} // end namespace syntax
} // end namespace swift


#endif // SWIFT_SYNTAX_RAWSYNTAX_H

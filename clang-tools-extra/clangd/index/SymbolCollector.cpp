//===--- SymbolCollector.cpp -------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolCollector.h"
#include "AST.h"
#include "CanonicalIncludes.h"
#include "CodeComplete.h"
#include "CodeCompletionStrings.h"
#include "ExpectedTypes.h"
#include "Logger.h"
#include "SourceCode.h"
#include "SymbolLocation.h"
#include "URI.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

namespace clang {
namespace clangd {
namespace {

/// If \p ND is a template specialization, returns the described template.
/// Otherwise, returns \p ND.
const NamedDecl &getTemplateOrThis(const NamedDecl &ND) {
  if (auto T = ND.getDescribedTemplate())
    return *T;
  return ND;
}

// Returns a URI of \p Path. Firstly, this makes the \p Path absolute using the
// current working directory of the given SourceManager if the Path is not an
// absolute path. If failed, this resolves relative paths against \p FallbackDir
// to get an absolute path. Then, this tries creating an URI for the absolute
// path with schemes specified in \p Opts. This returns an URI with the first
// working scheme, if there is any; otherwise, this returns None.
//
// The Path can be a path relative to the build directory, or retrieved from
// the SourceManager.
std::string toURI(const SourceManager &SM, llvm::StringRef Path,
                  const SymbolCollector::Options &Opts) {
  llvm::SmallString<128> AbsolutePath(Path);
  if (auto File = SM.getFileManager().getFile(Path)) {
    if (auto CanonPath = getCanonicalPath(*File, SM)) {
      AbsolutePath = *CanonPath;
    }
  }
  // We don't perform is_absolute check in an else branch because makeAbsolute
  // might return a relative path on some InMemoryFileSystems.
  if (!llvm::sys::path::is_absolute(AbsolutePath) && !Opts.FallbackDir.empty())
    llvm::sys::fs::make_absolute(Opts.FallbackDir, AbsolutePath);
  llvm::sys::path::remove_dots(AbsolutePath, /*remove_dot_dot=*/true);
  return URI::create(AbsolutePath).toString();
}

// All proto generated headers should start with this line.
static const char *PROTO_HEADER_COMMENT =
    "// Generated by the protocol buffer compiler.  DO NOT EDIT!";

// Checks whether the decl is a private symbol in a header generated by
// protobuf compiler.
// To identify whether a proto header is actually generated by proto compiler,
// we check whether it starts with PROTO_HEADER_COMMENT.
// FIXME: make filtering extensible when there are more use cases for symbol
// filters.
bool isPrivateProtoDecl(const NamedDecl &ND) {
  const auto &SM = ND.getASTContext().getSourceManager();
  auto Loc = spellingLocIfSpelled(findName(&ND), SM);
  auto FileName = SM.getFilename(Loc);
  if (!FileName.endswith(".proto.h") && !FileName.endswith(".pb.h"))
    return false;
  auto FID = SM.getFileID(Loc);
  // Double check that this is an actual protobuf header.
  if (!SM.getBufferData(FID).startswith(PROTO_HEADER_COMMENT))
    return false;

  // ND without identifier can be operators.
  if (ND.getIdentifier() == nullptr)
    return false;
  auto Name = ND.getIdentifier()->getName();
  if (!Name.contains('_'))
    return false;
  // Nested proto entities (e.g. Message::Nested) have top-level decls
  // that shouldn't be used (Message_Nested). Ignore them completely.
  // The nested entities are dangling type aliases, we may want to reconsider
  // including them in the future.
  // For enum constants, SOME_ENUM_CONSTANT is not private and should be
  // indexed. Outer_INNER is private. This heuristic relies on naming style, it
  // will include OUTER_INNER and exclude some_enum_constant.
  // FIXME: the heuristic relies on naming style (i.e. no underscore in
  // user-defined names) and can be improved.
  return (ND.getKind() != Decl::EnumConstant) || llvm::any_of(Name, islower);
}

// We only collect #include paths for symbols that are suitable for global code
// completion, except for namespaces since #include path for a namespace is hard
// to define.
bool shouldCollectIncludePath(index::SymbolKind Kind) {
  using SK = index::SymbolKind;
  switch (Kind) {
  case SK::Macro:
  case SK::Enum:
  case SK::Struct:
  case SK::Class:
  case SK::Union:
  case SK::TypeAlias:
  case SK::Using:
  case SK::Function:
  case SK::Variable:
  case SK::EnumConstant:
    return true;
  default:
    return false;
  }
}

// Return the symbol range of the token at \p TokLoc.
std::pair<SymbolLocation::Position, SymbolLocation::Position>
getTokenRange(SourceLocation TokLoc, const SourceManager &SM,
              const LangOptions &LangOpts) {
  auto CreatePosition = [&SM](SourceLocation Loc) {
    auto LSPLoc = sourceLocToPosition(SM, Loc);
    SymbolLocation::Position Pos;
    Pos.setLine(LSPLoc.line);
    Pos.setColumn(LSPLoc.character);
    return Pos;
  };

  auto TokenLength = clang::Lexer::MeasureTokenLength(TokLoc, SM, LangOpts);
  return {CreatePosition(TokLoc),
          CreatePosition(TokLoc.getLocWithOffset(TokenLength))};
}

// Return the symbol location of the token at \p TokLoc.
llvm::Optional<SymbolLocation>
getTokenLocation(SourceLocation TokLoc, const SourceManager &SM,
                 const SymbolCollector::Options &Opts,
                 const clang::LangOptions &LangOpts,
                 std::string &FileURIStorage) {
  auto Path = SM.getFilename(TokLoc);
  if (Path.empty())
    return None;
  FileURIStorage = toURI(SM, Path, Opts);
  SymbolLocation Result;
  Result.FileURI = FileURIStorage.c_str();
  auto Range = getTokenRange(TokLoc, SM, LangOpts);
  Result.Start = Range.first;
  Result.End = Range.second;

  return Result;
}

// Checks whether \p ND is a definition of a TagDecl (class/struct/enum/union)
// in a header file, in which case clangd would prefer to use ND as a canonical
// declaration.
// FIXME: handle symbol types that are not TagDecl (e.g. functions), if using
// the first seen declaration as canonical declaration is not a good enough
// heuristic.
bool isPreferredDeclaration(const NamedDecl &ND, index::SymbolRoleSet Roles) {
  const auto &SM = ND.getASTContext().getSourceManager();
  return (Roles & static_cast<unsigned>(index::SymbolRole::Definition)) &&
         isa<TagDecl>(&ND) && !isInsideMainFile(ND.getLocation(), SM);
}

RefKind toRefKind(index::SymbolRoleSet Roles) {
  return static_cast<RefKind>(static_cast<unsigned>(RefKind::All) & Roles);
}

bool shouldIndexRelation(const index::SymbolRelation &R) {
  // We currently only index BaseOf relations, for type hierarchy subtypes.
  return R.Roles & static_cast<unsigned>(index::SymbolRole::RelationBaseOf);
}

} // namespace

SymbolCollector::SymbolCollector(Options Opts) : Opts(std::move(Opts)) {}

void SymbolCollector::initialize(ASTContext &Ctx) {
  ASTCtx = &Ctx;
  CompletionAllocator = std::make_shared<GlobalCodeCompletionAllocator>();
  CompletionTUInfo =
      std::make_unique<CodeCompletionTUInfo>(CompletionAllocator);
}

bool SymbolCollector::shouldCollectSymbol(const NamedDecl &ND,
                                          const ASTContext &ASTCtx,
                                          const Options &Opts,
                                          bool IsMainFileOnly) {
  // Skip anonymous declarations, e.g (anonymous enum/class/struct).
  if (ND.getDeclName().isEmpty())
    return false;

  // Skip main-file symbols if we are not collecting them.
  if (IsMainFileOnly && !Opts.CollectMainFileSymbols)
    return false;

  // Skip symbols in anonymous namespaces in header files.
  if (!IsMainFileOnly && ND.isInAnonymousNamespace())
    return false;

  // We want most things but not "local" symbols such as symbols inside
  // FunctionDecl, BlockDecl, ObjCMethodDecl and OMPDeclareReductionDecl.
  // FIXME: Need a matcher for ExportDecl in order to include symbols declared
  // within an export.
  const auto *DeclCtx = ND.getDeclContext();
  switch (DeclCtx->getDeclKind()) {
  case Decl::TranslationUnit:
  case Decl::Namespace:
  case Decl::LinkageSpec:
  case Decl::Enum:
  case Decl::ObjCProtocol:
  case Decl::ObjCInterface:
  case Decl::ObjCCategory:
  case Decl::ObjCCategoryImpl:
  case Decl::ObjCImplementation:
    break;
  default:
    // Record has a few derivations (e.g. CXXRecord, Class specialization), it's
    // easier to cast.
    if (!isa<RecordDecl>(DeclCtx))
      return false;
  }

  // Avoid indexing internal symbols in protobuf generated headers.
  if (isPrivateProtoDecl(ND))
    return false;
  return true;
}

// Always return true to continue indexing.
bool SymbolCollector::handleDeclOccurence(
    const Decl *D, index::SymbolRoleSet Roles,
    llvm::ArrayRef<index::SymbolRelation> Relations, SourceLocation Loc,
    index::IndexDataConsumer::ASTNodeInfo ASTNode) {
  assert(ASTCtx && PP.get() && "ASTContext and Preprocessor must be set.");
  assert(CompletionAllocator && CompletionTUInfo);
  assert(ASTNode.OrigD);
  // Indexing API puts cannonical decl into D, which might not have a valid
  // source location for implicit/built-in decls. Fallback to original decl in
  // such cases.
  if (D->getLocation().isInvalid())
    D = ASTNode.OrigD;
  // If OrigD is an declaration associated with a friend declaration and it's
  // not a definition, skip it. Note that OrigD is the occurrence that the
  // collector is currently visiting.
  if ((ASTNode.OrigD->getFriendObjectKind() !=
       Decl::FriendObjectKind::FOK_None) &&
      !(Roles & static_cast<unsigned>(index::SymbolRole::Definition)))
    return true;
  // A declaration created for a friend declaration should not be used as the
  // canonical declaration in the index. Use OrigD instead, unless we've already
  // picked a replacement for D
  if (D->getFriendObjectKind() != Decl::FriendObjectKind::FOK_None)
    D = CanonicalDecls.try_emplace(D, ASTNode.OrigD).first->second;
  const NamedDecl *ND = dyn_cast<NamedDecl>(D);
  if (!ND)
    return true;

  // Mark D as referenced if this is a reference coming from the main file.
  // D may not be an interesting symbol, but it's cheaper to check at the end.
  auto &SM = ASTCtx->getSourceManager();
  auto SpellingLoc = SM.getSpellingLoc(Loc);
  if (Opts.CountReferences &&
      (Roles & static_cast<unsigned>(index::SymbolRole::Reference)) &&
      SM.getFileID(SpellingLoc) == SM.getMainFileID())
    ReferencedDecls.insert(ND);

  auto ID = getSymbolID(ND);
  if (!ID)
    return true;

  // Note: we need to process relations for all decl occurrences, including
  // refs, because the indexing code only populates relations for specific
  // occurrences. For example, RelationBaseOf is only populated for the
  // occurrence inside the base-specifier.
  processRelations(*ND, *ID, Relations);

  bool CollectRef = static_cast<unsigned>(Opts.RefFilter) & Roles;
  bool IsOnlyRef =
      !(Roles & (static_cast<unsigned>(index::SymbolRole::Declaration) |
                 static_cast<unsigned>(index::SymbolRole::Definition)));

  if (IsOnlyRef && !CollectRef)
    return true;

  // ND is the canonical (i.e. first) declaration. If it's in the main file
  // (which is not a header), then no public declaration was visible, so assume
  // it's main-file only.
  bool IsMainFileOnly =
      SM.isWrittenInMainFile(SM.getExpansionLoc(ND->getBeginLoc())) &&
      !ASTCtx->getLangOpts().IsHeaderFile;
  // In C, printf is a redecl of an implicit builtin! So check OrigD instead.
  if (ASTNode.OrigD->isImplicit() ||
      !shouldCollectSymbol(*ND, *ASTCtx, Opts, IsMainFileOnly))
    return true;
  // Do not store references to main-file symbols.
  if (CollectRef && !IsMainFileOnly && !isa<NamespaceDecl>(ND) &&
      (Opts.RefsInHeaders || SM.getFileID(SpellingLoc) == SM.getMainFileID()))
    DeclRefs[ND].emplace_back(SpellingLoc, Roles);
  // Don't continue indexing if this is a mere reference.
  if (IsOnlyRef)
    return true;

  // FIXME: ObjCPropertyDecl are not properly indexed here:
  // - ObjCPropertyDecl may have an OrigD of ObjCPropertyImplDecl, which is
  // not a NamedDecl.
  auto *OriginalDecl = dyn_cast<NamedDecl>(ASTNode.OrigD);
  if (!OriginalDecl)
    return true;

  const Symbol *BasicSymbol = Symbols.find(*ID);
  if (!BasicSymbol) // Regardless of role, ND is the canonical declaration.
    BasicSymbol = addDeclaration(*ND, std::move(*ID), IsMainFileOnly);
  else if (isPreferredDeclaration(*OriginalDecl, Roles))
    // If OriginalDecl is preferred, replace the existing canonical
    // declaration (e.g. a class forward declaration). There should be at most
    // one duplicate as we expect to see only one preferred declaration per
    // TU, because in practice they are definitions.
    BasicSymbol = addDeclaration(*OriginalDecl, std::move(*ID), IsMainFileOnly);

  if (Roles & static_cast<unsigned>(index::SymbolRole::Definition))
    addDefinition(*OriginalDecl, *BasicSymbol);

  return true;
}

bool SymbolCollector::handleMacroOccurence(const IdentifierInfo *Name,
                                           const MacroInfo *MI,
                                           index::SymbolRoleSet Roles,
                                           SourceLocation Loc) {
  if (!Opts.CollectMacro)
    return true;
  assert(PP.get());

  const auto &SM = PP->getSourceManager();
  auto DefLoc = MI->getDefinitionLoc();

  // Builtin macros don't have useful locations and aren't needed in completion.
  if (MI->isBuiltinMacro())
    return true;

  // Skip main-file symbols if we are not collecting them.
  bool IsMainFileSymbol = SM.isInMainFile(SM.getExpansionLoc(DefLoc));
  if (IsMainFileSymbol && !Opts.CollectMainFileSymbols)
    return false;

  // Also avoid storing predefined macros like __DBL_MIN__.
  if (SM.isWrittenInBuiltinFile(DefLoc))
    return true;

  // Mark the macro as referenced if this is a reference coming from the main
  // file. The macro may not be an interesting symbol, but it's cheaper to check
  // at the end.
  if (Opts.CountReferences &&
      (Roles & static_cast<unsigned>(index::SymbolRole::Reference)) &&
      SM.getFileID(SM.getSpellingLoc(Loc)) == SM.getMainFileID())
    ReferencedMacros.insert(Name);
  // Don't continue indexing if this is a mere reference.
  // FIXME: remove macro with ID if it is undefined.
  if (!(Roles & static_cast<unsigned>(index::SymbolRole::Declaration) ||
        Roles & static_cast<unsigned>(index::SymbolRole::Definition)))
    return true;

  auto ID = getSymbolID(Name->getName(), MI, SM);
  if (!ID)
    return true;

  // Only collect one instance in case there are multiple.
  if (Symbols.find(*ID) != nullptr)
    return true;

  Symbol S;
  S.ID = std::move(*ID);
  S.Name = Name->getName();
  if (!IsMainFileSymbol) {
    S.Flags |= Symbol::IndexedForCodeCompletion;
    S.Flags |= Symbol::VisibleOutsideFile;
  }
  S.SymInfo = index::getSymbolInfoForMacro(*MI);
  std::string FileURI;
  // FIXME: use the result to filter out symbols.
  shouldIndexFile(SM.getFileID(Loc));
  if (auto DeclLoc =
          getTokenLocation(DefLoc, SM, Opts, PP->getLangOpts(), FileURI))
    S.CanonicalDeclaration = *DeclLoc;

  CodeCompletionResult SymbolCompletion(Name);
  const auto *CCS = SymbolCompletion.CreateCodeCompletionStringForMacro(
      *PP, *CompletionAllocator, *CompletionTUInfo);
  std::string Signature;
  std::string SnippetSuffix;
  getSignature(*CCS, &Signature, &SnippetSuffix);
  S.Signature = Signature;
  S.CompletionSnippetSuffix = SnippetSuffix;

  IndexedMacros.insert(Name);
  setIncludeLocation(S, DefLoc);
  Symbols.insert(S);
  return true;
}

void SymbolCollector::processRelations(
    const NamedDecl &ND, const SymbolID &ID,
    ArrayRef<index::SymbolRelation> Relations) {
  // Store subtype relations.
  if (!dyn_cast<TagDecl>(&ND))
    return;

  for (const auto &R : Relations) {
    if (!shouldIndexRelation(R))
      continue;

    const Decl *Object = R.RelatedSymbol;

    auto ObjectID = getSymbolID(Object);
    if (!ObjectID)
      continue;

    // Record the relation.
    // TODO: There may be cases where the object decl is not indexed for some
    // reason. Those cases should probably be removed in due course, but for
    // now there are two possible ways to handle it:
    //   (A) Avoid storing the relation in such cases.
    //   (B) Store it anyways. Clients will likely lookup() the SymbolID
    //       in the index and find nothing, but that's a situation they
    //       probably need to handle for other reasons anyways.
    // We currently do (B) because it's simpler.
    this->Relations.insert(Relation{ID, RelationKind::BaseOf, *ObjectID});
  }
}

void SymbolCollector::setIncludeLocation(const Symbol &S, SourceLocation Loc) {
  if (Opts.CollectIncludePath)
    if (shouldCollectIncludePath(S.SymInfo.Kind))
      // Use the expansion location to get the #include header since this is
      // where the symbol is exposed.
      IncludeFiles[S.ID] =
          PP->getSourceManager().getDecomposedExpansionLoc(Loc).first;
}

void SymbolCollector::finish() {
  // At the end of the TU, add 1 to the refcount of all referenced symbols.
  auto IncRef = [this](const SymbolID &ID) {
    if (const auto *S = Symbols.find(ID)) {
      Symbol Inc = *S;
      ++Inc.References;
      Symbols.insert(Inc);
    }
  };
  for (const NamedDecl *ND : ReferencedDecls) {
    if (auto ID = getSymbolID(ND)) {
      IncRef(*ID);
    }
  }
  if (Opts.CollectMacro) {
    assert(PP);
    // First, drop header guards. We can't identify these until EOF.
    for (const IdentifierInfo *II : IndexedMacros) {
      if (const auto *MI = PP->getMacroDefinition(II).getMacroInfo())
        if (auto ID = getSymbolID(II->getName(), MI, PP->getSourceManager()))
          if (MI->isUsedForHeaderGuard())
            Symbols.erase(*ID);
    }
    // Now increment refcounts.
    for (const IdentifierInfo *II : ReferencedMacros) {
      if (const auto *MI = PP->getMacroDefinition(II).getMacroInfo())
        if (auto ID = getSymbolID(II->getName(), MI, PP->getSourceManager()))
          IncRef(*ID);
    }
  }

  // Fill in IncludeHeaders.
  // We delay this until end of TU so header guards are all resolved.
  // Symbols in slabs aren' mutable, so insert() has to walk all the strings :-(
  llvm::SmallString<256> QName;
  for (const auto &Entry : IncludeFiles)
    if (const Symbol *S = Symbols.find(Entry.first)) {
      QName = S->Scope;
      QName.append(S->Name);
      if (auto Header = getIncludeHeader(QName, Entry.second)) {
        Symbol NewSym = *S;
        NewSym.IncludeHeaders.push_back({*Header, 1});
        Symbols.insert(NewSym);
      }
    }

  const auto &SM = ASTCtx->getSourceManager();
  llvm::DenseMap<FileID, std::string> URICache;
  auto GetURI = [&](FileID FID) -> llvm::Optional<std::string> {
    auto Found = URICache.find(FID);
    if (Found == URICache.end()) {
      if (auto *FileEntry = SM.getFileEntryForID(FID)) {
        auto FileURI = toURI(SM, FileEntry->getName(), Opts);
        Found = URICache.insert({FID, FileURI}).first;
      } else {
        // Ignore cases where we can not find a corresponding file entry
        // for the loc, thoses are not interesting, e.g. symbols formed
        // via macro concatenation.
        return None;
      }
    }
    return Found->second;
  };
  // Populate Refs slab from DeclRefs.
  if (auto MainFileURI = GetURI(SM.getMainFileID())) {
    for (const auto &It : DeclRefs) {
      if (auto ID = getSymbolID(It.first)) {
        for (const auto &LocAndRole : It.second) {
          auto FileID = SM.getFileID(LocAndRole.first);
          // FIXME: use the result to filter out references.
          shouldIndexFile(FileID);
          if (auto FileURI = GetURI(FileID)) {
            auto Range =
                getTokenRange(LocAndRole.first, SM, ASTCtx->getLangOpts());
            Ref R;
            R.Location.Start = Range.first;
            R.Location.End = Range.second;
            R.Location.FileURI = FileURI->c_str();
            R.Kind = toRefKind(LocAndRole.second);
            Refs.insert(*ID, R);
          }
        }
      }
    }
  }

  ReferencedDecls.clear();
  ReferencedMacros.clear();
  DeclRefs.clear();
  FilesToIndexCache.clear();
  HeaderIsSelfContainedCache.clear();
  IncludeFiles.clear();
}

const Symbol *SymbolCollector::addDeclaration(const NamedDecl &ND, SymbolID ID,
                                              bool IsMainFileOnly) {
  auto &Ctx = ND.getASTContext();
  auto &SM = Ctx.getSourceManager();

  Symbol S;
  S.ID = std::move(ID);
  std::string QName = printQualifiedName(ND);
  // FIXME: this returns foo:bar: for objective-C methods, we prefer only foo:
  // for consistency with CodeCompletionString and a clean name/signature split.
  std::tie(S.Scope, S.Name) = splitQualifiedName(QName);
  std::string TemplateSpecializationArgs = printTemplateSpecializationArgs(ND);
  S.TemplateSpecializationArgs = TemplateSpecializationArgs;

  // We collect main-file symbols, but do not use them for code completion.
  if (!IsMainFileOnly && isIndexedForCodeCompletion(ND, Ctx))
    S.Flags |= Symbol::IndexedForCodeCompletion;
  if (isImplementationDetail(&ND))
    S.Flags |= Symbol::ImplementationDetail;
  if (!IsMainFileOnly)
    S.Flags |= Symbol::VisibleOutsideFile;
  S.SymInfo = index::getSymbolInfo(&ND);
  std::string FileURI;
  auto Loc = spellingLocIfSpelled(findName(&ND), SM);
  assert(Loc.isValid() && "Invalid source location for NamedDecl");
  // FIXME: use the result to filter out symbols.
  shouldIndexFile(SM.getFileID(Loc));
  if (auto DeclLoc =
          getTokenLocation(Loc, SM, Opts, ASTCtx->getLangOpts(), FileURI))
    S.CanonicalDeclaration = *DeclLoc;

  S.Origin = Opts.Origin;
  if (ND.getAvailability() == AR_Deprecated)
    S.Flags |= Symbol::Deprecated;

  // Add completion info.
  // FIXME: we may want to choose a different redecl, or combine from several.
  assert(ASTCtx && PP.get() && "ASTContext and Preprocessor must be set.");
  // We use the primary template, as clang does during code completion.
  CodeCompletionResult SymbolCompletion(&getTemplateOrThis(ND), 0);
  const auto *CCS = SymbolCompletion.CreateCodeCompletionString(
      *ASTCtx, *PP, CodeCompletionContext::CCC_Symbol, *CompletionAllocator,
      *CompletionTUInfo,
      /*IncludeBriefComments*/ false);
  std::string Documentation =
      formatDocumentation(*CCS, getDocComment(Ctx, SymbolCompletion,
                                              /*CommentsFromHeaders=*/true));
  if (!(S.Flags & Symbol::IndexedForCodeCompletion)) {
    if (Opts.StoreAllDocumentation)
      S.Documentation = Documentation;
    Symbols.insert(S);
    return Symbols.find(S.ID);
  }
  S.Documentation = Documentation;
  std::string Signature;
  std::string SnippetSuffix;
  getSignature(*CCS, &Signature, &SnippetSuffix);
  S.Signature = Signature;
  S.CompletionSnippetSuffix = SnippetSuffix;
  std::string ReturnType = getReturnType(*CCS);
  S.ReturnType = ReturnType;

  llvm::Optional<OpaqueType> TypeStorage;
  if (S.Flags & Symbol::IndexedForCodeCompletion) {
    TypeStorage = OpaqueType::fromCompletionResult(*ASTCtx, SymbolCompletion);
    if (TypeStorage)
      S.Type = TypeStorage->raw();
  }

  Symbols.insert(S);
  setIncludeLocation(S, ND.getLocation());
  return Symbols.find(S.ID);
}

void SymbolCollector::addDefinition(const NamedDecl &ND,
                                    const Symbol &DeclSym) {
  if (DeclSym.Definition)
    return;
  // If we saw some forward declaration, we end up copying the symbol.
  // This is not ideal, but avoids duplicating the "is this a definition" check
  // in clang::index. We should only see one definition.
  Symbol S = DeclSym;
  std::string FileURI;
  const auto &SM = ND.getASTContext().getSourceManager();
  auto Loc = spellingLocIfSpelled(findName(&ND), SM);
  // FIXME: use the result to filter out symbols.
  shouldIndexFile(SM.getFileID(Loc));
  if (auto DefLoc =
          getTokenLocation(Loc, SM, Opts, ASTCtx->getLangOpts(), FileURI))
    S.Definition = *DefLoc;
  Symbols.insert(S);
}

/// Gets a canonical include (URI of the header or <header> or "header") for
/// header of \p FID (which should usually be the *expansion* file).
/// Returns None if includes should not be inserted for this file.
llvm::Optional<std::string>
SymbolCollector::getIncludeHeader(llvm::StringRef QName, FileID FID) {
  const SourceManager &SM = ASTCtx->getSourceManager();
  const FileEntry *FE = SM.getFileEntryForID(FID);
  if (!FE || FE->getName().empty())
    return llvm::None;
  llvm::StringRef Filename = FE->getName();
  // If a file is mapped by canonical headers, use that mapping, regardless
  // of whether it's an otherwise-good header (header guards etc).
  if (Opts.Includes) {
    llvm::StringRef Canonical = Opts.Includes->mapHeader(Filename, QName);
    // If we had a mapping, always use it.
    if (Canonical.startswith("<") || Canonical.startswith("\""))
      return Canonical.str();
    if (Canonical != Filename)
      return toURI(SM, Canonical, Opts);
  }
  if (!isSelfContainedHeader(FID)) {
    // A .inc or .def file is often included into a real header to define
    // symbols (e.g. LLVM tablegen files).
    if (Filename.endswith(".inc") || Filename.endswith(".def"))
      return getIncludeHeader(QName, SM.getFileID(SM.getIncludeLoc(FID)));
    // Conservatively refuse to insert #includes to files without guards.
    return llvm::None;
  }
  // Standard case: just insert the file itself.
  return toURI(SM, Filename, Opts);
}

bool SymbolCollector::isSelfContainedHeader(FileID FID) {
  // The real computation (which will be memoized).
  auto Compute = [&] {
    const SourceManager &SM = ASTCtx->getSourceManager();
    const FileEntry *FE = SM.getFileEntryForID(FID);
    if (!FE)
      return false;
    if (!PP->getHeaderSearchInfo().isFileMultipleIncludeGuarded(FE))
      return false;
    // This pattern indicates that a header can't be used without
    // particular preprocessor state, usually set up by another header.
    if (isDontIncludeMeHeader(SM.getBufferData(FID)))
      return false;
    return true;
  };

  auto R = HeaderIsSelfContainedCache.try_emplace(FID, false);
  if (R.second)
    R.first->second = Compute();
  return R.first->second;
}

// Is Line an #if or #ifdef directive?
static bool isIf(llvm::StringRef Line) {
  Line = Line.ltrim();
  if (!Line.consume_front("#"))
    return false;
  Line = Line.ltrim();
  return Line.startswith("if");
}
// Is Line an #error directive mentioning includes?
static bool isErrorAboutInclude(llvm::StringRef Line) {
  Line = Line.ltrim();
  if (!Line.consume_front("#"))
    return false;
  Line = Line.ltrim();
  if (!Line.startswith("error"))
    return false;
  return Line.contains_lower("includ"); // Matches "include" or "including".
}

bool SymbolCollector::isDontIncludeMeHeader(llvm::StringRef Content) {
  llvm::StringRef Line;
  // Only sniff up to 100 lines or 10KB.
  Content = Content.take_front(100 * 100);
  for (unsigned I = 0; I < 100 && !Content.empty(); ++I) {
    std::tie(Line, Content) = Content.split('\n');
    if (isIf(Line) && isErrorAboutInclude(Content.split('\n').first))
      return true;
  }
  return false;
}

bool SymbolCollector::shouldIndexFile(FileID FID) {
  if (!Opts.FileFilter)
    return true;
  auto I = FilesToIndexCache.try_emplace(FID);
  if (I.second)
    I.first->second = Opts.FileFilter(ASTCtx->getSourceManager(), FID);
  return I.first->second;
}

} // namespace clangd
} // namespace clang

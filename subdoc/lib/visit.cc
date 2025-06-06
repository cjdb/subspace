// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "subdoc/lib/visit.h"

#include <regex>

#include "subdoc/lib/database.h"
#include "subdoc/lib/doc_attributes.h"
#include "subdoc/lib/parse_comment.h"
#include "subdoc/lib/path.h"
#include "subdoc/lib/record_type.h"
#include "subdoc/lib/requires.h"
#include "subdoc/lib/stmt_to_string.h"
#include "subdoc/lib/type.h"
#include "subdoc/lib/unique_symbol.h"
#include "sus/assertions/check.h"
#include "sus/assertions/unreachable.h"
#include "sus/cmp/ord.h"
#include "sus/iter/compat_ranges.h"
#include "sus/iter/iterator.h"
#include "sus/iter/once.h"
#include "sus/mem/swap.h"
#include "sus/prelude.h"

namespace subdoc {

struct DiagnosticIds {
  static DiagnosticIds with_context(clang::ASTContext& ast_cx) noexcept {
    return DiagnosticIds{
        .superceded_comment = ast_cx.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Error,
            "ignored API comment, superceded by comment at %0"),
        .malformed_comment = ast_cx.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Error, "malformed API comment: %0"),
        .misplaced_comment = ast_cx.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Error, "API comment will be ignored, %0"),
    };
  }

  const unsigned int superceded_comment;
  const unsigned int malformed_comment;
  const unsigned int misplaced_comment;
};

static bool should_skip_decl(VisitCx& cx, clang::Decl* decl) {
  auto* ndecl = clang::dyn_cast<clang::NamedDecl>(decl);
  if (!ndecl) {
    return true;
  }

  clang::DeclContext* context = decl->getDeclContext();
  while (clang::isa<clang::LinkageSpecDecl>(context))
    context = context->getParent();
  if (!(clang::isa<clang::RecordDecl>(context) ||
        clang::isa<clang::NamespaceDecl>(context) ||
        clang::isa<clang::TranslationUnitDecl>(context))) {
    // Skip decls in function bodies.
    return true;
  }

  // TODO: These could be configurable. As well as user-defined namespaces to
  // skip.
  if (path_contains_namespace(ndecl,
                              Namespace::with<Namespace::Tag::Anonymous>())) {
    return true;
  }
  // TODO: Make this configurable on the command line.
  if (path_contains_namespace(
          ndecl, Namespace::with<Namespace::Tag::Named>("__private"))) {
    return true;
  }
  // TODO: Make this configurable on the command line.
  if (path_contains_namespace(ndecl,
                              Namespace::with<Namespace::Tag::Named>("test"))) {
    return true;
  }
  if (path_is_private(ndecl)) {
    return true;
  }
  if (!cx.should_include_decl_based_on_file(decl)) {
    return true;
  }
  return false;
}

static clang::RawComment* get_raw_comment(const clang::Decl* decl) noexcept {
  return decl->getASTContext().getRawCommentForDeclNoCache(decl);
}

static Vec<std::string> collect_template_params(
    const clang::TemplateDecl* tmpl,
    clang::Preprocessor& preprocessor) noexcept {
  Vec<std::string> template_params;
  if (clang::TemplateParameterList* params = tmpl->getTemplateParameters()) {
    for (clang::NamedDecl* n : *params) {
      // Skip auto vars from the function parameter list, which get added as
      // auto template parameters.
      if (n->isImplicit()) continue;

      // TODO: Get the default values and the type (auto vs class).
      if (auto* parm = clang::dyn_cast<clang::TemplateTypeParmDecl>(n)) {
        std::stringstream s;
        s << "class";
        if (parm->isParameterPack()) s << "...";
        s << " ";
        s << parm->getNameAsString();
        auto get_default_arg_info = [](clang::TemplateTypeParmDecl* parm) {
#if CLANG_VERSION_MAJOR >= 19
          return parm->getDefaultArgument().getTypeSourceInfo();
#else
          return parm->getDefaultArgumentInfo();
#endif
        };
        if (clang::TypeSourceInfo* def = get_default_arg_info(parm)) {
          s << " = ";
          s << def->getType().getAsString();
        }
        template_params.push(sus::move(s).str());
      } else if (auto* val =
                     clang::dyn_cast<clang::NonTypeTemplateParmDecl>(n)) {
        std::stringstream s;
        s << val->getType().getAsString();
        if (val->isParameterPack()) s << "...";
        s << " ";
        s << val->getNameAsString();
        auto get_default_arg_expr = [](clang::NonTypeTemplateParmDecl* val) {
#if CLANG_VERSION_MAJOR >= 19
          const clang::TemplateArgument& arg =
              val->getDefaultArgument().getArgument();
          return !arg.isNull() ? arg.getAsExpr() : nullptr;
#else
          return val->getDefaultArgument();
#endif
        };
        if (clang::Expr* e = get_default_arg_expr(val)) {
          s << " = ";
          // TODO: There can be types in here that need to be resolved,
          // and can be linked to database entries.
          s << stmt_to_string(*e, val->getASTContext().getSourceManager(),
                              preprocessor);
        }
        template_params.push(sus::move(s).str());
      } else {
        fmt::println(
            stderr, "WARNING: Unknown TemplateParameterList member on Record:");
        n->dumpColor();
      }
    }
  }
  return template_params;
}

static Option<RequiresConstraints> collect_template_constraints(
    const clang::TemplateDecl* tmpl,
    clang::Preprocessor& preprocessor) noexcept {
  Option<RequiresConstraints> constraints;
  llvm::SmallVector<const clang::Expr*> c_exprs;
  tmpl->getAssociatedConstraints(c_exprs);
  for (const clang::Expr* e : c_exprs) {
    requires_constraints_add_expr(constraints.get_or_insert_default(),
                                  tmpl->getASTContext(), preprocessor, e);
  }
  return constraints;
}

static Option<RequiresConstraints> collect_function_constraints(
    const clang::FunctionDecl* decl,
    clang::Preprocessor& preprocessor) noexcept {
  Option<RequiresConstraints> constraints;
  llvm::SmallVector<const clang::Expr*> assoc;
  decl->getAssociatedConstraints(assoc);
  for (const clang::Expr* e : assoc) {
    requires_constraints_add_expr(constraints.get_or_insert_default(),
                                  decl->getASTContext(), preprocessor, e);
  }
  return constraints;
}

class Visitor : public clang::RecursiveASTVisitor<Visitor> {
 public:
  Visitor(VisitCx& cx, Database& docs_db, clang::Preprocessor& preprocessor,
          DiagnosticIds ids)
      : cx_(cx),
        docs_db_(docs_db),
        preprocessor_(preprocessor),
        diag_ids_(sus::move(ids)) {}
  bool shouldVisitLambdaBody() const { return false; }

  bool VisitMacros(clang::Decl* decl) {
    clang::SourceManager& sm = decl->getASTContext().getSourceManager();
    auto& ast_comments = decl->getASTContext().Comments;

    // Visit macros.
    for (auto& [identifier_info, macro_state] : preprocessor_.macros()) {
      auto name = std::string(identifier_info->getName());

      if (!cx_.options.macro_prefixes.iter().any(
              [&](const auto& prefix) { return name.starts_with(prefix); })) {
        continue;
      }

      // Get all comments in the file the macro definition was from.
      clang::MacroDefinition defn =
          preprocessor_.getMacroDefinition(identifier_info);
      clang::MacroInfo* info = defn.getMacroInfo();
      if (info == nullptr) continue;
      const std::pair<clang::FileID, u32> decomposed_loc =
          sm.getDecomposedLoc(info->getDefinitionLoc());
      const clang::FileID file = decomposed_loc.first;
      if (file.isInvalid()) continue;

      const std::map<unsigned, clang::RawComment*>* comments_in_file =
          ast_comments.getCommentsInFile(file);
      if (comments_in_file == nullptr) continue;

      const u32 macro_start = decomposed_loc.second;

      // Find the nearest comment above the macro definition.
      clang::RawComment* raw_comment = [&]() -> clang::RawComment* {
        if (comments_in_file->empty()) return nullptr;

        auto it = comments_in_file->lower_bound(macro_start);
        if (it == comments_in_file->begin()) return nullptr;
        --it;
        clang::RawComment* raw_comment = it->second;

        // The comment should be docs and be above the macro.
        if (!raw_comment->isDocumentation()) return nullptr;
        if (raw_comment->isTrailingComment()) return nullptr;

        // Now we know the offset into the file for the end of the macro and the
        // start of the comment.
        const u32 comment_end = ast_comments.getCommentEndOffset(raw_comment);

        // Make sure the comment is actually against the macro. Read the
        // original buffer.
        bool invalid = false;
        const char* buffer = sm.getBufferData(file, &invalid).data();
        if (invalid) return nullptr;
        llvm::StringRef between(buffer + comment_end,
                                macro_start - comment_end);

        // Drop the #define for this macro.
        while (between.ends_with(" "))
          between = between.substr(0u, between.size() - 1u);
        if (between.ends_with("define"))
          between = between.substr(0u, between.size() - strlen("define"));
        while (between.ends_with(" "))
          between = between.substr(0u, between.size() - 1u);
        if (between.ends_with("#"))
          between = between.substr(0u, between.size() - strlen("#"));

        // There should be no other declarations or macros between the comment and
        // this macro. This pattern is copied from
        // clang::ASTContext::getRawCommentForDeclNoCacheImpl().
        if (between.find_last_of(";{}#@") != llvm::StringRef::npos) {
          return nullptr;
        }
        return raw_comment;
      }();

      auto params = Option<Vec<std::string>>();
      if (info->isFunctionLike()) {
        params = sus::some(sus::iter::from_range(info->params())
                               .map([](const clang::IdentifierInfo* p) {
                                 // A `...` comes out with the __VA_ARGS__ name
                                 // here.
                                 if (p->getName() == "__VA_ARGS__")
                                   return std::string("...");
                                 else
                                   return std::string(p->getName());
                               })
                               .collect<Vec<std::string>>());
      }

      Comment comment = make_db_comment(decl->getASTContext(), raw_comment, "");
      auto key = MacroId(sus::clone(name));
      auto me = MacroElement(sus::move(comment), sus::move(name),
                             sus::move(params), macro_start);
      add_macro_to_db(sus::clone(key), sus::move(me), docs_db_.global.macros,
                      decl->getASTContext());
      add_source_link_to_db(docs_db_.global.macros.find(key)->second,
                            SourceLink::DefinitionLocation,
                            info->getDefinitionLoc(), info->getDefinitionLoc(),
                            decl->getASTContext());
    }
    return true;
  }

  bool VisitStaticAssertDecl(clang::StaticAssertDecl*) noexcept {
    // llvm::errs() << "StaticAssertDecl\n";
    return true;
  }

  bool VisitNamespaceDecl(clang::NamespaceDecl* decl) noexcept {
    if (should_skip_decl(cx_, decl)) return true;
    clang::RawComment* raw_comment = get_raw_comment(decl);

    Comment comment = make_db_comment(decl->getASTContext(), raw_comment, "");
    auto ne =
        NamespaceElement(iter_namespace_path(decl).collect_vec(),
                         sus::move(comment), decl->getNameAsString(),
                         decl->getASTContext().getSourceManager().getFileOffset(
                             decl->getLocation()));
    NamespaceElement& parent = [&]() -> NamespaceElement& {
      clang::DeclContext* context = decl->getDeclContext();
      // TODO: Save the linkage spec (`extern "C"`) so we can show it.
      while (clang::isa<clang::LinkageSpecDecl>(context))
        context = context->getParent();

      if (clang::isa<clang::TranslationUnitDecl>(context)) {
        return docs_db_.find_namespace_mut(nullptr).unwrap();
      } else {
        return docs_db_
            .find_namespace_mut(clang::cast<clang::NamespaceDecl>(context))
            .unwrap();
      }
    }();
    NamespaceId key = key_for_namespace(decl);
    add_namespace_to_db(sus::clone(key), sus::move(ne), parent.namespaces,
                        decl->getASTContext());
    add_source_link_to_db(
        parent.namespaces.find(key)->second,
        [&] {
          if (raw_comment) return SourceLink::CommentLocation;
          return SourceLink::UnknownLocation;
        }(),
        decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
    return true;
  }

  bool VisitRecordDecl(clang::RecordDecl* decl) noexcept {
    if (!decl->getDefinition()) return true;
    clang::CXXRecordDecl* cxxdecl = clang::dyn_cast<clang::CXXRecordDecl>(decl);
    if (cxxdecl && cxxdecl->isLocalClass()) return true;  // Inside a function.
    if (should_skip_decl(cx_, decl)) return true;
    clang::RawComment* raw_comment = get_raw_comment(decl);

    RecordType record_type = [&]() {
      if (decl->isStruct()) return RecordType::Struct;
      if (decl->isUnion()) return RecordType::Union;
      return RecordType::Class;
    }();

    clang::RecordDecl* parent_record_decl =
        clang::dyn_cast<clang::RecordDecl>(decl->getDeclContext());

    Vec<std::string> template_params;
    Option<RequiresConstraints> constraints;
    if (cxxdecl) {
      if (clang::ClassTemplateDecl* tmpl =
              cxxdecl->getDescribedClassTemplate()) {
        template_params = collect_template_params(tmpl, preprocessor_);
        constraints = collect_template_constraints(tmpl, preprocessor_);
      }
    }

    std::string name = [&]() {
      if (auto* t = decl->getTypedefNameForAnonDecl())
        return t->getNameAsString();
      else
        return decl->getNameAsString();
    }();

    Comment comment =
        make_db_comment(decl->getASTContext(), raw_comment, decl->getName());
    auto re = RecordElement(
        iter_namespace_path(decl).collect_vec(), sus::move(comment),
        sus::move(name),
        iter_record_path(parent_record_decl)
            .map([](std::string_view&& v) { return std::string(v); })
            .collect_vec(),
        record_type, sus::move(constraints), sus::move(template_params),
        decl->getDefinition()->hasAttr<clang::FinalAttr>(),
        decl->getASTContext().getSourceManager().getFileOffset(
            decl->getLocation()));

    clang::DeclContext* context = decl->getDeclContext();
    // TODO: Save the linkage spec (`extern "C"`) so we can show it.
    while (clang::isa<clang::LinkageSpecDecl>(context))
      context = context->getParent();

    if (clang::isa<clang::TranslationUnitDecl>(context)) {
      NamespaceElement& parent = docs_db_.find_namespace_mut(nullptr).unwrap();
      auto key = RecordId(*decl);
      add_record_to_db(sus::clone(key), sus::move(re), parent.records,
                       decl->getASTContext());
      add_source_link_to_db(parent.records.find(key)->second,
                            // Only definitions are visited.
                            SourceLink::DefinitionLocation, decl->getLocation(),
                            decl->getBeginLoc(), decl->getASTContext());
    } else if (clang::isa<clang::NamespaceDecl>(context)) {
      auto* namespace_decl = clang::cast<clang::NamespaceDecl>(context);
      // Template specializations can be for classes that are part of a
      // namespace we never recorded as the files were excluded. e.g.
      // ```
      // template <>
      // struct fmt::formatter<MyType, char> {};
      // ```
      if (should_skip_decl(cx_, namespace_decl)) {
        // TODO: Should we generate docs for such things?
        return true;
      }
      NamespaceElement& parent =
          docs_db_.find_namespace_mut(namespace_decl)
              .expect(
                  "No parent namespace found in db for NamespaceDecl context");
      auto key = RecordId(*decl);
      add_record_to_db(sus::clone(key), sus::move(re), parent.records,
                       decl->getASTContext());
      add_source_link_to_db(parent.records.find(key)->second,
                            // Only definitions are visited.
                            SourceLink::DefinitionLocation, decl->getLocation(),
                            decl->getBeginLoc(), decl->getASTContext());
    } else {
      sus_check(clang::isa<clang::RecordDecl>(context));
      if (Option<RecordElement&> parent =
              docs_db_.find_record_mut(clang::cast<clang::RecordDecl>(context));
          parent.is_some()) {
        auto key = RecordId(*decl);
        add_record_to_db(sus::clone(key), sus::move(re), parent->records,
                         decl->getASTContext());
        add_source_link_to_db(parent->records.find(key)->second,
                              // Only definitions are visited.
                              SourceLink::DefinitionLocation,
                              decl->getLocation(), decl->getBeginLoc(),
                              decl->getASTContext());
      }
    }
    return true;
  }

  bool VisitFieldDecl(clang::FieldDecl* decl) noexcept {
    if (should_skip_decl(cx_, decl)) return true;
    clang::RawComment* raw_comment = get_raw_comment(decl);

    clang::RecordDecl* record_decl =
        clang::cast<clang::RecordDecl>(decl->getDeclContext());

    Comment comment = make_db_comment(decl->getASTContext(), raw_comment,
                                      record_decl->getName());

    auto linked_type = LinkedType::with_type(
        build_local_type(decl->getType(),
                         decl->getASTContext(),
                         preprocessor_, decl->getBeginLoc()),
        docs_db_);

    auto fe = FieldElement(
        iter_namespace_path(decl).collect_vec(), sus::move(comment),
        std::string(decl->getName()), sus::move(linked_type),
        iter_record_path(record_decl)
            .map([](std::string_view&& v) { return std::string(v); })
            .collect_vec(),
        // Static data members are found in VisitVarDecl.
        FieldElement::NonStatic,
        // Non-static fields can't have template parameters.
        sus::empty,
        // Non-static fields can't have constraits.
        sus::none(),
        decl->getASTContext().getSourceManager().getFileOffset(
            decl->getLocation()));

    if (Option<RecordElement&> parent = docs_db_.find_record_mut(record_decl);
        parent.is_some()) {
      add_field_to_db(unique_from_decl(decl), sus::move(fe), parent->fields,
                      decl->getASTContext());
      add_source_link_to_db(
          parent->fields.find(unique_from_decl(decl))->second,
          [&] {
            if (raw_comment) return SourceLink::CommentLocation;
            return SourceLink::UnknownLocation;
          }(),
          decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
    }
    return true;
  }

  bool VisitVarDecl(clang::VarDecl* decl) noexcept {
    // Static data members are represented as VarDecl, not FieldDecl.
    if (should_skip_decl(cx_, decl)) return true;

    auto* context = decl->getDeclContext();
    // TODO: Save the linkage spec (`extern "C"`) so we can show it.
    while (clang::isa<clang::LinkageSpecDecl>(context))
      context = context->getParent();

    // We only visit static data members, so the context is a record.
    std::string self_name;
    if (auto* record_ctx = clang::dyn_cast<clang::RecordDecl>(context)) {
      self_name = record_ctx->getName();
    }

    clang::RawComment* raw_comment = get_raw_comment(decl);
    Comment comment = make_db_comment(decl->getASTContext(), raw_comment,
                                      sus::move(self_name));

    Vec<std::string> template_params;
    Option<RequiresConstraints> constraints;
    if (clang::VarTemplateDecl* tmpl = decl->getDescribedVarTemplate()) {
      template_params = collect_template_params(tmpl, preprocessor_);
      constraints = collect_template_constraints(tmpl, preprocessor_);
    }

    // If a variable is written as `auto x = Foo()` we want to get the deduced
    // `Foo` type. We don't do this generally for all `auto` types.
    clang::QualType type = decl->getType();
    if (auto* auto_type = clang::dyn_cast<clang::AutoType>(&*type);
        auto_type && auto_type->isDeduced()) {
      type = type.getDesugaredType(decl->getASTContext());
    }

    auto linked_type = LinkedType::with_type(
        build_local_type(type, decl->getASTContext(),
                         preprocessor_, decl->getBeginLoc()),
        docs_db_);

    Vec<std::string> record_path;
    if (auto* record_ctx = clang::dyn_cast<clang::RecordDecl>(context)) {
      record_path =
          iter_record_path(record_ctx)
              .map([](std::string_view&& v) { return std::string(v); })
              .collect_vec();
    }

    auto fe =
        FieldElement(iter_namespace_path(decl).collect_vec(),
                     sus::move(comment), std::string(decl->getName()),
                     sus::move(linked_type), sus::move(record_path),
                     // NonStatic data members are found in VisitFieldDecl.
                     FieldElement::Static, sus::move(template_params),
                     sus::move(constraints),
                     decl->getASTContext().getSourceManager().getFileOffset(
                         decl->getLocation()));

    SourceLink::Quality source_link_quality = [&] {
      if (decl->getDefinition() == decl) {
        if (raw_comment) return SourceLink::CommentAndDefinitionLocation;
        return SourceLink::DefinitionLocation;
      }
      if (raw_comment) return SourceLink::CommentLocation;
      return SourceLink::UnknownLocation;
    }();

    if (clang::isa<clang::RecordDecl>(context)) {
      if (Option<RecordElement&> parent =
              docs_db_.find_record_mut(clang::cast<clang::RecordDecl>(context));
          parent.is_some()) {
        add_field_to_db(unique_from_decl(decl), sus::move(fe), parent->fields,
                        decl->getASTContext());
        add_source_link_to_db(
            parent->fields.find(unique_from_decl(decl))->second,
            source_link_quality, decl->getLocation(), decl->getBeginLoc(),
            decl->getASTContext());
      }
    } else if (clang::isa<clang::NamespaceDecl>(context)) {
      if (Option<NamespaceElement&> parent = docs_db_.find_namespace_mut(
              clang::cast<clang::NamespaceDecl>(context));
          parent.is_some()) {
        add_field_to_db(unique_from_decl(decl), sus::move(fe),
                        parent->variables, decl->getASTContext());
        add_source_link_to_db(
            parent->variables.find(unique_from_decl(decl))->second,
            source_link_quality, decl->getLocation(), decl->getBeginLoc(),
            decl->getASTContext());
      }
    } else if (clang::isa<clang::TranslationUnitDecl>(context)) {
      NamespaceElement& parent = docs_db_.find_namespace_mut(nullptr).unwrap();
      add_field_to_db(unique_from_decl(decl), sus::move(fe), parent.variables,
                      decl->getASTContext());
      add_source_link_to_db(
          parent.variables.find(unique_from_decl(decl))->second,
          source_link_quality, decl->getLocation(), decl->getBeginLoc(),
          decl->getASTContext());
    }
    return true;
  }

  bool VisitEnumDecl(clang::EnumDecl* decl) noexcept {
    if (should_skip_decl(cx_, decl)) return true;
    // clang::RawComment* raw_comment = get_raw_comment(decl);
    // if (raw_comment)
    //   llvm::errs() << "EnumDecl " << raw_comment->getKind() << "\n";
    return true;
  }

  bool VisitTypedefDecl(clang::TypedefDecl* decl) noexcept {
    return VisitTypeAliasDecl(decl);
  }

  /// Received TypedefNameDecl as it handles TypeAliasDecl and TypedefDecl.
  bool VisitTypeAliasDecl(clang::TypedefNameDecl* decl) noexcept {
    if (should_skip_decl(cx_, decl)) return true;

    clang::RawComment* raw_comment = get_raw_comment(decl);
    Comment comment = make_db_comment(decl->getASTContext(), raw_comment, "");

    auto* context = decl->getDeclContext();
    // TODO: Save the linkage spec (`extern "C"`) so we can show it.
    while (clang::isa<clang::LinkageSpecDecl>(context))
      context = context->getParent();

    Option<RequiresConstraints> constraints;
    if (auto* alias_decl = clang::dyn_cast<clang::TypeAliasDecl>(decl)) {
      if (auto* tmpl = alias_decl->getDescribedAliasTemplate()) {
        constraints = collect_template_constraints(tmpl, preprocessor_);
      }
    }

    auto te = AliasElement(
        iter_namespace_path(decl).collect_vec(), sus::move(comment),
        decl->getNameAsString(),
        decl->getASTContext().getSourceManager().getFileOffset(
            decl->getLocation()),
        iter_record_path(context)
            .map([](std::string_view v) { return std::string(v); })
            .collect_vec(),
        AliasStyle::NewType, sus::move(constraints),
        AliasTarget::with<AliasOfType>(LinkedType::with_type(
            build_local_type(decl->getUnderlyingType(),
                             decl->getASTContext(),
                             preprocessor_, decl->getBeginLoc()),
            docs_db_)));

    if (auto* rec_ctx = clang::dyn_cast<clang::RecordDecl>(context)) {
      if (Option<RecordElement&> parent = docs_db_.find_record_mut(rec_ctx);
          parent.is_some()) {
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent->aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent->aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      }
    } else if (auto* ns_ctx = clang::dyn_cast<clang::NamespaceDecl>(context)) {
      if (Option<NamespaceElement&> parent =
              docs_db_.find_namespace_mut(ns_ctx);
          parent.is_some()) {
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent->aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent->aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      }
    } else {
      sus_check(clang::isa<clang::TranslationUnitDecl>(context));
      NamespaceElement& parent = docs_db_.find_namespace_mut(nullptr).unwrap();
      auto key = AliasId(decl->getNameAsString());
      add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                      decl->getASTContext());
      add_source_link_to_db(
          parent.aliases.find(key)->second,
          [&] {
            if (raw_comment) return SourceLink::CommentLocation;
            return SourceLink::UnknownLocation;
          }(),
          decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
    }
    return true;
  }

  bool VisitUsingEnumDecl(clang::UsingEnumDecl* decl) noexcept {
    clang::RawComment* raw_comment = get_raw_comment(decl);
    if (raw_comment) {
      // `using enum` actually brings in each element of the enum, so a comment
      // on the using declaration will have nowhere to appear in the generated
      // docs.
      decl->getASTContext()
          .getDiagnostics()
          .Report(raw_comment->getBeginLoc(), diag_ids_.misplaced_comment)
          .AddString("`using enum` can not display a comment");
    }

    return VisitUsingDecl(decl);
  }

  bool VisitUsingDecl(clang::BaseUsingDecl* decl) noexcept {
    if (should_skip_decl(cx_, decl)) return true;
    clang::RawComment* raw_comment = get_raw_comment(decl);
    // TODO: The comment should be inherited from the target of the using decl.
    Comment comment = make_db_comment(decl->getASTContext(), raw_comment, "");
    // `using enum` can pull in more than one shadow, it pulls in every constant
    // in the enum.
    for (clang::UsingShadowDecl* shadow : decl->shadows()) {
      Vec<Namespace> target_namespace_path =
          iter_namespace_path(shadow).collect_vec();
      Vec<std::string> target_record_path =
          iter_record_path(shadow->getDeclContext())
              .map([](std::string_view v) { return std::string(v); })
              .collect_vec();

      if (auto* tag =
              clang::dyn_cast<clang::TagDecl>(shadow->getTargetDecl())) {
        auto* context =
            clang::dyn_cast<clang::NamespaceDecl>(decl->getDeclContext());
        if (!context &&
            !clang::isa<clang::TranslationUnitDecl>(decl->getDeclContext())) {
          // The context for a using record/enum is a namespace or translation
          // unit.
          decl->dump();
          decl->getDeclContext()->dumpAsDecl();
          sus::unreachable();
        }

        auto te = AliasElement(
            iter_namespace_path(decl).collect_vec(), sus::move(comment),
            decl->getNameAsString(),
            decl->getASTContext().getSourceManager().getFileOffset(
                decl->getLocation()),
            sus::empty,  // No record path.
            AliasStyle::Forwarding,
            sus::none(),  // No constraints.
            AliasTarget::with<AliasOfType>(LinkedType::with_type(
                build_local_type(clang::QualType(tag->getTypeForDecl(), 0u),
                                 tag->getASTContext(),
                                 preprocessor_, tag->getBeginLoc()),
                docs_db_)));
        NamespaceElement& parent =
            docs_db_.find_namespace_mut(context).unwrap();
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent.aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      } else if (auto* classtmpl = clang::dyn_cast<clang::ClassTemplateDecl>(
                     shadow->getTargetDecl())) {
        auto* context =
            clang::dyn_cast<clang::NamespaceDecl>(decl->getDeclContext());
        if (!context &&
            !clang::isa<clang::TranslationUnitDecl>(decl->getDeclContext())) {
          // The context for a using record/enum is a namespace or translation
          // unit.
          decl->dump();
          decl->getDeclContext()->dumpAsDecl();
          sus::unreachable();
        }

        auto te = AliasElement(
            iter_namespace_path(decl).collect_vec(), sus::move(comment),
            decl->getNameAsString(),
            decl->getASTContext().getSourceManager().getFileOffset(
                decl->getLocation()),
            sus::empty,  // No record path.
            AliasStyle::Forwarding,
            sus::none(),  // No constraints.
            AliasTarget::with<AliasOfType>(LinkedType::with_type(
                build_local_type(
                    clang::QualType(
                        classtmpl->getTemplatedDecl()->getTypeForDecl(), 0u),
                    classtmpl->getASTContext(),
                    preprocessor_, classtmpl->getBeginLoc()),
                docs_db_)));
        NamespaceElement& parent =
            docs_db_.find_namespace_mut(context).unwrap();
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent.aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      } else if (auto* condecl = clang::dyn_cast<clang::ConceptDecl>(
                     shadow->getTargetDecl())) {
        auto* context =
            clang::dyn_cast<clang::NamespaceDecl>(decl->getDeclContext());
        if (!context &&
            !clang::isa<clang::TranslationUnitDecl>(decl->getDeclContext())) {
          // The context for a concept is a namespace or translation unit.
          decl->dump();
          decl->getDeclContext()->dumpAsDecl();
          sus::unreachable();
        }

        Vec<Namespace> target_namespaces =
            iter_namespace_path(condecl).collect_vec();
        std::string target_name = condecl->getNameAsString();

        auto te = AliasElement(
            iter_namespace_path(decl).collect_vec(), sus::move(comment),
            decl->getNameAsString(),
            decl->getASTContext().getSourceManager().getFileOffset(
                decl->getLocation()),
            sus::empty,  // No record path.
            AliasStyle::Forwarding,
            sus::none(),  // No constraints.
            AliasTarget::with<AliasOfConcept>(LinkedConcept::with_concept(
                target_namespaces, sus::move(target_name), docs_db_)));
        NamespaceElement& parent =
            docs_db_.find_namespace_mut(context).unwrap();
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent.aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      } else if (auto* ecdecl = clang::dyn_cast<clang::EnumConstantDecl>(
                     shadow->getTargetDecl())) {
        ecdecl->dump();
        decl->dump();
        decl->getBeginLoc().dump(decl->getASTContext().getSourceManager());
        // TODO: Put these into static fields on a record, and const global
        // variables on a namespace.
        //sus::unreachable();
      } else if (auto* vardecl =
                     clang::dyn_cast<clang::VarDecl>(shadow->getTargetDecl())) {
        auto* context =
            clang::dyn_cast<clang::NamespaceDecl>(decl->getDeclContext());
        if (!context &&
            !clang::isa<clang::TranslationUnitDecl>(decl->getDeclContext())) {
          // The context for a variable is a namespace or translation unit. You
          // can't write an alias to a static class data member.
          decl->dump();
          decl->getDeclContext()->dumpAsDecl();
          sus::unreachable();
        }

        Vec<Namespace> target_namespaces =
            iter_namespace_path(vardecl).collect_vec();
        std::string target_name = vardecl->getNameAsString();

        auto te = AliasElement(
            iter_namespace_path(decl).collect_vec(), sus::move(comment),
            decl->getNameAsString(),
            decl->getASTContext().getSourceManager().getFileOffset(
                decl->getLocation()),
            sus::empty,  // No record path.
            AliasStyle::Forwarding,
            sus::none(),  // No constraints.
            AliasTarget::with<AliasOfVariable>(LinkedVariable::with_variable(
                target_namespaces, sus::move(target_name), docs_db_)));
        NamespaceElement& parent =
            docs_db_.find_namespace_mut(context).unwrap();
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent.aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      } else if (auto* method = clang::dyn_cast<clang::CXXMethodDecl>(
                     shadow->getTargetDecl())) {
        auto* context =
            clang::dyn_cast<clang::RecordDecl>(decl->getDeclContext());
        if (!context) {
          // The context for a using method is a record.
          decl->dump();
          decl->getDeclContext()->dumpAsDecl();
          sus::unreachable();
        }

        auto te = AliasElement(
            iter_namespace_path(decl).collect_vec(), sus::move(comment),
            decl->getNameAsString(),
            decl->getASTContext().getSourceManager().getFileOffset(
                decl->getLocation()),
            iter_record_path(context)
                .map([](std::string_view v) { return std::string(v); })
                .collect_vec(),
            AliasStyle::Forwarding,
            sus::none(),  // No constraints.
            AliasTarget::with<AliasOfMethod>(
                // TODO: This should store a LinkedFunction instead of a
                // LinkedType + string. But on failure to find something in the
                // db we need enough info to write out the whole thing, so maybe
                // a LinkedMethod so that we have a record path in there.
                LinkedType::with_type(
                    build_local_type(
                        clang::QualType(context->getTypeForDecl(), 0u),
                        context->getASTContext(),
                        preprocessor_, context->getBeginLoc()),
                    docs_db_),
                method->getNameAsString()));
        RecordElement& parent = docs_db_.find_record_mut(context).unwrap();
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent.aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      } else if (auto* funcdecl = clang::dyn_cast<clang::FunctionDecl>(
                     shadow->getTargetDecl())) {
        auto* context =
            clang::dyn_cast<clang::NamespaceDecl>(decl->getDeclContext());
        if (!context &&
            !clang::isa<clang::TranslationUnitDecl>(decl->getDeclContext())) {
          // The context for a function is a namespace or translation unit.
          decl->dump();
          decl->getDeclContext()->dumpAsDecl();
          sus::unreachable();
        }

        Vec<Namespace> target_namespaces =
            iter_namespace_path(funcdecl).collect_vec();
        std::string target_name = funcdecl->getNameAsString();

        auto te = AliasElement(
            iter_namespace_path(decl).collect_vec(), sus::move(comment),
            decl->getNameAsString(),
            decl->getASTContext().getSourceManager().getFileOffset(
                decl->getLocation()),
            sus::empty,  // No record path.
            AliasStyle::Forwarding,
            sus::none(),  // No constraints.
            AliasTarget::with<AliasOfFunction>(LinkedFunction::with_function(
                target_namespaces.as_slice(), sus::move(target_name),
                docs_db_)));
        NamespaceElement& parent =
            docs_db_.find_namespace_mut(context).unwrap();
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent.aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      } else if (auto* functmpldecl =
                     clang::dyn_cast<clang::FunctionTemplateDecl>(
                         shadow->getTargetDecl())) {
        auto* context =
            clang::dyn_cast<clang::NamespaceDecl>(decl->getDeclContext());
        if (!context &&
            !clang::isa<clang::TranslationUnitDecl>(decl->getDeclContext())) {
          // The context for a function is a namespace or translation unit.
          decl->dump();
          decl->getDeclContext()->dumpAsDecl();
          sus::unreachable();
        }

        Vec<Namespace> target_namespaces =
            iter_namespace_path(functmpldecl).collect_vec();
        std::string target_name = functmpldecl->getNameAsString();

        auto te = AliasElement(
            iter_namespace_path(decl).collect_vec(), sus::move(comment),
            decl->getNameAsString(),
            decl->getASTContext().getSourceManager().getFileOffset(
                decl->getLocation()),
            sus::empty,  // No record path.
            AliasStyle::Forwarding,
            sus::none(),  // No constraints.
            AliasTarget::with<AliasOfFunction>(LinkedFunction::with_function(
                target_namespaces.as_slice(), sus::move(target_name),
                docs_db_)));
        NamespaceElement& parent =
            docs_db_.find_namespace_mut(context).unwrap();
        auto key = AliasId(decl->getNameAsString());
        add_alias_to_db(sus::clone(key), sus::move(te), parent.aliases,
                        decl->getASTContext());
        add_source_link_to_db(
            parent.aliases.find(key)->second,
            [&] {
              if (raw_comment) return SourceLink::CommentLocation;
              return SourceLink::UnknownLocation;
            }(),
            decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
      } else {
        fmt::println(stderr, "Unknown shadow target in forwarding using decl");
        decl->dump();
        decl->getBeginLoc().dump(decl->getASTContext().getSourceManager());
        fmt::println(stderr, "");
        shadow->getTargetDecl()->dump();
        sus::unreachable();
      }
    }

    return true;
  }

  bool VisitConceptDecl(clang::ConceptDecl* decl) noexcept {
    if (should_skip_decl(cx_, decl)) return true;
    clang::RawComment* raw_comment = get_raw_comment(decl);

    Comment comment = make_db_comment(decl->getASTContext(), raw_comment, "");

    Vec<std::string> template_params;
    template_params = collect_template_params(decl, preprocessor_);

    RequiresConstraints constraints;
    requires_constraints_add_expr(constraints, decl->getASTContext(),
                                  preprocessor_, decl->getConstraintExpr());

    auto ce =
        ConceptElement(iter_namespace_path(decl).collect_vec(),
                       sus::move(comment), decl->getNameAsString(),
                       sus::move(template_params), sus::move(constraints),
                       decl->getASTContext().getSourceManager().getFileOffset(
                           decl->getLocation()));
    NamespaceElement& parent = [&]() -> NamespaceElement& {
      clang::DeclContext* context = decl->getDeclContext();
      if (clang::isa<clang::TranslationUnitDecl>(context)) {
        return docs_db_.find_namespace_mut(nullptr).unwrap();
      } else {
        return docs_db_
            .find_namespace_mut(clang::cast<clang::NamespaceDecl>(context))
            .unwrap();
      }
    }();
    ConceptId key = key_for_concept(decl);
    add_concept_to_db(sus::clone(key), sus::move(ce), parent.concepts,
                      decl->getASTContext());
    add_source_link_to_db(parent.concepts.find(key)->second,
                          // Concepts are always a definition.
                          SourceLink::DefinitionLocation, decl->getLocation(),
                          decl->getBeginLoc(), decl->getASTContext());
    return true;
  }

  bool VisitFunctionDecl(clang::FunctionDecl* decl) noexcept {
    // A template instantiation fills in concrete types for a templated
    // function. For documentation, we want to show the template at its
    // declaration, we are not interested in instantiations where it gets used.
    if (decl->isTemplateInstantiation()) return true;
    if (should_skip_decl(cx_, decl)) return true;
    /// Friend functions are handled in `VisitFriendDecl`.
    if (decl->getFriendObjectKind()) return true;

    // TODO: Save the linkage spec (`extern "C"`) so we can show it.
    clang::DeclContext* context = decl->getDeclContext();
    while (clang::isa<clang::LinkageSpecDecl>(context))
      context = context->getParent();

    auto map_and_self_name = [&]()
        -> Option<sus::Tuple<
            std::unordered_map<FunctionId, FunctionElement, FunctionId::Hash>&,
            std::string_view>> {
      if (clang::isa<clang::CXXConstructorDecl>(decl)) {
        sus_check(clang::isa<clang::RecordDecl>(context));
        if (Option<RecordElement&> parent = docs_db_.find_record_mut(
                clang::cast<clang::RecordDecl>(context));
            parent.is_some()) {
          return sus::some(
              sus::tuple(parent.as_value_mut().ctors, parent.as_value().name));
        }
      } else if (clang::isa<clang::CXXDestructorDecl>(decl)) {
        sus_check(clang::isa<clang::RecordDecl>(context));
        if (Option<RecordElement&> parent = docs_db_.find_record_mut(
                clang::cast<clang::RecordDecl>(context));
            parent.is_some()) {
          return sus::some(
              sus::tuple(parent.as_value_mut().dtors, parent.as_value().name));
        }
      } else if (clang::isa<clang::CXXConversionDecl>(decl)) {
        sus_check(clang::isa<clang::RecordDecl>(context));
        if (Option<RecordElement&> parent = docs_db_.find_record_mut(
                clang::cast<clang::RecordDecl>(context));
            parent.is_some()) {
          return sus::some(sus::tuple(parent.as_value_mut().conversions,
                                      parent.as_value().name));
        }
      } else if (clang::isa<clang::CXXMethodDecl>(decl)) {
        sus_check(clang::isa<clang::RecordDecl>(context));
        if (Option<RecordElement&> parent = docs_db_.find_record_mut(
                clang::cast<clang::RecordDecl>(context));
            parent.is_some()) {
          return sus::some(sus::tuple(parent.as_value_mut().methods,
                                      parent.as_value().name));
        }
      } else if (clang::isa<clang::CXXDeductionGuideDecl>(decl)) {
        sus_check(clang::isa<clang::NamespaceDecl>(context));
        // TODO: How do we get from here to the class that the deduction guide
        // is for reliably? getCorrespondingConstructor() would work if it's
        // generated only. Will the DeclContext find it?
        // return sus::some(parent->deductions);
      } else {
        // Note: VisitFriendDecl has a copy of this same logic.
        if (Option<NamespaceElement&> parent =
                docs_db_.find_namespace_mut(find_nearest_namespace(decl));
            parent.is_some()) {
          return sus::some(sus::tuple(parent.as_value_mut().functions, ""));
        }
      }
      return sus::none();
    }();

    if (map_and_self_name.is_some()) {
      auto&& [map, self_name] = sus::move(map_and_self_name).unwrap();

      add_function_with_comment(decl, context, map, self_name,
                                get_raw_comment(decl));
      // We look for comments on this function and any overridden methods.
      if (const auto* mdecl = clang::dyn_cast<clang::CXXMethodDecl>(decl)) {
        for (const clang::RawComment* raw_comment :
             sus::iter::from_range(mdecl->overridden_methods())
                 .map(get_raw_comment))
          add_function_with_comment(decl, context, map, self_name, raw_comment);
      }
    }
    return true;
  }

  bool VisitFriendDecl(clang::FriendDecl* decl) noexcept {
    if (!decl->getFriendDecl()) return true;
    if (!clang::isa<clang::FunctionDecl>(decl->getFriendDecl())) return true;

    // This is a friend function which may be declared inside a class. Once
    // we visit the FuncionDecl we can't get back to the FriendDecl to find
    // the class. So we handle it directly here instead.
    auto* fdecl = clang::cast<clang::FunctionDecl>(decl->getFriendDecl());
    // Friend forward declarations are not visited, they would create an
    // overload which does not actually exist.
    if (fdecl->getDefinition() != fdecl) return true;

    if (should_skip_decl(cx_, fdecl)) return true;

    // We get the context from the FriendDecl, which is the class, **not**
    // from the FunctionDecl for which it would be the namespace the class
    // is in.
    //
    // TODO: Save the linkage spec (`extern "C"`) so we can show it.
    clang::DeclContext* context = decl->getDeclContext();
    while (clang::isa<clang::LinkageSpecDecl>(context))
      context = context->getParent();

    // Note: This duplicates logic from VisitFunctionDecl. The function will
    // be visited again later, but it will already be in the db and get
    // ignored.

    std::string self_name;
    if (Option<RecordElement&> record =
            docs_db_.find_record_mut(clang::cast<clang::RecordDecl>(context));
        record.is_some()) {
      self_name = record.as_value().name;
    }

    // TODO: Should we store friend functions into the `record` instead of the
    // namespace?
    if (Option<NamespaceElement&> parent =
            docs_db_.find_namespace_mut(find_nearest_namespace(decl));
        parent.is_some()) {
      add_function_with_comment(fdecl, context, parent.as_value_mut().functions,
                                self_name, get_raw_comment(fdecl));
    }
    return true;
  }

 private:
  void add_function_with_comment(
      clang::FunctionDecl* decl, clang::DeclContext* context,
      std::unordered_map<FunctionId, FunctionElement, FunctionId::Hash>& map,
      std::string_view self_name, const clang::RawComment* raw_comment) {
    Comment comment =
        make_db_comment(decl->getASTContext(), raw_comment, self_name);

    auto params =
        Vec<FunctionParameter>::with_capacity(decl->parameters().size());
    for (const clang::ParmVarDecl* v : decl->parameters()) {
      auto linked_type = LinkedType::with_type(
          build_local_type(v->getType(), v->getASTContext(),
                           preprocessor_, v->getBeginLoc()),
          docs_db_);
      params.emplace(sus::move(linked_type), v->getNameAsString(),
                     sus::none()  // TODO: `v->getDefaultArg()`
      );
    }

    Vec<std::string> template_params;
    Option<RequiresConstraints> constraints;
    if (clang::FunctionTemplateDecl* tmpl =
            decl->getDescribedFunctionTemplate()) {
      template_params = collect_template_params(tmpl, preprocessor_);
      constraints = collect_template_constraints(tmpl, preprocessor_);
    } else {
      constraints = collect_function_constraints(decl, preprocessor_);
    }

    std::string signature_name = [&] {
      if (auto* mdecl = clang::dyn_cast<clang::CXXConstructorDecl>(decl)) {
        return mdecl->getThisType()
            ->getPointeeType()
            ->getAsRecordDecl()
            ->getNameAsString();
      } else if (auto* convdecl =
                     clang::dyn_cast<clang::CXXConversionDecl>(decl)) {
        Type t = build_local_type(convdecl->getReturnType(),
                                  convdecl->getASTContext(),
                                  preprocessor_, convdecl->getBeginLoc());
        return std::string("operator ") + t.name;
      } else {
        return decl->getNameAsString();
      }
    }();
    std::string function_name = [&] {
      if (auto* lit = decl->getLiteralIdentifier()) {
        // User-defined literals are displayed... less literally.
        return std::string(lit->getName()) + std::string(" literal");
      } else {
        return signature_name;
      }
    }();

    // Make a copy before moving `comment` to the contructor argument.
    Option<std::string> overload_set = sus::clone(comment.attrs.overload_set);

    std::string signature = "(";
    for (clang::ParmVarDecl* p : decl->parameters()) {
      signature += p->getOriginalType().getAsString();
    }
    signature += ")";
    if (auto* mdecl = clang::dyn_cast<clang::CXXMethodDecl>(decl)) {
      // Prevent a parameter and return qualifier from possibly being
      // confused for eachother in the string by putting a delimiter in
      // here that can't appear in the parameter list.
      signature += " -> ";
      signature += mdecl->getMethodQualifiers().getAsString();
      switch (mdecl->getRefQualifier()) {
        case clang::RefQualifierKind::RQ_None: break;
        case clang::RefQualifierKind::RQ_LValue: signature += "&"; break;
        case clang::RefQualifierKind::RQ_RValue: signature += "&&"; break;
      }
    }
    if (constraints.is_some()) {
      signature += " requires ";
      for (const auto& [i, c] : constraints->list.iter().enumerate()) {
        if (i > 0u) signature += ",";
        switch (c) {
          case RequiresConstraintTag::Concept: {
            const RequiresConceptConstraint& con =
                c.as<RequiresConstraintTag::Concept>();
            signature += con.concept_name;
            signature += "<";
            for (const auto& [j, arg] : con.args.iter().enumerate()) {
              if (j > 0u) signature += ",";
              signature += arg;
            }
            signature += ">";
            break;
          }
          case RequiresConstraintTag::Text: {
            signature += c.as<RequiresConstraintTag::Text>();
            break;
          }
        }
      }
    }

    auto linked_return_type = LinkedType::with_type(
        build_local_type(decl->getReturnType(),
                         decl->getASTContext(),
                         preprocessor_, decl->getBeginLoc()),
        docs_db_);

    FunctionId db_key = key_for_function(decl, overload_set);
    auto fe = FunctionElement(
        iter_namespace_path(decl).collect_vec(), sus::move(comment),
        sus::move(function_name), sus::move(signature_name),
        sus::move(signature),
        decl->isOverloadedOperator() || decl->getLiteralIdentifier() != nullptr,
        sus::move(linked_return_type), sus::move(constraints),
        sus::move(template_params), decl->isDeleted(), sus::move(params),
        sus::move(overload_set),
        iter_record_path(decl)
            .map([](std::string_view&& v) { return std::string(v); })
            .collect_vec(),
        decl->getASTContext().getSourceManager().getFileOffset(
            decl->getLocation()));

    if (auto* mdecl = clang::dyn_cast<clang::CXXMethodDecl>(decl)) {
      sus_check(clang::isa<clang::RecordDecl>(context));

      // TODO: It's possible to overload a method in a base class. What
      // should we show then? Let's show protected virtual methods just
      // in the classes where they are public, so we need to include
      // them in subclasses.

      if (Option<RecordElement&> parent =
              docs_db_.find_record_mut(clang::cast<clang::RecordDecl>(context));
          parent.is_some()) {
        fe.overloads[0u].method = sus::some(MethodSpecific{
            .is_static = mdecl->isStatic(),
            .is_volatile = mdecl->isVolatile(),
            .is_virtual = mdecl->isVirtual(),
            .is_ctor = clang::isa<clang::CXXConstructorDecl>(decl),
            .is_dtor = clang::isa<clang::CXXDestructorDecl>(decl),
            .is_conversion = clang::isa<clang::CXXConversionDecl>(decl),
            .is_explicit =
                [](const clang::CXXConstructorDecl* ctor) {
                  if (ctor) {
                    return ctor->isExplicit();
                  } else {
                    return false;
                  }
                }(clang::dyn_cast<clang::CXXConstructorDecl>(decl)),
            .qualifier =
                [mdecl]() {
                  switch (mdecl->getRefQualifier()) {
                    case clang::RQ_None:
                      if (mdecl->isConst())
                        return MethodQualifier::Const;
                      else
                        return MethodQualifier::Mutable;
                    case clang::RQ_LValue:
                      if (mdecl->isConst())
                        return MethodQualifier::ConstLValue;
                      else
                        return MethodQualifier::MutableLValue;
                    case clang::RQ_RValue:
                      if (mdecl->isConst())
                        return MethodQualifier::ConstRValue;
                      else
                        return MethodQualifier::MutableRValue;
                  }
                  sus::unreachable();
                }(),
        });
      }
    }
    add_function_overload_to_db(sus::clone(db_key), sus::move(fe), map,
                                decl->getASTContext());
    add_source_link_to_db(
        map.find(db_key)->second,
        [&] {
          if (decl->getDefinition() == decl) {
            if (raw_comment) return SourceLink::CommentAndDefinitionLocation;
            return SourceLink::DefinitionLocation;
          }
          if (raw_comment) return SourceLink::CommentLocation;
          return SourceLink::UnknownLocation;
        }(),
        decl->getLocation(), decl->getBeginLoc(), decl->getASTContext());
  }

  template <class MapT>
  void add_function_overload_to_db(FunctionId key, FunctionElement db_element,
                                   MapT& db_map,
                                   clang::ASTContext& ast_cx) noexcept {
    bool add_overload = true;
    auto it = db_map.find(key);
    if (it == db_map.end()) {
      db_map.emplace(sus::move(key), std::move(db_element));
      // The overload is added with the `db_element` as a whole.
      add_overload = false;
    } else if (!it->second.has_found_comment() &&
               db_element.has_found_comment()) {
      // Steal the comment.
      sus::mem::swap(db_map.at(key).comment, db_element.comment);
      add_overload = true;
    } else if (!db_element.has_found_comment()) {
      // Leave the existing comment in place.
      add_overload = true;
    } else if (db_element.comment.begin_loc == it->second.comment.begin_loc) {
      // We already visited this thing, from another translation unit.
      add_overload = false;
    } else {
      // The comment is ambiguous, there's another comment for the same overload
      // set. This is an error.
      add_overload = false;
      const auto& old_comment_element = it->second;
      ast_cx.getDiagnostics()
          .Report(db_element.comment.attrs.location,
                  diag_ids_.superceded_comment)
          .AddString(old_comment_element.comment.begin_loc);
    }

    if (add_overload) {
      sus_check_with_message(db_element.overloads.len() == 1u,
                             "Expected to add FunctionElement with 1 overload");

      bool exists = db_map.at(key).overloads.iter().any(
          [&db_element](const FunctionOverload& overload) {
            return overload.signature_key ==
                   db_element.overloads[0u].signature_key;
          });
      if (!exists)
        db_map.at(key).overloads.push(sus::move(db_element.overloads[0u]));
    }
  }

  template <class MapT>
  void add_macro_to_db(MacroId key, MacroElement db_element, MapT& db_map,
                       clang::ASTContext& ast_cx) noexcept {
    auto it = db_map.find(key);
    if (it == db_map.end()) {
      db_map.emplace(sus::move(key), std::move(db_element));
    } else if (!it->second.has_found_comment() &&
               db_element.has_found_comment()) {
      // Steal the comment.
      sus::mem::swap(db_map.at(key).comment, db_element.comment);
    } else if (!db_element.has_found_comment()) {
      // Leave the existing comment in place, do nothing.
    } else if (db_element.comment.begin_loc == it->second.comment.begin_loc) {
      // We already visited this thing, from another translation unit.
    } else {
      const MacroElement& old_element = it->second;
      ast_cx.getDiagnostics()
          .Report(db_element.comment.attrs.location,
                  diag_ids_.superceded_comment)
          .AddString(old_element.comment.begin_loc);
    }
  }

  template <class MapT>
  void add_namespace_to_db(NamespaceId key, NamespaceElement db_element,
                           MapT& db_map, clang::ASTContext& ast_cx) noexcept {
    auto it = db_map.find(key);
    if (it == db_map.end()) {
      db_map.emplace(sus::move(key), std::move(db_element));
    } else if (!it->second.has_found_comment() &&
               db_element.has_found_comment()) {
      // Steal the comment.
      sus::mem::swap(db_map.at(key).comment, db_element.comment);
    } else if (!db_element.has_found_comment()) {
      // Leave the existing comment in place, do nothing.
    } else if (db_element.comment.begin_loc == it->second.comment.begin_loc) {
      // We already visited this thing, from another translation unit.
    } else {
      const NamespaceElement& old_element = it->second;
      ast_cx.getDiagnostics()
          .Report(db_element.comment.attrs.location,
                  diag_ids_.superceded_comment)
          .AddString(old_element.comment.begin_loc);
    }
  }

  template <class MapT>
  void add_concept_to_db(ConceptId key, ConceptElement db_element, MapT& db_map,
                         clang::ASTContext& ast_cx) noexcept {
    auto it = db_map.find(key);
    if (it == db_map.end()) {
      db_map.emplace(sus::move(key), std::move(db_element));
    } else if (!it->second.has_found_comment() &&
               db_element.has_found_comment()) {
      // Steal the comment.
      sus::mem::swap(db_map.at(key).comment, db_element.comment);
    } else if (!db_element.has_found_comment()) {
      // Leave the existing comment in place, do nothing.
    } else if (db_element.comment.begin_loc == it->second.comment.begin_loc) {
      // We already visited this thing, from another translation unit.
    } else {
      const ConceptElement& old_element = it->second;
      ast_cx.getDiagnostics()
          .Report(db_element.comment.attrs.location,
                  diag_ids_.superceded_comment)
          .AddString(old_element.comment.begin_loc);
    }
  }

  template <class ElementT, class MapT>
    requires ::sus::ptr::SameOrSubclassOf<ElementT*, CommentElement*>
  void add_record_to_db(RecordId key, ElementT db_element, MapT& db_map,
                        clang::ASTContext& ast_cx) noexcept {
    auto it = db_map.find(key);
    if (it == db_map.end()) {
      db_map.emplace(sus::move(key), std::move(db_element));
    } else if (!it->second.has_found_comment() &&
               db_element.has_found_comment()) {
      // Steal the comment.
      sus::mem::swap(db_map.at(key).comment, db_element.comment);
    } else if (!db_element.has_found_comment()) {
      // Leave the existing comment in place, do nothing.
    } else if (db_element.comment.begin_loc == it->second.comment.begin_loc) {
      // We already visited this thing, from another translation unit.
    } else {
      const ElementT& old_element = it->second;
      ast_cx.getDiagnostics()
          .Report(db_element.comment.attrs.location,
                  diag_ids_.superceded_comment)
          .AddString(old_element.comment.begin_loc);
    }
  }
  template <class MapT>
  void add_alias_to_db(AliasId key, AliasElement db_element, MapT& db_map,
                       clang::ASTContext& ast_cx) noexcept {
    auto it = db_map.find(key);
    if (it == db_map.end()) {
      db_map.emplace(sus::move(key), std::move(db_element));
    } else if (!it->second.has_found_comment() &&
               db_element.has_found_comment()) {
      // Steal the comment.
      sus::mem::swap(db_map.at(key).comment, db_element.comment);
    } else if (!db_element.has_found_comment()) {
      // Leave the existing comment in place, do nothing.
    } else if (db_element.comment.begin_loc == it->second.comment.begin_loc) {
      // We already visited this thing, from another translation unit.
    } else {
      const AliasElement& old_element = it->second;
      ast_cx.getDiagnostics()
          .Report(db_element.comment.attrs.location,
                  diag_ids_.superceded_comment)
          .AddString(old_element.comment.begin_loc);
    }
  }
  template <class ElementT, class MapT>
    requires ::sus::ptr::SameOrSubclassOf<ElementT*, CommentElement*>
  void add_field_to_db(UniqueSymbol uniq, ElementT db_element, MapT& db_map,
                       clang::ASTContext& ast_cx) noexcept {
    auto it = db_map.find(uniq);
    if (it == db_map.end()) {
      db_map.emplace(uniq, std::move(db_element));
    } else if (!it->second.has_found_comment() &&
               db_element.has_found_comment()) {
      // Steal the comment.
      sus::mem::swap(db_map.at(uniq).comment, db_element.comment);
    } else if (!db_element.has_found_comment()) {
      // Leave the existing comment in place, do nothing.
    } else if (db_element.comment.begin_loc == it->second.comment.begin_loc) {
      // We already visited this thing, from another translation unit.
    } else {
      const ElementT& old_element = it->second;
      ast_cx.getDiagnostics()
          .Report(db_element.comment.attrs.location,
                  diag_ids_.superceded_comment)
          .AddString(old_element.comment.begin_loc);
    }
  }

  Comment make_db_comment(clang::ASTContext& ast_cx,
                          const clang::RawComment* raw,
                          std::string_view self_name) noexcept {
    auto& src_manager = ast_cx.getSourceManager();
    if (raw) {
      sus::Result<ParsedComment, ParseCommentError> comment_result =
          parse_comment(ast_cx, *raw, self_name);
      if (comment_result.is_ok()) {
        auto&& [attrs, text] = sus::move(comment_result).unwrap();
        return Comment(sus::move(text),
                       raw->getBeginLoc().printToString(src_manager),
                       sus::move(attrs));
      }
      ast_cx.getDiagnostics()
          .Report(raw->getBeginLoc(), diag_ids_.malformed_comment)
          .AddString(sus::move(comment_result).unwrap_err().message);
    }
    return Comment();
  }

  void add_source_link_to_db(CommentElement& e, SourceLink::Quality quality,
                             clang::SourceLocation loc,
                             clang::SourceLocation begin_loc,
                             clang::ASTContext& ast_cx) noexcept {
    // The user may not have anywhere to store code in order to link to it.
    if (!cx_.options.generate_source_links) return;

    clang::SourceManager& sm = ast_cx.getSourceManager();

    const clang::FileEntry* entry = sm.getFileEntryForID(sm.getFileID(loc));
    while (loc.isMacroID()) {
      // If the macro call location is at the start of a line, we believe it's a
      // call to a macro where the decl is being defined. Then we want the
      // spelling location which is inside the macro. To find that we need to
      // get the spelling location from the decl's "*begin* location", which is
      // different from its "location".
      //
      // Otherwise, the macro is defining the name or something, but the decl
      // itself is at the call site of the macro, and we want the expansion
      // location at the caller.
      bool decl_inside_macro = false;
      clang::SourceLocation macro_call_loc = sm.getExpansionLoc(loc);
      if (auto buffer_ref = sm.getBufferOrNone(sm.getFileID(macro_call_loc));
          buffer_ref.has_value()) {
        const u32 end = sm.getFileOffset(macro_call_loc);
        llvm::StringRef buffer = buffer_ref->getBuffer();
        u32 start = end;
        while (start > 0u && buffer[start - 1u] != '\n') start -= 1u;
        buffer = buffer.substr(start, end - start);
        while (buffer.consume_front(" "));
        if (buffer.empty()) decl_inside_macro = true;
      }
      if (decl_inside_macro)
        loc = sm.getSpellingLoc(begin_loc);
      else
        loc = macro_call_loc;
      entry = sm.getFileEntryForID(sm.getFileID(loc));
    }

    if (entry) {
      // Canonicalize the path to use `/` instead of `\`.
      auto canonical_path = std::string(sm.getFilename(loc));
      std::replace(canonical_path.begin(), canonical_path.end(), '\\', '/');

      if (cx_.options.remove_path_prefix.is_some()) {
        // Canonicalize the path to use `/` instead of `\`.
        std::string canonical_prefix =
            sus::clone(cx_.options.remove_path_prefix.as_value());
        std::replace(canonical_prefix.begin(), canonical_prefix.end(), '\\',
                     '/');

        if (canonical_path.starts_with(canonical_prefix)) {
          canonical_path = canonical_path.substr(canonical_prefix.size());
          if (canonical_path.starts_with("/"))
            canonical_path = canonical_path.substr(1u);
        }
      }
      if (cx_.options.add_path_prefix.is_some()) {
        const auto& prefix = cx_.options.add_path_prefix.as_value();
        if (!prefix.ends_with("/")) canonical_path.insert(0u, "/");
        canonical_path.insert(0u, prefix);
      }

      std::ostringstream line;
      if (cx_.options.source_line_prefix.is_some()) {
        line << cx_.options.source_line_prefix.as_value();
      }
      line << sm.getLineNumber(sm.getFileID(loc), sm.getFileOffset(loc));

      auto link = Option<SourceLink>(SourceLink{
          .quality = quality,
          .file_path = sus::move(canonical_path),
          .line = sus::move(line).str(),
      });
      if (link > e.source_link) e.source_link = sus::move(link);
    }
  }

  VisitCx& cx_;
  Database& docs_db_;
  DiagnosticIds diag_ids_;
  clang::Preprocessor& preprocessor_;
};

class AstConsumer : public clang::ASTConsumer {
 public:
  AstConsumer(VisitCx& cx, Database& docs_db, clang::Preprocessor& preprocessor)
      : cx_(cx), docs_db_(docs_db), preprocessor_(preprocessor) {}

  bool HandleTopLevelDecl(clang::DeclGroupRef group_ref) noexcept final {
    for (clang::Decl* decl : group_ref) {
      clang::SourceManager& sm = decl->getASTContext().getSourceManager();

      if (!decl->getLocation().isMacroID()) {
        // Don't visit the same file repeatedly.
        auto v = VisitedLocation(decl->getLocation().printToString(sm));
        if (cx_.visited_locations.contains(v)) {
          continue;
        }
        cx_.visited_locations.emplace(sus::move(v));
      }

      if (!cx_.should_include_decl_based_on_file(decl)) {
        continue;
      }

      {
        auto visitor =
            Visitor(cx_, docs_db_, preprocessor_,
                    DiagnosticIds::with_context(decl->getASTContext()));
        if (!visitor.TraverseDecl(decl)) {
          return false;
        }
        if (!visitor.VisitMacros(decl)) {
          return false;
        }
      }
      if (decl->getASTContext().getDiagnostics().getNumErrors() > 0u)
        return false;
    }
    return true;
  }

  void HandleTranslationUnit(clang::ASTContext& ast_cx) noexcept final {
    if (cx_.options.on_tu_complete.is_some()) {
      ::sus::fn::call(*cx_.options.on_tu_complete, ast_cx, preprocessor_);
    }
  }

 private:
  VisitCx& cx_;
  Database& docs_db_;
  clang::Preprocessor& preprocessor_;
};

std::unique_ptr<clang::FrontendAction> VisitorFactory::create() noexcept {
  return std::make_unique<VisitorAction>(cx, docs_db, line_stats);
}

std::unique_ptr<clang::ASTConsumer> VisitorAction::CreateASTConsumer(
    clang::CompilerInstance& compiler, llvm::StringRef file) noexcept {
  if (cx.options.show_progress) {
    if (std::string_view(file) != line_stats.cur_file_name) {
      fmt::println(stderr, "[{}/{}] {}", line_stats.cur_file,
                   line_stats.num_files, std::string_view(file));
      line_stats.cur_file += 1u;
      line_stats.cur_file_name = std::string(file);
    }
  }
  return std::make_unique<AstConsumer>(cx, docs_db, compiler.getPreprocessor());
}

bool VisitCx::should_include_decl_based_on_file(clang::Decl* decl) noexcept {
  clang::SourceManager& sm = decl->getASTContext().getSourceManager();

  clang::SourceLocation loc = decl->getLocation();
  const clang::FileEntry* entry = sm.getFileEntryForID(sm.getFileID(loc));
  // If a macro is defining stuff, we care about including it or not based on
  // where the macro is used, which is the expansion location.
  while (loc.isMacroID()) {
    loc = sm.getExpansionLoc(loc);
    entry = sm.getFileEntryForID(sm.getFileID(loc));
  }

  // No FileEntry (and not a macro, since we've found the macro expansion
  // above already) means a builtin, including a lot of `std::`, or maybe some
  // other things. We don't want to chase builtins.
  if (!entry) {
    return false;
  }

  // And if there's no path then we also default to include it.
  llvm::StringRef path = entry->tryGetRealPathName();
  if (path.empty()) {
    return true;
  }
  // Canonicalize the path to use `/` instead of `\`.
  auto canonical_path = std::string(path);
  std::replace(canonical_path.begin(), canonical_path.end(), '\\', '/');

  // Compare the path to the user-specified include/exclude-patterns.
  enum { CheckPath, ExcludePath, IncludePath } what_to_do;
  // We cache the regex decision for each visited path name.
  if (auto it = visited_paths_.find(canonical_path);
      it != visited_paths_.end()) {
    what_to_do = it->second.included ? IncludePath : ExcludePath;
  } else {
    what_to_do = CheckPath;
  }

  switch (what_to_do) {
    case IncludePath: return true;
    case ExcludePath: return false;
    case CheckPath: {
      auto [it, inserted] =
          visited_paths_.emplace(sus::move(canonical_path), VisitedPath(true));
      sus_check(inserted);
      auto& [path_str, visited_path] = *it;
      if (!std::regex_search(path_str, options.include_path_patterns)) {
        visited_path.included = false;
        return false;
      }
      if (std::regex_search(path_str, options.exclude_path_patterns)) {
        visited_path.included = false;
        return false;
      }
      return true;
    }
  }
  sus::unreachable();
}

}  // namespace subdoc

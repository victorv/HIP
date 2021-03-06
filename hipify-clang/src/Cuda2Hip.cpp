/*
Copyright (c) 2015-2017 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
/**
 * @file Cuda2Hip.cpp
 *
 * This file is compiled and linked into clang based hipify tool.
 */
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "CUDA2HipMap.h"
#include "LLVMCompat.h"
#include "StringUtils.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

#define DEBUG_TYPE "cuda2hip"


// Set up the command line options
static cl::OptionCategory ToolTemplateCategory("CUDA to HIP source translator options");

static cl::opt<std::string> OutputFilename("o",
  cl::desc("Output filename"),
  cl::value_desc("filename"),
  cl::cat(ToolTemplateCategory));

static cl::opt<bool> Inplace("inplace",
  cl::desc("Modify input file inplace, replacing input with hipified output, save backup in .prehip file"),
  cl::value_desc("inplace"),
  cl::cat(ToolTemplateCategory));

static cl::opt<bool> NoBackup("no-backup",
  cl::desc("Don't create a backup file for the hipified source"),
  cl::value_desc("no-backup"),
  cl::cat(ToolTemplateCategory));

static cl::opt<bool> NoOutput("no-output",
  cl::desc("Don't write any translated output to stdout"),
  cl::value_desc("no-output"),
  cl::cat(ToolTemplateCategory));

static cl::opt<bool> PrintStats("print-stats",
  cl::desc("Print translation statistics"),
  cl::value_desc("print-stats"),
  cl::cat(ToolTemplateCategory));

static cl::opt<std::string> OutputStatsFilename("o-stats",
  cl::desc("Output filename for statistics"),
  cl::value_desc("filename"),
  cl::cat(ToolTemplateCategory));

static cl::opt<bool> Examine("examine",
  cl::desc("Combines -no-output and -print-stats options"),
  cl::value_desc("examine"),
  cl::cat(ToolTemplateCategory));

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

class Cuda2Hip {
public:
  Cuda2Hip(Replacements& R, const std::string &srcFileName) :
    Replace(R), mainFileName(srcFileName) {}

  enum msgTypes {
    HIPIFY_ERROR = 0,
    HIPIFY_WARNING
  };

  std::string getMsgType(msgTypes type) {
    switch (type) {
      case HIPIFY_ERROR: return "error";
      default:
      case HIPIFY_WARNING: return "warning";
    }
  }

protected:
  Replacements& Replace;
  std::string mainFileName;

  virtual void insertReplacement(const Replacement &rep, const FullSourceLoc &fullSL) {
    llcompat::insertReplacement(Replace, rep);
    if (PrintStats) {
      rep.getLength();
      Statistics::current().lineTouched(fullSL.getExpansionLineNumber());
      Statistics::current().bytesChanged(rep.getLength());
    }
  }
  void insertHipHeaders(Cuda2Hip *owner, const SourceManager &SM) {
    if (Replace.size() > 0) {
      std::string repName = "#include <hip/hip_runtime.h>";
      Statistics::current().incrementCounter({repName, ConvTypes::CONV_INCLUDE_CUDA_MAIN_H, ApiTypes::API_RUNTIME}, "#include <cuda>");
      SourceLocation sl = SM.getLocForStartOfFile(SM.getMainFileID());
      FullSourceLoc fullSL(sl, SM);
      Replacement Rep(SM, sl, 0, repName + "\n");
      insertReplacement(Rep, fullSL);
    }
  }

  void printHipifyMessage(const SourceManager &SM, const SourceLocation &sl, const std::string &message, msgTypes msgType = HIPIFY_WARNING) {
    FullSourceLoc fullSL(sl, SM);
    llvm::errs() << "[HIPIFY] " << getMsgType(msgType) << ": " << mainFileName << ":" << fullSL.getExpansionLineNumber() << ":" << fullSL.getExpansionColumnNumber() << ": " << message << "\n";
  }

  void processString(StringRef s, SourceManager &SM, SourceLocation start) {
    size_t begin = 0;
    while ((begin = s.find("cu", begin)) != StringRef::npos) {
      const size_t end = s.find_first_of(" ", begin + 4);
      StringRef name = s.slice(begin, end);
      const auto found = CUDA_RENAMES_MAP().find(name);
      if (found != CUDA_RENAMES_MAP().end()) {
        StringRef repName = found->second.hipName;
        hipCounter counter = {"[string literal]", ConvTypes::CONV_LITERAL, ApiTypes::API_RUNTIME, found->second.unsupported};
        Statistics::current().incrementCounter(counter, name.str());
        if (!counter.unsupported) {
          SourceLocation sl = start.getLocWithOffset(begin + 1);
          Replacement Rep(SM, sl, name.size(), repName);
          FullSourceLoc fullSL(sl, SM);
          insertReplacement(Rep, fullSL);
        }
      }

      if (end == StringRef::npos) {
        break;
      }
      begin = end + 1;
    }
  }
};

class Cuda2HipCallback;

class HipifyPPCallbacks : public PPCallbacks, public SourceFileCallbacks, public Cuda2Hip {
public:
  HipifyPPCallbacks(Replacements& R, const std::string &mainFileName)
    : Cuda2Hip(R, mainFileName) {}

  virtual bool handleBeginSource(CompilerInstance &CI
#if LLVM_VERSION_MAJOR <= 4
                                 , StringRef Filename
#endif
                                 ) override {
    Preprocessor &PP = CI.getPreprocessor();
    SourceManager &SM = CI.getSourceManager();
    setSourceManager(&SM);
    PP.addPPCallbacks(std::unique_ptr<HipifyPPCallbacks>(this));
    setPreprocessor(&PP);
    return true;
  }

  virtual void handleEndSource() override;

  virtual void InclusionDirective(SourceLocation hash_loc,
                                  const Token &include_token,
                                  StringRef file_name, bool is_angled,
                                  CharSourceRange filename_range,
                                  const FileEntry *file, StringRef search_path,
                                  StringRef relative_path,
                                  const clang::Module *imported) override {
    if (!_sm->isWrittenInMainFile(hash_loc) || !is_angled) {
      return; // We're looking to rewrite angle-includes in the main file to point to hip.
    }

    const auto found = CUDA_INCLUDE_MAP.find(file_name);
    if (found == CUDA_INCLUDE_MAP.end()) {
      // Not a CUDA include - don't touch it.
      return;
    }

    Statistics::current().incrementCounter(found->second, file_name.str());
    if (found->second.unsupported) {
      // An unsupported CUDA header? Oh dear. Print a warning.
      printHipifyMessage(*_sm, hash_loc, "Unsupported CUDA header used: " + file_name.str());
      return;
    }

    StringRef repName = found->second.hipName;
    DEBUG(dbgs() << "Include file found: " << file_name << "\n"
                 << "SourceLocation: "
                 << filename_range.getBegin().printToString(*_sm) << "\n"
                 << "Will be replaced with " << repName << "\n");
    SourceLocation sl = filename_range.getBegin();
    SourceLocation sle = filename_range.getEnd();
    const char *B = _sm->getCharacterData(sl);
    const char *E = _sm->getCharacterData(sle);
    SmallString<128> tmpData;
    Replacement Rep(*_sm, sl, E - B, Twine("<" + repName + ">").toStringRef(tmpData));
    FullSourceLoc fullSL(sl, *_sm);
    insertReplacement(Rep, fullSL);
  }

  /**
   * Look at, and consider altering, a given token.
   *
   * If it's not a CUDA identifier, nothing happens.
   * If it's an unsupported CUDA identifier, a warning is emitted.
   * Otherwise, the source file is updated with the corresponding hipification.
   */
  void RewriteToken(Token t) {
    // String literals containing CUDA references need fixing...
    if (t.is(tok::string_literal)) {
      StringRef s(t.getLiteralData(), t.getLength());
      processString(unquoteStr(s), *_sm, t.getLocation());
      return;
    } else if (!t.isAnyIdentifier()) {
      // If it's neither a string nor an identifier, we don't care.
      return;
    }

    StringRef name = t.getIdentifierInfo()->getName();
    const auto found = CUDA_RENAMES_MAP().find(name);
    if (found == CUDA_RENAMES_MAP().end()) {
      // So it's an identifier, but not CUDA? Boring.
      return;
    }
    Statistics::current().incrementCounter(found->second, name.str());

    SourceLocation sl = t.getLocation();
    if (found->second.unsupported) {
      // An unsupported identifier? Curses! Warn the user.
      printHipifyMessage(*_sm, sl, "Unsupported CUDA identifier used: " + name.str());
      return;
    }

    StringRef repName = found->second.hipName;
    Replacement Rep(*_sm, sl, name.size(), repName);
    FullSourceLoc fullSL(sl, *_sm);
    insertReplacement(Rep, fullSL);
  }

  virtual void MacroDefined(const Token &MacroNameTok,
                            const MacroDirective *MD) override {
    if (!_sm->isWrittenInMainFile(MD->getLocation()) ||
        MD->getKind() != MacroDirective::MD_Define) {
      return;
    }

    for (auto T : MD->getMacroInfo()->tokens()) {
      RewriteToken(T);
    }
  }

  virtual void MacroExpands(const Token &MacroNameTok,
                            const MacroDefinition &MD, SourceRange Range,
                            const MacroArgs *Args) override {

    if (!_sm->isWrittenInMainFile(MacroNameTok.getLocation())) {
      return; // Macros in headers are not our concern.
    }

    // Is the macro itself a CUDA identifier? If so, rewrite it
    RewriteToken(MacroNameTok);

    // If it's a macro with arguments, rewrite all the arguments as hip, too.
    for (unsigned int i = 0; Args && i < MD.getMacroInfo()->GET_NUM_ARGS(); i++) {
      std::vector<Token> toks;
      // Code below is a kind of stolen from 'MacroArgs::getPreExpArgument'
      // to workaround the 'const' MacroArgs passed into this hook.
      const Token *start = Args->getUnexpArgument(i);
      size_t len = Args->getArgLength(start) + 1;
      llcompat::EnterPreprocessorTokenStream(*_pp, start, len, false);

      do {
        toks.push_back(Token());
        Token &tk = toks.back();
        _pp->Lex(tk);
      } while (toks.back().isNot(tok::eof));

      _pp->RemoveTopOfLexerStack();
      // end of stolen code

      for (auto tok : toks) {
        RewriteToken(tok);
      }
    }
  }

  void EndOfMainFile() override {}

  void setSourceManager(SourceManager *sm) { _sm = sm; }
  void setPreprocessor(Preprocessor *pp) { _pp = pp; }
  void setMatch(Cuda2HipCallback *match) { Match = match; }

private:
  SourceManager *_sm = nullptr;
  Preprocessor *_pp = nullptr;
  Cuda2HipCallback *Match = nullptr;
};

class Cuda2HipCallback : public MatchFinder::MatchCallback, public Cuda2Hip {
private:
  bool cudaCall(const MatchFinder::MatchResult &Result) {
    const CallExpr *call = Result.Nodes.getNodeAs<CallExpr>("cudaCall");
    if (!call) {
      return false; // Another handler will do it.
    }

    const FunctionDecl *funcDcl = call->getDirectCallee();
    std::string name = funcDcl->getDeclName().getAsString();
    SourceManager *SM = Result.SourceManager;
    SourceLocation sl = call->getLocStart();

    // TODO: Make a lookup table just for functions to improve performance.
    const auto found = CUDA_IDENTIFIER_MAP.find(name);
    if (found == CUDA_IDENTIFIER_MAP.end()) {
      std::string msg = "the following reference is not handled: '" + name + "' [function call].";
      printHipifyMessage(*SM, sl, msg);
      return true;
    }

    const hipCounter& hipCtr = found->second;
    Statistics::current().incrementCounter(hipCtr, name);

    if (hipCtr.unsupported) {
      return true; // Silently fail when you find an unsupported member.
      // TODO: Print a warning with the diagnostics API?
    }

    size_t length = name.size();
    Replacement Rep(*SM, sl, length, hipCtr.hipName);
    FullSourceLoc fullSL(sl, *SM);
    insertReplacement(Rep, fullSL);

    return true;
  }

  SourceRange getReadRange(clang::SourceManager &SM, const SourceRange &exprRange) {
    SourceLocation begin = exprRange.getBegin();
    SourceLocation end = exprRange.getEnd();

    bool beginSafe = !SM.isMacroBodyExpansion(begin) || Lexer::isAtStartOfMacroExpansion(begin, SM, LangOptions{});
    bool endSafe = !SM.isMacroBodyExpansion(end) || Lexer::isAtEndOfMacroExpansion(end, SM, LangOptions{});

    if (beginSafe && endSafe) {
      return {SM.getFileLoc(begin), SM.getFileLoc(end)};
    } else {
      return {SM.getSpellingLoc(begin), SM.getSpellingLoc(end)};
    }
  }

  SourceRange getWriteRange(clang::SourceManager &SM, const SourceRange &exprRange) {
    SourceLocation begin = exprRange.getBegin();
    SourceLocation end = exprRange.getEnd();

    // If the range is contained within a macro, update the macro definition.
    // Otherwise, use the file location and hope for the best.
    if (!SM.isMacroBodyExpansion(begin) || !SM.isMacroBodyExpansion(end)) {
      return {SM.getFileLoc(begin), SM.getFileLoc(end)};
    }

    return {SM.getSpellingLoc(begin), SM.getSpellingLoc(end)};
  }

  StringRef readSourceText(clang::SourceManager& SM, const SourceRange& exprRange) {
    return Lexer::getSourceText(CharSourceRange::getTokenRange(getReadRange(SM, exprRange)), SM, LangOptions(), nullptr);
  }

  /**
   * Get a string representation of the expression `arg`, unless it's a defaulting function
   * call argument, in which case get a 0. Used for building argument lists to kernel calls.
   */
  std::string stringifyZeroDefaultedArg(SourceManager& SM, const Expr* arg) {
    if (isa<CXXDefaultArgExpr>(arg)) {
      return "0";
    } else {
      return readSourceText(SM, arg->getSourceRange());
    }
  }

  bool cudaLaunchKernel(const MatchFinder::MatchResult &Result) {
    StringRef refName = "cudaLaunchKernel";
    if (const CUDAKernelCallExpr *launchKernel = Result.Nodes.getNodeAs<CUDAKernelCallExpr>(refName)) {
      SmallString<40> XStr;
      raw_svector_ostream OS(XStr);

      LangOptions DefaultLangOptions;
      SourceManager *SM = Result.SourceManager;

      const Expr& calleeExpr = *(launchKernel->getCallee());
      OS << "hipLaunchKernelGGL(" << readSourceText(*SM, calleeExpr.getSourceRange()) << ", ";

      // Next up are the four kernel configuration parameters, the last two of which are optional and default to zero.
      const CallExpr& config = *(launchKernel->getConfig());

      // Copy the two dimensional arguments verbatim.
      OS << "dim3(" << readSourceText(*SM, config.getArg(0)->getSourceRange()) << "), ";
      OS << "dim3(" << readSourceText(*SM, config.getArg(1)->getSourceRange()) << "), ";

      // The stream/memory arguments default to zero if omitted.
      OS << stringifyZeroDefaultedArg(*SM, config.getArg(2)) << ", ";
      OS << stringifyZeroDefaultedArg(*SM, config.getArg(3));

      // If there are ordinary arguments to the kernel, just copy them verbatim into our new call.
      int numArgs = launchKernel->getNumArgs();
      if (numArgs > 0) {
        OS << ", ";

        // Start of the first argument.
        SourceLocation argStart = launchKernel->getArg(0)->getLocStart();

        // End of the last argument.
        SourceLocation argEnd = launchKernel->getArg(numArgs - 1)->getLocEnd();

        OS << readSourceText(*SM, {argStart, argEnd});
      }

      OS << ")";

      SourceRange replacementRange = getWriteRange(*SM, {launchKernel->getLocStart(), launchKernel->getLocEnd()});
      SourceLocation launchStart = replacementRange.getBegin();
      SourceLocation launchEnd = replacementRange.getEnd();

      size_t length = SM->getCharacterData(Lexer::getLocForEndOfToken(
                        launchEnd, 0, *SM, DefaultLangOptions)) -
                        SM->getCharacterData(launchStart);

      Replacement Rep(*SM, launchStart, length, OS.str());
      FullSourceLoc fullSL(launchStart, *SM);
      insertReplacement(Rep, fullSL);
      hipCounter counter = {"hipLaunchKernelGGL", ConvTypes::CONV_KERN, ApiTypes::API_RUNTIME};
      Statistics::current().incrementCounter(counter, refName.str());
      return true;
    }
    return false;
  }

  bool cudaBuiltin(const MatchFinder::MatchResult &Result) {
    if (const MemberExpr *threadIdx = Result.Nodes.getNodeAs<MemberExpr>("cudaBuiltin")) {
      if (const OpaqueValueExpr *refBase =
        dyn_cast<OpaqueValueExpr>(threadIdx->getBase())) {
        if (const DeclRefExpr *declRef =
          dyn_cast<DeclRefExpr>(refBase->getSourceExpr())) {
          SourceLocation sl = threadIdx->getLocStart();
          SourceManager *SM = Result.SourceManager;
          StringRef name = declRef->getDecl()->getName();
          StringRef memberName = threadIdx->getMemberDecl()->getName();
          size_t pos = memberName.find_first_not_of("__fetch_builtin_");
          memberName = memberName.slice(pos, memberName.size());
          SmallString<128> tmpData;
          name = Twine(name + "." + memberName).toStringRef(tmpData);

          // TODO: Make a lookup table just for builtins to improve performance.
          const auto found = CUDA_IDENTIFIER_MAP.find(name);
          if (found != CUDA_IDENTIFIER_MAP.end()) {
            Statistics::current().incrementCounter(found->second, name.str());
            if (!found->second.unsupported) {
              StringRef repName = found->second.hipName;
              Replacement Rep(*SM, sl, name.size(), repName);
              FullSourceLoc fullSL(sl, *SM);
              insertReplacement(Rep, fullSL);
            }
          } else {
            std::string msg = "the following reference is not handled: '" + name.str() + "' [builtin].";
            printHipifyMessage(*SM, sl, msg);
          }
        }
      }
      return true;
    }
    return false;
  }

  bool cudaEnumConstantRef(const MatchFinder::MatchResult &Result) {
    if (const DeclRefExpr *enumConstantRef = Result.Nodes.getNodeAs<DeclRefExpr>("cudaEnumConstantRef")) {
      StringRef name = enumConstantRef->getDecl()->getName();
      SourceLocation sl = enumConstantRef->getLocStart();
      SourceManager *SM = Result.SourceManager;

      // TODO: Make a lookup table just for enum values to improve performance.
      const auto found = CUDA_IDENTIFIER_MAP.find(name);
      if (found != CUDA_IDENTIFIER_MAP.end()) {
        Statistics::current().incrementCounter(found->second, name.str());
        if (!found->second.unsupported) {
          StringRef repName = found->second.hipName;
          Replacement Rep(*SM, sl, name.size(), repName);
          FullSourceLoc fullSL(sl, *SM);
          insertReplacement(Rep, fullSL);
        }
      } else {
        std::string msg = "the following reference is not handled: '" + name.str() + "' [enum constant ref].";
        printHipifyMessage(*SM, sl, msg);
      }
      return true;
    }
    return false;
  }

  bool cudaType(const MatchFinder::MatchResult& Result) {
      const clang::TypeLoc* ret = Result.Nodes.getNodeAs<TypeLoc>("cudaType");
      if (!ret) {
          return false;
      }

      // Ignore qualifiers - they don't alter our decision to rename.
      clang::UnqualTypeLoc tl = ret->getUnqualifiedLoc();
      const Type& typeObject = *(tl.getTypePtr());

      std::string typeName = tl.getType().getAsString();

      // Irritatingly, enum/struct types are identified as `enum/struct <something>`, and unlike most compound
      // types (such as pointers or references), there isn't another type node inside. So we have
      // to make do with what we've got. There's probably a better way of doing this...
      if (typeObject.isEnumeralType()) {
        removePrefixIfPresent(typeName, "enum ");
      }
      if (typeObject.isStructureType()) {
        removePrefixIfPresent(typeName, "struct ");
      }

      // Do we have a replacement for this type?
      const auto found = CUDA_TYPE_NAME_MAP.find(typeName);
      if (found == CUDA_TYPE_NAME_MAP.end()) {
          return false;
      }

      SourceManager &SM = *(Result.SourceManager);

      // Start of the type expression to replace.
      SourceLocation sl = tl.getBeginLoc();

      const hipCounter& hipCtr = found->second;
      if (hipCtr.unsupported) {
          printHipifyMessage(SM, sl, "Unsupported CUDA '" + typeName);
          return false;
      }

      // Apply the rename!
      Replacement Rep(SM, sl, typeName.size(), hipCtr.hipName);
      FullSourceLoc fullSL(sl, SM);
      insertReplacement(Rep, fullSL);

      return true;
  }

  bool cudaSharedIncompleteArrayVar(const MatchFinder::MatchResult &Result) {
    StringRef refName = "cudaSharedIncompleteArrayVar";
    if (const VarDecl *sharedVar = Result.Nodes.getNodeAs<VarDecl>(refName)) {
      // Example: extern __shared__ uint sRadix1[];
      if (sharedVar->hasExternalFormalLinkage()) {
        QualType QT = sharedVar->getType();
        std::string typeName;
        if (QT->isIncompleteArrayType()) {
          const ArrayType *AT = QT.getTypePtr()->getAsArrayTypeUnsafe();
          QT = AT->getElementType();
          if (QT.getTypePtr()->isBuiltinType()) {
            QT = QT.getCanonicalType();
            const BuiltinType *BT = dyn_cast<BuiltinType>(QT);
            if (BT) {
              LangOptions LO;
              LO.CUDA = true;
              PrintingPolicy policy(LO);
              typeName = BT->getName(policy);
            }
          } else {
            typeName = QT.getAsString();
          }
        }
        if (!typeName.empty()) {
          SourceLocation slStart = sharedVar->getLocStart();
          SourceLocation slEnd = sharedVar->getLocEnd();
          SourceManager *SM = Result.SourceManager;
          size_t repLength = SM->getCharacterData(slEnd) - SM->getCharacterData(slStart) + 1;
          std::string varName = sharedVar->getNameAsString();
          std::string repName = "HIP_DYNAMIC_SHARED(" + typeName + ", " + varName + ")";
          Replacement Rep(*SM, slStart, repLength, repName);
          FullSourceLoc fullSL(slStart, *SM);
          insertReplacement(Rep, fullSL);
          hipCounter counter = { "HIP_DYNAMIC_SHARED", ConvTypes::CONV_MEM, ApiTypes::API_RUNTIME };
          Statistics::current().incrementCounter(counter, refName.str());
        }
      }
      return true;
    }
    return false;
  }

  bool stringLiteral(const MatchFinder::MatchResult &Result) {
    if (const clang::StringLiteral *sLiteral = Result.Nodes.getNodeAs<clang::StringLiteral>("stringLiteral")) {
      if (sLiteral->getCharByteWidth() == 1) {
        StringRef s = sLiteral->getString();
        SourceManager *SM = Result.SourceManager;
        processString(s, *SM, sLiteral->getLocStart());
      }
      return true;
    }
    return false;
  }

public:
  Cuda2HipCallback(Replacements& Replace, ast_matchers::MatchFinder *parent, HipifyPPCallbacks *PPCallbacks, const std::string &mainFileName)
    : Cuda2Hip(Replace, mainFileName), owner(parent), PP(PPCallbacks) {
    PP->setMatch(this);
  }

  void run(const MatchFinder::MatchResult &Result) override {
    if (cudaType(Result)) return;
    if (cudaCall(Result)) return;
    if (cudaBuiltin(Result)) return;
    if (cudaEnumConstantRef(Result)) return;
    if (cudaLaunchKernel(Result)) return;
    if (cudaSharedIncompleteArrayVar(Result)) return;
    if (stringLiteral(Result)) return;
  }

private:
  ast_matchers::MatchFinder *owner;
  HipifyPPCallbacks *PP;
};

void HipifyPPCallbacks::handleEndSource() {
  insertHipHeaders(Match, *_sm);
}

void addAllMatchers(ast_matchers::MatchFinder &Finder, Cuda2HipCallback *Callback) {
  // Rewrite CUDA api calls to hip ones.
  Finder.addMatcher(
      callExpr(
          isExpansionInMainFile(),
          callee(
              functionDecl(
                  matchesName("cu.*"),
                  unless(
                      // Clang generates structs with functions on them to represent things like
                      // threadIdx.x. We have other logic to handle those builtins directly, so
                      // we need to suppress the call-handling.
                      // We can't handle those directly in the call-handler without special-casing
                      // it unpleasantly, since the names of the functions are unique only per-struct.
                      matchesName("__fetch_builtin.*")
                  )
              )
          )
      ).bind("cudaCall"),
      Callback
  );

  // Rewrite all references to CUDA types to their corresponding hip types.
  Finder.addMatcher(
      typeLoc(
          isExpansionInMainFile()
      ).bind("cudaType"),
      Callback
  );

  // Replace references to CUDA names in string literals with the equivalent hip names.
  Finder.addMatcher(stringLiteral(isExpansionInMainFile()).bind("stringLiteral"), Callback);

  // Replace the <<<...>>> language extension with a hip kernel launch
  Finder.addMatcher(cudaKernelCallExpr(isExpansionInMainFile()).bind("cudaLaunchKernel"), Callback);

  // Replace cuda builtins.
  Finder.addMatcher(
      memberExpr(
          isExpansionInMainFile(),
          hasObjectExpression(
              hasType(
                  cxxRecordDecl(
                      matchesName("__cuda_builtin_")
                  )
              )
          )
      ).bind("cudaBuiltin"),
      Callback
  );

  // Map CUDA enum _values_ to their hip equivalents.
  Finder.addMatcher(
      declRefExpr(
          isExpansionInMainFile(),
          to(
              enumConstantDecl(
                  matchesName("cu.*|CU.*")
              )
          )
      ).bind("cudaEnumConstantRef"),
      Callback
  );

  Finder.addMatcher(
      varDecl(
          isExpansionInMainFile(),
          allOf(
              hasAttr(attr::CUDAShared),
              hasType(incompleteArrayType())
          )
      ).bind("cudaSharedIncompleteArrayVar"),
      Callback
  );
}

void copyFile(const std::string& src, const std::string& dst) {
  std::ifstream source(src, std::ios::binary);
  std::ofstream dest(dst, std::ios::binary);
  dest << source.rdbuf();
}

int main(int argc, const char **argv) {
  llcompat::PrintStackTraceOnErrorSignal();

  CommonOptionsParser OptionsParser(argc, argv, ToolTemplateCategory, llvm::cl::OneOrMore);
  std::vector<std::string> fileSources = OptionsParser.getSourcePathList();
  std::string dst = OutputFilename;
  if (!dst.empty() && fileSources.size() > 1) {
    llvm::errs() << "[HIPIFY] conflict: -o and multiple source files are specified.\n";
    return 1;
  }

  if (NoOutput) {
    if (Inplace) {
      llvm::errs() << "[HIPIFY] conflict: both -no-output and -inplace options are specified.\n";
      return 1;
    }
    if (!dst.empty()) {
      llvm::errs() << "[HIPIFY] conflict: both -no-output and -o options are specified.\n";
      return 1;
    }
  }

  if (Examine) {
    NoOutput = PrintStats = true;
  }

  int Result = 0;

  // Arguments for the Statistics print routines.
  std::unique_ptr<std::ostream> csv = nullptr;
  llvm::raw_ostream* statPrint = nullptr;
  if (!OutputStatsFilename.empty()) {
    csv = std::unique_ptr<std::ostream>(new std::ofstream(OutputStatsFilename, std::ios_base::trunc));
  }
  if (PrintStats) {
    statPrint = &llvm::errs();
  }

  for (const auto & src : fileSources) {
    if (dst.empty()) {
      if (Inplace) {
        dst = src;
      } else {
        dst = src + ".hip";
      }
    } else if (Inplace) {
      llvm::errs() << "[HIPIFY] conflict: both -o and -inplace options are specified.\n";
      return 1;
    }

    std::string tmpFile = src + ".hipify-tmp";

    // Create a copy of the file to work on. When we're done, we'll move this onto the
    // output (which may mean overwriting the input, if we're in-place).
    // Should we fail for some reason, we'll just leak this file and not corrupt the input.
    copyFile(src, tmpFile);

    // Initialise the statistics counters for this file.
    Statistics::setActive(src);

    // RefactoringTool operates on the file in-place. Giving it the output path is no good,
    // because that'll break relative includes, and we don't want to overwrite the input file.
    // So what we do is operate on a copy, which we then move to the output.
    RefactoringTool Tool(OptionsParser.getCompilations(), tmpFile);
    ast_matchers::MatchFinder Finder;

    // The Replacements to apply to the file `src`.
    Replacements& replacementsToUse = llcompat::getReplacements(Tool, tmpFile);
    HipifyPPCallbacks* PPCallbacks = new HipifyPPCallbacks(replacementsToUse, tmpFile);
    Cuda2HipCallback Callback(replacementsToUse, &Finder, PPCallbacks, tmpFile);

    addAllMatchers(Finder, &Callback);

    auto action = newFrontendActionFactory(&Finder, PPCallbacks);

    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster("--cuda-host-only", ArgumentInsertPosition::BEGIN));

    // Ensure at least c++11 is used.
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster("-std=c++11", ArgumentInsertPosition::BEGIN));
#if defined(HIPIFY_CLANG_RES)
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster("-resource-dir=" HIPIFY_CLANG_RES));
#endif
    Tool.appendArgumentsAdjuster(getClangSyntaxOnlyAdjuster());
    Result += Tool.run(action.get());
    Tool.clearArgumentsAdjusters();

    LangOptions DefaultLangOptions;
    IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
    TextDiagnosticPrinter DiagnosticPrinter(llvm::errs(), &*DiagOpts);
    DiagnosticsEngine Diagnostics(IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs()), &*DiagOpts, &DiagnosticPrinter, false);

    SourceManager SM(Diagnostics, Tool.getFiles());

    Rewriter Rewrite(SM, DefaultLangOptions);
    if (!Tool.applyAllReplacements(Rewrite)) {
      DEBUG(dbgs() << "Skipped some replacements.\n");
    }

    // Either move the tmpfile to the output, or remove it.
    if (!NoOutput) {
      Result += Rewrite.overwriteChangedFiles();
      rename(tmpFile.c_str(), dst.c_str());
    } else {
      remove(tmpFile.c_str());
    }

    Statistics::current().markCompletion();
    Statistics::current().print(csv.get(), statPrint);

    dst.clear();
  }

  if (fileSources.size() > 1) {
    Statistics::printAggregate(csv.get(), statPrint);
  }

  return Result;
}

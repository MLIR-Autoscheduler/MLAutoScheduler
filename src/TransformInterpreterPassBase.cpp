//===- TransformInterpreterPassBase.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Base class with shared implementation for transform dialect interpreter
// passes.
//
//===----------------------------------------------------------------------===//

#include "TransformInterpreterPassBase.h"
#include "mlir/Dialect/Transform/IR/TransformDialect.h"
#include "mlir/Dialect/Transform/IR/TransformInterfaces.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/FunctionInterfaces.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/Visitors.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/FileUtilities.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

using namespace mlir;

#define DEBUG_TYPE "transform-dialect-interpreter"
#define DBGS() (llvm::dbgs() << "[" DEBUG_TYPE << "]: ")
#define DEBUG_TYPE_DUMP_STDERR "transform-dialect-dump-repro"
#define DEBUG_TYPE_DUMP_FILE "transform-dialect-save-repro"

/// Name of the attribute used for targeting the transform dialect interpreter
/// at specific operations.
constexpr static llvm::StringLiteral kTransformDialectTagAttrName =
    "transform.target_tag";
/// Value of the attribute indicating the root payload operation.
constexpr static llvm::StringLiteral kTransformDialectTagPayloadRootValue =
    "payload_root";
/// Value of the attribute indicating the container of transform operations
/// (containing the top-level transform operation).
constexpr static llvm::StringLiteral
    kTransformDialectTagTransformContainerValue = "transform_container";

/// Utility to parse the content of a `transformFileName` MLIR file containing
/// a transform dialect specification.
static mlir::LogicalResult
parseTransformModuleFromFile(mlir::MLIRContext *context,
                             llvm::StringRef transformFileName,
                             mlir::OwningOpRef<mlir::ModuleOp> &transformModule)
{
  if (transformFileName.empty())
  {
    LLVM_DEBUG(
        DBGS() << "no transform file name specified, assuming the transform "
                  "module is embedded in the IR next to the top-level\n");
    return success();
  }
  // Parse transformFileName content into a ModuleOp.
  std::string errorMessage;
  auto memoryBuffer = mlir::openInputFile(transformFileName, &errorMessage);
  if (!memoryBuffer)
  {
    return emitError(FileLineColLoc::get(
               StringAttr::get(context, transformFileName), 0, 0))
           << "failed to parse transform file";
  }
  // Tell sourceMgr about this buffer, the parser will pick it up.
  llvm::SourceMgr sourceMgr;
  sourceMgr.AddNewSourceBuffer(std::move(memoryBuffer), llvm::SMLoc());
  transformModule =
      mlir::OwningOpRef<mlir::ModuleOp>(parseSourceFile<mlir::ModuleOp>(sourceMgr, context));
  return success();
}

/// Finds the single top-level transform operation with `root` as ancestor.
/// Reports an error if there is more than one such operation and returns the
/// first one found. Reports an error returns nullptr if no such operation
/// found.
static mlir::Operation *findTopLevelTransform(mlir::Operation *root,
                                              llvm::StringRef filenameOption)
{
  ::mlir::transform::TransformOpInterface topLevelTransform = nullptr;
  WalkResult walkResult = root->walk<WalkOrder::PreOrder>(
      [&](::mlir::transform::TransformOpInterface transformOp)
      {
        if (!transformOp
                 ->hasTrait<transform::PossibleTopLevelTransformOpTrait>())
          return WalkResult::skip();
        if (!topLevelTransform)
        {
          topLevelTransform = transformOp;
          return WalkResult::skip();
        }
        auto diag = transformOp.emitError()
                    << "more than one top-level transform op";
        diag.attachNote(topLevelTransform.getLoc())
            << "previous top-level transform op";
        return WalkResult::interrupt();
      });
  if (walkResult.wasInterrupted())
    return nullptr;
  if (!topLevelTransform)
  {
    auto diag = root->emitError()
                << "could not find a nested top-level transform op";
    diag.attachNote() << "use the '" << filenameOption
                      << "' option to provide transform as external file";
    return nullptr;
  }
  return topLevelTransform;
}

/// Finds an operation nested in `root` that has the transform dialect tag
/// attribute with the value specified as `tag`. Assumes only one operation
/// may have the tag. Returns nullptr if there is no such operation.
static mlir::Operation *findOpWithTag(mlir::Operation *root, StringRef tagKey,
                                      StringRef tagValue)
{
  mlir::Operation *found = nullptr;
  WalkResult walkResult = root->walk<WalkOrder::PreOrder>(
      [tagKey, tagValue, &found, root](mlir::Operation *op)
      {
        auto attr = op->getAttrOfType<StringAttr>(tagKey);
        if (!attr || attr.getValue() != tagValue)
          return WalkResult::advance();

        if (found)
        {
          InFlightDiagnostic diag = root->emitError()
                                    << "more than one operation with " << tagKey
                                    << "=\"" << tagValue << "\" attribute";
          diag.attachNote(found->getLoc()) << "first operation";
          diag.attachNote(op->getLoc()) << "other operation";
          return WalkResult::interrupt();
        }

        found = op;
        return WalkResult::advance();
      });
  if (walkResult.wasInterrupted())
    return nullptr;

  if (!found)
  {
    root->emitError() << "could not find the operation with " << tagKey << "=\""
                      << tagValue << "\" attribute";
  }
  return found;
}

/// Returns the ancestor of `target` that doesn't have a parent.
static mlir::Operation *getRootOperation(mlir::Operation *target)
{
  mlir::Operation *root = target;
  while (root->getParentOp())
    root = root->getParentOp();
  return root;
}

/// Prints the CLI command running the repro with the current path.
// TODO: make binary name optional by querying LLVM command line API for the
// name of the current binary.
static llvm::raw_ostream &
printReproCall(llvm::raw_ostream &os, StringRef rootOpName, StringRef passName,
               const Pass::Option<std::string> &debugPayloadRootTag,
               const Pass::Option<std::string> &debugTransformRootTag,
               const Pass::Option<std::string> &transformLibraryFileName,
               StringRef binaryName)
{
  std::string transformLibraryOption = "";
  if (!transformLibraryFileName.empty())
  {
    transformLibraryOption =
        llvm::formatv(" {0}={1}", transformLibraryFileName.getArgStr(),
                      transformLibraryFileName.getValue())
            .str();
  }
  os << llvm::formatv(
      "{7} --pass-pipeline=\"{0}({1}{{{2}={3} {4}={5}{6}})\"", rootOpName,
      passName, debugPayloadRootTag.getArgStr(),
      debugPayloadRootTag.empty()
          ? StringRef(kTransformDialectTagPayloadRootValue)
          : debugPayloadRootTag,
      debugTransformRootTag.getArgStr(),
      debugTransformRootTag.empty()
          ? StringRef(kTransformDialectTagTransformContainerValue)
          : debugTransformRootTag,
      transformLibraryOption, binaryName);
  return os;
}

/// Prints the module rooted at `root` to `os` and appends
/// `transformContainer` if it is not nested in `root`.
llvm::raw_ostream &printModuleForRepro(llvm::raw_ostream &os, mlir::Operation *root,
                                       mlir::Operation *transform)
{
  root->print(os);
  if (!root->isAncestor(transform))
    transform->print(os);
  return os;
}

/// Saves the payload and the transform IR into a temporary file and reports
/// the file name to `os`.
void saveReproToTempFile(
    llvm::raw_ostream &os, mlir::Operation *target, mlir::Operation *transform,
    StringRef passName, const Pass::Option<std::string> &debugPayloadRootTag,
    const Pass::Option<std::string> &debugTransformRootTag,
    const Pass::Option<std::string> &transformLibraryFileName,
    StringRef binaryName)
{
  using llvm::sys::fs::TempFile;
  mlir::Operation *root = getRootOperation(target);

  SmallVector<char, 128> tmpPath;
  llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/true, tmpPath);
  llvm::sys::path::append(tmpPath, "transform_dialect_%%%%%%.mlir");
  llvm::Expected<TempFile> tempFile = TempFile::create(tmpPath);
  if (!tempFile)
  {
    os << "could not open temporary file to save the repro\n";
    return;
  }

  llvm::raw_fd_ostream fout(tempFile->FD, /*shouldClose=*/false);
  printModuleForRepro(fout, root, transform);
  fout.flush();
  std::string filename = tempFile->TmpName;

  if (tempFile->keep())
  {
    os << "could not preserve the temporary file with the repro\n";
    return;
  }

  os << "=== Transform Interpreter Repro ===\n";
  printReproCall(os, root->getName().getStringRef(), passName,
                 debugPayloadRootTag, debugTransformRootTag,
                 transformLibraryFileName, binaryName)
      << " " << filename << "\n";
  os << "===================================\n";
}

// Optionally perform debug actions requested by the user to dump IR and a
// repro to stderr and/or a file.
static void performOptionalDebugActions(
    mlir::Operation *target, mlir::Operation *transform, StringRef passName,
    const Pass::Option<std::string> &debugPayloadRootTag,
    const Pass::Option<std::string> &debugTransformRootTag,
    const Pass::Option<std::string> &transformLibraryFileName,
    StringRef binaryName)
{
  mlir::MLIRContext *context = target->getContext();

  // If we are not planning to print, bail early.
  bool hasDebugFlags = false;
  DEBUG_WITH_TYPE(DEBUG_TYPE_DUMP_STDERR, { hasDebugFlags = true; });
  DEBUG_WITH_TYPE(DEBUG_TYPE_DUMP_FILE, { hasDebugFlags = true; });
  if (!hasDebugFlags)
    return;

  // We will be mutating the IR to set attributes. If this is running
  // concurrently on several parts of a container or using a shared transform
  // script, this would create a race. Bail in multithreaded mode and require
  // the user to disable threading to dump repros.
  static llvm::sys::SmartMutex<true> dbgStreamMutex;
  if (target->getContext()->isMultithreadingEnabled())
  {
    llvm::sys::SmartScopedLock<true> lock(dbgStreamMutex);
    llvm::dbgs() << "=======================================================\n";
    llvm::dbgs() << "|      Transform reproducers cannot be produced       |\n";
    llvm::dbgs() << "|              in multi-threaded mode!                |\n";
    llvm::dbgs() << "=======================================================\n";
    return;
  }

  mlir::Operation *root = getRootOperation(target);

  // Add temporary debug / repro attributes, these must never leak out.
  if (debugPayloadRootTag.empty())
  {
    target->setAttr(
        kTransformDialectTagAttrName,
        StringAttr::get(context, kTransformDialectTagPayloadRootValue));
  }
  if (debugTransformRootTag.empty())
  {
    transform->setAttr(
        kTransformDialectTagAttrName,
        StringAttr::get(context, kTransformDialectTagTransformContainerValue));
  }

  DEBUG_WITH_TYPE(DEBUG_TYPE_DUMP_STDERR, {
    llvm::dbgs() << "=== Transform Interpreter Repro ===\n";
    printReproCall(llvm::dbgs() << "cat <<EOF | ",
                   root->getName().getStringRef(), passName,
                   debugPayloadRootTag, debugTransformRootTag,
                   transformLibraryFileName, binaryName)
        << "\n";
    printModuleForRepro(llvm::dbgs(), root, transform);
    llvm::dbgs() << "\nEOF\n";
    llvm::dbgs() << "===================================\n";
  });
  (void)root;
  DEBUG_WITH_TYPE(DEBUG_TYPE_DUMP_FILE, {
    saveReproToTempFile(llvm::dbgs(), target, transform, passName,
                        debugPayloadRootTag, debugTransformRootTag,
                        transformLibraryFileName, binaryName);
  });

  // Remove temporary attributes if they were set.
  if (debugPayloadRootTag.empty())
    target->removeAttr(kTransformDialectTagAttrName);
  if (debugTransformRootTag.empty())
    transform->removeAttr(kTransformDialectTagAttrName);
}

/// Replaces external symbols in `block` with their (non-external) definitions
/// from the given module.
static mlir::LogicalResult defineDeclaredSymbols(Block &block, mlir::ModuleOp definitions)
{
  mlir::MLIRContext &ctx = *definitions->getContext();
  auto consumedName =
      StringAttr::get(&ctx, transform::TransformDialect::kArgConsumedAttrName);
  auto readOnlyName =
      StringAttr::get(&ctx, transform::TransformDialect::kArgReadOnlyAttrName);

  for (mlir::Operation &op : llvm::make_early_inc_range(block))
  {
    LLVM_DEBUG(DBGS() << op << "\n");
    auto symbol = dyn_cast<SymbolOpInterface>(op);
    if (!symbol)
      continue;
    if (symbol->getNumRegions() == 1 && !symbol->getRegion(0).empty())
      continue;

    LLVM_DEBUG(DBGS() << "looking for definition of symbol "
                      << symbol.getNameAttr() << ":");
    SymbolTable symbolTable(definitions);
    mlir::Operation *externalSymbol = symbolTable.lookup(symbol.getNameAttr());
    if (!externalSymbol || externalSymbol->getNumRegions() != 1 ||
        externalSymbol->getRegion(0).empty())
    {
      LLVM_DEBUG(llvm::dbgs() << "not found\n");
      continue;
    }

    auto symbolFunc = dyn_cast<FunctionOpInterface>(op);
    auto externalSymbolFunc = dyn_cast<FunctionOpInterface>(externalSymbol);
    if (!symbolFunc || !externalSymbolFunc)
    {
      LLVM_DEBUG(llvm::dbgs() << "cannot compare types\n");
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "found @" << externalSymbol << "\n");
    if (symbolFunc.getFunctionType() != externalSymbolFunc.getFunctionType())
    {
      return symbolFunc.emitError()
             << "external definition has a mismatching signature ("
             << externalSymbolFunc.getFunctionType() << ")";
    }

    for (unsigned i = 0, e = symbolFunc.getNumArguments(); i < e; ++i)
    {
      bool isExternalConsumed =
          externalSymbolFunc.getArgAttr(i, consumedName) != nullptr;
      bool isExternalReadonly =
          externalSymbolFunc.getArgAttr(i, readOnlyName) != nullptr;
      bool isConsumed = symbolFunc.getArgAttr(i, consumedName) != nullptr;
      bool isReadonly = symbolFunc.getArgAttr(i, readOnlyName) != nullptr;
      if (!isExternalConsumed && !isExternalReadonly)
      {
        if (isConsumed)
          externalSymbolFunc.setArgAttr(i, consumedName, UnitAttr::get(&ctx));
        else if (isReadonly)
          externalSymbolFunc.setArgAttr(i, readOnlyName, UnitAttr::get(&ctx));
        continue;
      }

      if ((isExternalConsumed && !isConsumed) ||
          (isExternalReadonly && !isReadonly))
      {
        return symbolFunc.emitError()
               << "external definition has mismatching consumption annotations "
                  "for argument #"
               << i;
      }
    }

    OpBuilder builder(&op);
    builder.setInsertionPoint(&op);
    builder.clone(*externalSymbol);
    symbol->erase();
  }

  return success();
}

mlir::LogicalResult interpreterBaseRunOnOperationImpl(
    mlir::Operation *target, StringRef passName,
    const std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &sharedTransformModule,
    const std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &libraryModule,
    const RaggedArray<mlir::transform::MappedValue> &extraMappings,
    const mlir::transform::TransformOptions &options,
    const Pass::Option<std::string> &transformFileName,
    const Pass::Option<std::string> &transformLibraryFileName,
    const Pass::Option<std::string> &debugPayloadRootTag,
    const Pass::Option<std::string> &debugTransformRootTag,
    StringRef binaryName)
{

  // Step 1
  // ------
  // If debugPayloadRootTag was passed, then we are in user-specified selection
  // of the transformed IR. This corresponds to REPL debug mode. Otherwise, just
  // apply to `target`.
  mlir::Operation *payloadRoot = target;
  if (!debugPayloadRootTag.empty())
  {
    payloadRoot = findOpWithTag(target, kTransformDialectTagAttrName,
                                debugPayloadRootTag);
    if (!payloadRoot)
      return failure();
  }

  // Step 2
  // ------
  // If a shared transform was specified separately, use it. Otherwise, the
  // transform is embedded in the payload IR. If debugTransformRootTag was
  // passed, then we are in user-specified selection of the transforming IR.
  // This corresponds to REPL debug mode.
  bool sharedTransform = (sharedTransformModule && *sharedTransformModule);
  mlir::Operation *transformContainer =
      sharedTransform ? sharedTransformModule->get() : target;
  mlir::Operation *transformRoot =
      debugTransformRootTag.empty()
          ? findTopLevelTransform(transformContainer,
                                  transformFileName.getArgStr())
          : findOpWithTag(transformContainer, kTransformDialectTagAttrName,
                          debugTransformRootTag);
  if (!transformRoot)
    return failure();

  if (!transformRoot->hasTrait<mlir::transform::PossibleTopLevelTransformOpTrait>())
  {
    return emitError(transformRoot->getLoc())
           << "expected the transform entry point to be a top-level transform "
              "op";
  }

  // Step 3
  // ------
  // Copy external defintions for symbols if provided. Be aware of potential
  // concurrent execution (normally, the error shouldn't be triggered unless the
  // transform IR modifies itself in a pass, which is also forbidden elsewhere).
  if (!sharedTransform && libraryModule && *libraryModule)
  {
    if (!target->isProperAncestor(transformRoot))
    {
      InFlightDiagnostic diag =
          transformRoot->emitError()
          << "cannot inject transform definitions next to pass anchor op";
      diag.attachNote(target->getLoc()) << "pass anchor op";
      return diag;
    }
    if (failed(defineDeclaredSymbols(*transformRoot->getBlock(),
                                     libraryModule->get())))
      return failure();
  }

  // Step 4
  // ------
  // Optionally perform debug actions requested by the user to dump IR and a
  // repro to stderr and/or a file.
  performOptionalDebugActions(target, transformRoot, passName,
                              debugPayloadRootTag, debugTransformRootTag,
                              transformLibraryFileName, binaryName);

  // Step 5
  // ------
  // Apply the transform to the IR
  return mlir::transform::applyTransforms(payloadRoot, cast<mlir::transform::TransformOpInterface>(transformRoot),
                                          extraMappings, options);
}

mlir::LogicalResult interpreterBaseRunOnOperationImplModified(
    mlir::Operation *target, StringRef passName,
    const std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &sharedTransformModule,
    const std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &libraryModule,
    const RaggedArray<transform::MappedValue> &extraMappings,
    const transform::TransformOptions &options,
    const Pass::Option<std::string> &transformFileName,
    const Pass::Option<std::string> &transformLibraryFileName,
    const Pass::Option<std::string> &debugPayloadRootTag,
    const Pass::Option<std::string> &debugTransformRootTag,
    StringRef binaryName)
{

  // Step 1
  // ------
  // If debugPayloadRootTag was passed, then we are in user-specified selection
  // of the transformed IR. This corresponds to REPL debug mode. Otherwise, just
  // apply to `target`.

  mlir::Operation *payloadRoot = target;
  if (!debugPayloadRootTag.empty())
  {
    payloadRoot = findOpWithTag(target, kTransformDialectTagAttrName,
                                debugPayloadRootTag);
    if (!payloadRoot)
      return failure();
  }

  // Step 2
  // ------
  // If a shared transform was specified separately, use it. Otherwise, the
  // transform is embedded in the payload IR. If debugTransformRootTag was
  // passed, then we are in user-specified selection of the transforming IR.
  // This corresponds to REPL debug mode.
  bool sharedTransform = (sharedTransformModule && *sharedTransformModule);
  mlir::Operation *transformContainer =
      sharedTransform ? sharedTransformModule->get() : target;
  mlir::Operation *transformRoot =
      debugTransformRootTag.empty()
          ? findTopLevelTransform(transformContainer,
                                  transformFileName.getArgStr())
          : findOpWithTag(transformContainer, kTransformDialectTagAttrName,
                          debugTransformRootTag);
  if (!transformRoot)
    return failure();

  if (!transformRoot->hasTrait<mlir::transform::PossibleTopLevelTransformOpTrait>())
  {
    return emitError(transformRoot->getLoc())
           << "expected the transform entry point to be a top-level transform "
              "op";
  }

  // Step 3
  // ------
  // Copy external defintions for symbols if provided. Be aware of potential
  // concurrent execution (normally, the error shouldn't be triggered unless the
  // transform IR modifies itself in a pass, which is also forbidden elsewhere).
  if (!sharedTransform && libraryModule && *libraryModule)
  {
    if (!target->isProperAncestor(transformRoot))
    {
      InFlightDiagnostic diag =
          transformRoot->emitError()
          << "cannot inject transform definitions next to pass anchor op";
      diag.attachNote(target->getLoc()) << "pass anchor op";
      return diag;
    }
    if (failed(defineDeclaredSymbols(*transformRoot->getBlock(),
                                     libraryModule->get())))
      return failure();
  }

  // Step 4
  // ------
  // Optionally perform debug actions requested by the user to dump IR and a
  // repro to stderr and/or a file.
  performOptionalDebugActions(target, transformRoot, passName,
                              debugPayloadRootTag, debugTransformRootTag,
                              transformLibraryFileName, binaryName);

  // Step 5
  // ------
  // Apply the transform to the IR
  return mlir::transform::applyTransforms(payloadRoot, cast<mlir::transform::TransformOpInterface>(transformRoot),
                                          extraMappings, options);
}

mlir::LogicalResult interpreterBaseInitializeImpl(
    mlir::MLIRContext *context, StringRef transformFileName,
    StringRef transformLibraryFileName,
    std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &module,
    std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &libraryModule,
    function_ref<std::optional<mlir::LogicalResult>(mlir::OpBuilder &, mlir::Location)>
        moduleBuilder)
{
  mlir::OwningOpRef<mlir::ModuleOp> parsed;
  if (failed(parseTransformModuleFromFile(context, transformFileName, parsed)))
    return failure();
  if (parsed && failed(mlir::verify(*parsed)))
    return failure();

  mlir::OwningOpRef<mlir::ModuleOp> parsedLibrary;
  if (failed(parseTransformModuleFromFile(context, transformLibraryFileName,
                                          parsedLibrary)))
    return failure();
  if (parsedLibrary && failed(mlir::verify(*parsedLibrary)))
    return failure();

  if (parsed)
  {
    module = std::make_shared<mlir::OwningOpRef<mlir::ModuleOp>>(std::move(parsed));
  }
  else if (moduleBuilder)
  {
    // TODO: better location story.
    auto location = UnknownLoc::get(context);
    auto localModule = std::make_shared<mlir::OwningOpRef<mlir::ModuleOp>>(
        mlir::ModuleOp::create(location, "__transform"));

    OpBuilder b(context);
    b.setInsertionPointToEnd(localModule->get().getBody());
    if (std::optional<mlir::LogicalResult> result = moduleBuilder(b, location))
    {
      if (failed(*result))
        return failure();
      module = std::move(localModule);
    }
  }

  if (!parsedLibrary || !*parsedLibrary)
    return success();

  if (module && *module)
  {
    if (failed(defineDeclaredSymbols(*module->get().getBody(),
                                     parsedLibrary.get())))
      return failure();
  }
  else
  {
    libraryModule =
        std::make_shared<mlir::OwningOpRef<mlir::ModuleOp>>(std::move(parsedLibrary));
  }
  return success();
}

mlir::LogicalResult interpreterBaseInitializeImplModified(
    mlir::MLIRContext *context, StringRef transformFileName,
    StringRef transformLibraryFileName,
    std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &module,
    std::shared_ptr<mlir::OwningOpRef<mlir::ModuleOp>> &libraryModule)
{
  mlir::OwningOpRef<mlir::ModuleOp> parsed = mlir::OwningOpRef<mlir::ModuleOp>(parseSourceString<mlir::ModuleOp>(transformFileName, context));
  /*if (failed())
    return failure();*/
  if (parsed && failed(mlir::verify(*parsed)))
    return failure();
  auto start = std::chrono::high_resolution_clock::now();
  /// CHANGE HERE TO GET MODULE DIRECTLY
  mlir::OwningOpRef<mlir::ModuleOp> parsedLibrary;
  if (failed(parseTransformModuleFromFile(context, transformLibraryFileName,
                                          parsedLibrary)))
    return failure();
  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  std::cout << "Time taken by Parsing string: " << duration.count() << " microseconds" << std::endl;
  if (parsedLibrary && failed(mlir::verify(*parsedLibrary)))
    return failure();

  if (parsed)
  {
    module = std::make_shared<mlir::OwningOpRef<mlir::ModuleOp>>(std::move(parsed));
  }

  if (!parsedLibrary || !*parsedLibrary)
    return success();

  if (module && *module)
  {
    if (failed(defineDeclaredSymbols(*module->get().getBody(),
                                     parsedLibrary.get())))
      return failure();
  }
  else
  {
    libraryModule =
        std::make_shared<mlir::OwningOpRef<mlir::ModuleOp>>(std::move(parsedLibrary));
  }
  return success();
}
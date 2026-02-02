#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Tooling.h"
#include "clang/AST/Comment.h"
#include "clang/AST/RawCommentList.h"
#include "clang/Basic/FileEntry.h"
#include "llvm/Support/MemoryBuffer.h"
#include "nlohmann/json.hpp"
#include "pugixml.hpp"
#include "yaml-cpp/yaml.h"
#include "cxxopts.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <regex>

// #define DEBUG

namespace fs = std::filesystem;
using json   = nlohmann::json;
using namespace clang;
using namespace clang::tooling;

static YAML::Node functions              = {};
static YAML::Node variables              = {};
static YAML::Node types                  = {};
static YAML::Node headers                = {};
static std::string current_function_name = "";
std::string module_filename;
std::string output_filename;
std::filesystem::path module_filepath;
bool analyze_all_files      = false;
bool analyze_function_calls = false;
bool analyze_docs           = false;
bool analyze_includes       = false;
bool process_as_header_file = false;

// AST Visitor
class YamlyzeVisitor : public RecursiveASTVisitor<YamlyzeVisitor> {
private:
  ASTContext *Context;
  SourceManager *SM;

public:
  explicit YamlyzeVisitor(ASTContext *Context) 
    : Context(Context), SM(&Context->getSourceManager()) {}

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldWalkTypesOfTypeLocs() const { return false; }

  bool VisitFunctionDecl(FunctionDecl *FD) {
    // Skip if it's in a system header or not in the file we want
    SourceLocation Loc = FD->getLocation();
    if (!Loc.isValid() || SM->isInSystemHeader(Loc))
      return true;
    
    if (!analyze_all_files) {
      FileID FID = SM->getFileID(Loc);
      OptionalFileEntryRef FE = SM->getFileEntryRefForID(FID);
      if (!FE)
        return true;
      
      std::filesystem::path current_file = std::filesystem::weakly_canonical(std::string(FE->getName()));
      if (module_filepath.compare(current_file) != 0)
        return true;
    }

    // Skip forward declarations unless we're processing as a header
    if (!process_as_header_file && !FD->hasBody())
      return true;

    std::string function_name = FD->getNameAsString();
    current_function_name = function_name;

    // Get storage class
    std::string storage_class = "normal";
    switch (FD->getStorageClass()) {
      case SC_Static: storage_class = "static"; break;
      case SC_Extern: storage_class = "extern"; break;
      default: break;
    }

    // Get return type
    QualType ReturnType = FD->getReturnType();
    std::string return_type = ReturnType.getAsString();

    // Create function YAML object
    functions[current_function_name]["class"]   = storage_class;
    functions[current_function_name]["args"]    = {};
    functions[current_function_name]["calls"]   = {};
    functions[current_function_name]["docs"]    = {};
    functions[current_function_name]["returns"] = return_type;

    // Process parameters
    for (unsigned i = 0; i < FD->getNumParams(); ++i) {
      const ParmVarDecl *Param = FD->getParamDecl(i);
      
      YAML::Node new_arg;
      new_arg["name"] = Param->getNameAsString();
      new_arg["type"] = Param->getType().getAsString();
      
      // Get size (similar to clang_Type_getSizeOf)
      QualType ParamType = Param->getType();
      if (!ParamType->isIncompleteType() && !ParamType->isDependentType()) {
        int64_t size = Context->getTypeSize(ParamType) / 8; // Convert bits to bytes
        new_arg["size"] = static_cast<int>(size);
      } else {
        new_arg["size"] = -1;
      }
      
      functions[current_function_name]["args"].push_back(new_arg);
    }

    // Process Doxygen comments (simplified version)
    if (analyze_docs) {
      const RawComment *RC = Context->getRawCommentForDeclNoCache(FD);
      if (RC) {
        std::string CommentText = RC->getFormattedText(*SM, Context->getDiagnostics());
        functions[current_function_name]["docs"]["raw"] = CommentText;
      }
    }

    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (!analyze_function_calls || current_function_name.empty())
      return true;

    const FunctionDecl *Callee = CE->getDirectCallee();
    if (Callee) {
      functions[current_function_name]["calls"].push_back(Callee->getNameAsString());
    }
    
    return true;
  }

  bool VisitVarDecl(VarDecl *VD) {
    // Only process global variables
    if (!VD->hasGlobalStorage())
      return true;

    SourceLocation Loc = VD->getLocation();
    if (!Loc.isValid() || SM->isInSystemHeader(Loc))
      return true;

    if (!analyze_all_files) {
      FileID FID = SM->getFileID(Loc);
      OptionalFileEntryRef FE = SM->getFileEntryRefForID(FID);
      if (!FE)
        return true;
      
      std::filesystem::path current_file = std::filesystem::weakly_canonical(std::string(FE->getName()));
      if (module_filepath.compare(current_file) != 0)
        return true;
    }

    std::string var_name = VD->getNameAsString();
    
    std::string storage_class = "global";
    switch (VD->getStorageClass()) {
      case SC_Static: storage_class = "static"; break;
      case SC_Extern: storage_class = "extern"; break;
      default: break;
    }

    // Only add variables at translation unit level
    if (VD->getDeclContext()->isTranslationUnit()) {
      variables[var_name]["class"] = storage_class;
    }
    
    variables[var_name]["type"] = VD->getType().getAsString();

    return true;
  }

  bool VisitTypedefDecl(TypedefDecl *TD) {
    SourceLocation Loc = TD->getLocation();
    if (!Loc.isValid() || SM->isInSystemHeader(Loc))
      return true;

    if (!analyze_all_files) {
      FileID FID = SM->getFileID(Loc);
      OptionalFileEntryRef FE = SM->getFileEntryRefForID(FID);
      if (!FE)
        return true;
      
      std::filesystem::path current_file = std::filesystem::weakly_canonical(std::string(FE->getName()));
      if (module_filepath.compare(current_file) != 0)
        return true;
    }

    std::string name = TD->getNameAsString();
    QualType UnderlyingType = TD->getUnderlyingType();
    
    types[name]["type"] = UnderlyingType.getAsString();
    types[name]["invariants"] = {};

    // Handle struct/enum typedefs
    if (const auto *RT = UnderlyingType->getAs<RecordType>()) {
      RecordDecl *RD = RT->getDecl();
      if (RD->isStruct()) {
        types[name]["type"] = "struct";
        for (const auto *Field : RD->fields()) {
          YAML::Node member;
          member["name"] = Field->getNameAsString();
          member["type"] = Field->getType().getAsString();
          types[name]["members"].push_back(member);
        }
      }
    } else if (const auto *ET = UnderlyingType->getAs<EnumType>()) {
      EnumDecl *ED = ET->getDecl();
      types[name]["type"] = "enum";
      for (const auto *ECD : ED->enumerators()) {
        types[name]["values"][ECD->getNameAsString()] = ECD->getInitVal().getSExtValue();
      }
    }

    // Process Doxygen comments (simplified)
    if (analyze_docs) {
      const RawComment *RC = Context->getRawCommentForDeclNoCache(TD);
      if (RC) {
        std::string CommentText = RC->getFormattedText(*SM, Context->getDiagnostics());
        types[name]["docs"]["raw"] = CommentText;
      }
    }

    return true;
  }
};

// AST Consumer
class YamlyzeASTConsumer : public ASTConsumer {
private:
  YamlyzeVisitor Visitor;

public:
  explicit YamlyzeASTConsumer(ASTContext *Context)
    : Visitor(Context) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }
};

// Frontend Action
class YamlyzeFrontendAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override {
    return std::make_unique<YamlyzeASTConsumer>(&CI.getASTContext());
  }
};

int main(int argc, char **argv) {
  std::string option_file;

  cxxopts::Options options("yamlyze", "Creates a YAML representation of C/C++ source files");
  // clang-format off
  options.add_options()
    ("f,file", "Source/header file", cxxopts::value<std::string>(module_filename))
    ("o,options", "Compile options file", cxxopts::value<std::string>(option_file))
    ("i,includes", "Report included files", cxxopts::value<bool>(analyze_includes)->default_value("false"))
    ("c,calls", "Report function calls", cxxopts::value<bool>(analyze_function_calls)->default_value("false"))
    ("d,docs", "Report Doxygen comments", cxxopts::value<bool>(analyze_docs)->default_value("false"))
    ("a,all", "Analyze all included files", cxxopts::value<bool>(analyze_all_files)->default_value("false"))
    ("H,header", "Process as a header file", cxxopts::value<bool>(process_as_header_file)->default_value("false"))
    ("O,output", "Save output to file", cxxopts::value<std::string>(output_filename)->default_value(""))
    ("h,help", "Print usage");
  // clang-format on

  auto result = options.parse(argc, argv);

  if (result.count("help") || module_filename.empty()) {
    std::cout << options.help() << std::endl;
    exit(0);
  }

  module_filepath = std::filesystem::weakly_canonical(module_filename);

  // Import the compile options
  std::vector<std::string> arg_strings;
  if (!option_file.empty()) {
    std::ifstream file(option_file);

    if (!file.is_open()) {
      std::cerr << "Couldn't open options files\r\n" << option_file << "\r\n";
      exit(1);
    }

    std::string option_data = std::string{ std::istreambuf_iterator<char>{ file }, {} };
    std::stringstream ssin(option_data);
    while (ssin.good()) {
      std::string temp;
      ssin >> temp;
      // Escape strings
      size_t pos = temp.find("\\\"");
      while (pos != std::string::npos) {
        temp.replace(pos, 2, "\"");
        pos = temp.find("\\\"", pos + 1);
      }
      // Ignore empty strings and "troublesome" compile time options
      if ((temp.length() > 0) && (temp.compare("-Werror") != 0))
        arg_strings.push_back(temp);
    }
  }

  // Read the source file
  auto FileOrErr = llvm::MemoryBuffer::getFile(module_filepath.string());
  if (!FileOrErr) {
    std::cerr << "Error: Could not read file: " << module_filepath << "\n";
    return 1;
  }
  
  std::string SourceCode = FileOrErr.get()->getBuffer().str();

  // Run the tool
  std::unique_ptr<ASTUnit> AST = buildASTFromCodeWithArgs(
      SourceCode,
      arg_strings,
      module_filepath.string());

  if (!AST) {
    std::cerr << "Error: Failed to parse the file\n";
    return 1;
  }

  // Visit the AST
  YamlyzeVisitor Visitor(&AST->getASTContext());
  Visitor.TraverseDecl(AST->getASTContext().getTranslationUnitDecl());

  // Use the filename to generate module name
  const std::string module_name = module_filename.substr(module_filename.find_last_of('/') + 1);

  // Create a summary YAML node to hold all the other nodes
  YAML::Node summary;
  summary["name"]      = module_name;
  summary["functions"] = functions;
  summary["variables"] = variables;
  summary["types"]     = types;
  summary["headers"]   = headers;
  
  if (output_filename.empty()) {
    std::cout << summary << std::endl;
  } else {
    const auto output_path = std::filesystem::path(output_filename).parent_path();
    std::filesystem::create_directories(output_path);
    std::ofstream file_out(output_filename);
    file_out << summary;
  }

  return 0;
}

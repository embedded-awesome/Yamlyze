#include "clang-c/Index.h"
#include "clang-c/Documentation.h"
#include "nlohmann/json.hpp"
#include "pugixml.hpp"
#include "yaml-cpp/yaml.h"
#include "cxxopts.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <filesystem>

// #define DEBUG

namespace fs = std::filesystem;
using json   = nlohmann::json;

static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData);
static void include_visitor(CXFile included_file, CXSourceLocation *inclusion_stack, unsigned include_len, CXClientData client_data);

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

int main(int argc, char **argv)
{
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

  if (result.count("help") || module_filename.empty())
  {
    std::cout << options.help() << std::endl;
    exit(0);
  }

  module_filepath = std::filesystem::weakly_canonical(module_filename);

  // Import the compile options as an array of const char*
  std::vector<std::string> arg_strings;
  if (!option_file.empty())
  {
    std::ifstream file(option_file);

    if (!file.is_open())
    {
      std::cerr << "Couldn't open options files\r\n" << option_file << "\r\n";
      exit(1);
    }

    // Read the content of the compile options and split each option into a string
    std::string option_data = std::string{ std::istreambuf_iterator<char>{ file }, {} };
    //std::getline(file, line);
    std::stringstream ssin(option_data);
    while (ssin.good())
    {
      std::string temp;
      ssin >> temp;
      // Escape strings
      size_t pos = temp.find("\\\"");
      while (pos != std::string::npos)
      {
        // Replace this occurrence of Sub String
        temp.replace(pos, 2, "\"");
        // Get the next occurrence from the current position
        pos = temp.find("\\\"", pos + 1);
      }
      // Ignore empty strings and "troublesome" compile time options
      if ((temp.length() > 0) && (temp.compare("-Werror") != 0))
        arg_strings.push_back(temp);
    }
  }

  // Convert the vector of argument strings into an array suitable for command line args
  int argument_count     = 0;
  const char **arguments = new const char *[arg_strings.size()];
  for (const auto &a: arg_strings)
    arguments[argument_count++] = a.data();

  // Create an index with excludeDeclsFromPCH = 1, displayDiagnostics = 0
  CXIndex index = clang_createIndex(1, 0);

  CXTranslationUnit translationUnit = clang_parseTranslationUnit(index, module_filepath.generic_string().c_str(), arguments, argument_count, nullptr, 0, CXTranslationUnit_None);

#if defined(DEBUG)
  for (unsigned int i = 0, n = clang_getNumDiagnostics(translationUnit); i != n; ++i)
  {
    CXDiagnostic Diag = clang_getDiagnostic(translationUnit, i);
    CXString String   = clang_formatDiagnostic(Diag, clang_defaultDiagnosticDisplayOptions());
    std::cerr << clang_getCString(String) << std::endl;
    clang_disposeString(String);
  }
#endif

  // Add the include_visitor if indicated
  if (analyze_includes)
    clang_getInclusions(translationUnit, include_visitor, 0);

  // Visit all the nodes in the AST
  CXCursor cursor = clang_getTranslationUnitCursor(translationUnit);

#if defined(DEBUG)
  auto skipped = clang_getAllSkippedRanges(translationUnit);
  for (int a = 0; a < skipped->count; ++a)
  {
    auto start        = clang_getRangeStart(skipped->ranges[a]);
    auto cursor       = clang_getCursor(translationUnit, start);
    CXCursorKind kind = clang_getCursorKind(cursor);
    std::cout << kind;
  }
#endif

  clang_visitChildren(cursor, visitor, 0);

  // Release memory
  clang_disposeTranslationUnit(translationUnit);
  clang_disposeIndex(index);
  delete[] arguments;

  // Use the filename to generate module name
  const std::string module_name = module_filename.substr(module_filename.find_last_of('/') + 1);

  // Create a summary YAML node to hold all the other nodes
  YAML::Node summary;
  summary["name"]      = module_name;
  summary["functions"] = functions;
  summary["variables"] = variables;
  summary["types"]     = types;
  summary["headers"]   = headers;
  if (output_filename.empty())
  {
    std::cout << summary << std::endl;
  }
  else
  {
    const auto output_path = std::filesystem::path(output_filename).parent_path();
    std::filesystem::create_directories(output_path);
    std::ofstream file_out(output_filename);
    file_out << summary;
  }

  return 0;
}

/*
 * Visitor that prints the names of header files included
 */
static void include_visitor(CXFile included_file, CXSourceLocation *inclusion_stack, unsigned include_len, CXClientData client_data)
{
  // Note: Try use clang_getExpansionLocation(inclusion_stack) or clang_getSpellingLocation(inclusion_stack)
  // Ignore the file with include_len == 0, this is the file that is being parsed
  if (include_len != 0)
  {
    auto file_path = clang_getFileName(included_file);
    auto filename  = fs::path(clang_getCString(file_path)).filename().generic_string();
    YAML::Node new_header;
    new_header["path"]  = clang_getCString(file_path);
    new_header["level"] = include_len;
    headers[filename].push_back(new_header);
    clang_disposeString(file_path);
  }
}

static std::string clean_doxygen_text(pugi::xml_node &&node)
{
  std::string temp = node.text().as_string();
  std::regex regex("\\s*(.*?)(?:\\s|\\*)+");
  std::smatch s;
  if (std::regex_match(temp, s, regex))
    return s[1];
  else
    return temp;
}

static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData)
{
  CXCursorKind kind = clang_getCursorKind(cursor);
  // if (clang_isPreprocessing(kind)) {
  //   std::cout << kind << "\t";
  //   CXCursorKind parent_kind = clang_getCursorKind(parent);
  //   std::cout << parent_kind << "\t";
  //   auto cursorName = clang_getCursorSpelling(cursor);
  //   auto function_name = clang_getCString(cursorName);
  //   std::cout << function_name << "\n";
  //   clang_disposeString(cursorName);
  //   return CXChildVisit_Recurse;
  // } else {
  //   return CXChildVisit_Recurse;
  // }

  // Get the source location
  CXFile file;
  unsigned line;
  unsigned column;
  CXSourceLocation location = clang_getCursorLocation(cursor);
  clang_getFileLocation(location, &file, &line, &column, nullptr);

  if (file != NULL) {
    auto filename      = clang_getFileName(file);
    auto cstr_filename = clang_getCString(filename);

    const auto file_path = std::filesystem::weakly_canonical(cstr_filename);

    // Abort this visit if we have an invalid filename or we don't want to analyze all the files and the we aren't looking at the named file
    if (cstr_filename != nullptr && analyze_all_files == false && module_filepath.compare(file_path) != 0)
    {
      clang_disposeString(filename);
      return CXChildVisit_Continue;
    }
    // Note: We are disposing of filename so cstr_filename is no longer valid
    clang_disposeString(filename);
  }

  // Consider functions and methods
  if (kind == CXCursorKind::CXCursor_FunctionDecl || kind == CXCursorKind::CXCursor_CXXMethod)
  {
    // Check if it's a forward declaration
    // If the definition is null, then there is no definition in this translation
    // unit, so this cursor must be a forward declaration.
    auto definition = clang_getCursorDefinition(cursor);
    if (!process_as_header_file && clang_equalCursors(definition, clang_getNullCursor()))
      return CXChildVisit_Continue;

    // Get function name
    auto cursorName    = clang_getCursorSpelling(cursor);
    auto function_name = clang_getCString(cursorName);

    // Check if it is a static, extern or normal function
    std::string storage_class = "normal";
    switch (clang_Cursor_getStorageClass(cursor))
    {
      case CX_SC_Static: storage_class = "static"; break;
      case CX_SC_Extern: storage_class = "extern"; break;
      default: break;
    }

    // Get the return type
    auto result_type = clang_getTypeSpelling(clang_getResultType(clang_getCursorType(cursor)));

    // Create the function YAML object
    current_function_name                       = function_name;
    functions[current_function_name]["class"]   = storage_class;
    functions[current_function_name]["args"]    = {};
    functions[current_function_name]["calls"]   = {};
    functions[current_function_name]["docs"]    = {};
    functions[current_function_name]["returns"] = clang_getCString(result_type);

    // Iterate through the arguments
    for (int a = 0; a < clang_Cursor_getNumArguments(cursor); ++a)
    {
      auto arg_cursor = clang_Cursor_getArgument(cursor, a);

      CXString arg_name      = clang_getCursorDisplayName(arg_cursor);
      auto arg_type          = clang_getCursorType(arg_cursor);
      CXString arg_type_name = clang_getTypeSpelling(arg_type);
      int arg_size           = clang_Type_getSizeOf(arg_type);

      // Create argument JSON object
      YAML::Node new_arg;
      new_arg["name"] = clang_getCString(arg_name);
      new_arg["type"] = clang_getCString(arg_type_name);
      new_arg["size"] = arg_size;

      functions[current_function_name]["args"].push_back(new_arg);
      clang_disposeString(arg_name);
      clang_disposeString(arg_type_name);
    }

    // Parse Doxygen comments
    // Structure is Function: { Abstract: {Para: ...}, Parameters: [Parameter: {Discussion: {Para: ...}}, ...], ResultDiscussion: {Para: ...} }
    CXComment comment = clang_Cursor_getParsedComment(cursor);
    auto comment_kind = clang_Comment_getKind(comment);
    if (analyze_docs && comment_kind == CXComment_FullComment)
    {
      auto comment_string = clang_FullComment_getAsXML(comment);
      // std::clog << clang_getCString(comment_string) << std::endl;
      pugi::xml_document doc;
      doc.load_string(clang_getCString(comment_string));
      auto function_doxygen = doc.child("Function");
      if (function_doxygen.child("Abstract").child("Para"))
        functions[current_function_name]["docs"]["abstract"] = clean_doxygen_text(function_doxygen.child("Abstract").child("Para"));
      if (function_doxygen.child("ResultDiscussion").child("Para"))
        functions[current_function_name]["docs"]["return"] = clean_doxygen_text(function_doxygen.child("ResultDiscussion").child("Para"));
      for (auto p: function_doxygen.child("Parameters"))
      {
        auto arg_name = p.child("Name").text().as_string();
        for (auto a: functions[current_function_name]["args"])
          if (a["name"].as<std::string>() == arg_name)
          {
            if (p.child("Direction"))
              a["direction"] = p.child("Direction").text().as_string();
            if (p.child("Discussion"))
              a["docs"] = clean_doxygen_text(p.child("Discussion").child("Para"));
          }
      }
      clang_disposeString(comment_string);
    }

    clang_disposeString(cursorName);
    clang_disposeString(result_type);
  }
  else if (analyze_function_calls && kind == CXCursorKind::CXCursor_CallExpr)
  {
    if (!current_function_name.empty())
    {
      auto cursorName = clang_getCursorDisplayName(cursor);
      functions[current_function_name]["calls"].push_back(clang_getCString(cursorName));
      clang_disposeString(cursorName);
    }
  }
  else if (kind == CXCursorKind::CXCursor_VarDecl && clang_Cursor_hasVarDeclGlobalStorage(cursor) != 0)
  {
    auto cursorName        = clang_getCursorDisplayName(cursor);
    auto arg_type          = clang_getCursorType(cursor);
    CXString arg_type_name = clang_getTypeSpelling(arg_type);

    std::string storage_class = "global";
    switch (clang_Cursor_getStorageClass(cursor))
    {
      case CX_SC_Static: storage_class = "static"; break;
      case CX_SC_Extern: storage_class = "extern"; break;
      default: break;
    }
    if (clang_equalCursors(clang_getTranslationUnitCursor(clang_Cursor_getTranslationUnit(cursor)), clang_getCursorLexicalParent(cursor)))
    {
      variables[clang_getCString(cursorName)]["class"] = storage_class;
    }

    variables[clang_getCString(cursorName)]["type"] = clang_getCString(arg_type_name);

    clang_disposeString(cursorName);
    clang_disposeString(arg_type_name);
  }
  else if (kind == CXCursorKind::CXCursor_TypedefDecl)
  {
    // Get the name of the typedef
    auto cursorName  = clang_getCursorDisplayName(cursor);
    std::string name = clang_getCString(cursorName);

    // Get the cursor type
    auto cursor_type = clang_getTypedefDeclUnderlyingType(cursor);
    auto type_name   = clang_getTypeSpelling(cursor_type);

    // Create a new entry in the types
    types[name]["type"]       = clang_getCString(type_name);
    types[name]["invariants"] = {};

    // Inspect the preceding comment block and parse Doxygen fields
    CXComment comment = clang_Cursor_getParsedComment(cursor);
    if (analyze_docs && clang_Comment_getKind(comment) == CXComment_FullComment)
    {
      auto comment_string = clang_FullComment_getAsXML(comment);
      pugi::xml_document doc;
      doc.load_string(clang_getCString(comment_string));
      auto type = doc.child("Typedef");
      for (auto a: type.child("Discussion"))
        if (strncmp(a.attribute("kind").value(), "invariant", 9) == 0)
        {
          try
          {
            const auto node                                = YAML::Load(a.text().as_string());
            types[name]["invariants"][node.begin()->first] = node.begin()->second;
          } catch (...)
          {
            std::cerr << "Could not parse invariant from:" << std::endl << clang_getCString(comment_string) << std::endl;
          }
        }

      clang_disposeString(comment_string);
    }
    else if (clang_Comment_getKind(comment) != CXComment_Null)
    {
      //std::cerr << "Found comment of type: " << clang_Comment_getKind(comment) << std::endl;
    }

    // If the type is a elaborate type continue parsing
    if (cursor_type.kind == CXType_Elaborated)
    {
      YAML::Node temp_node = types[name];
      clang_visitChildren(
        cursor,
        [](CXCursor c, CXCursor parent, CXClientData client_data) {
          YAML::Node *node      = reinterpret_cast<YAML::Node *>(client_data);
          CXCursorKind sub_kind = clang_getCursorKind(c);
          auto sub_cursor_name  = clang_getCursorDisplayName(c);
          switch (sub_kind)
          {
            case CXCursor_StructDecl: (*node)["type"] = "struct"; break;
            case CXCursor_EnumDecl: (*node)["type"] = "enum"; break;
            case CXCursor_EnumConstantDecl: (*node)["values"][clang_getCString(sub_cursor_name)] = clang_getEnumConstantDeclValue(c); break;
            case CXCursor_FieldDecl: {
              auto sub_type = clang_getTypeSpelling(clang_getCursorType(c));
              YAML::Node member;
              member["name"] = clang_getCString(sub_cursor_name);
              member["type"] = clang_getCString(sub_type);
              (*node)["members"].push_back(member);
              clang_disposeString(sub_type);
              break;
            }

            default: break;
          }
          clang_disposeString(sub_cursor_name);
          return CXChildVisit_Recurse;
        },
        &temp_node);
    }

    // Clean up strings
    clang_disposeString(cursorName);
    clang_disposeString(type_name);

    return CXChildVisit_Continue;
  }
  else if (kind == CXCursorKind::CXCursor_Namespace || kind == CXCursorKind::CXCursor_ClassDecl)
  {
    return CXChildVisit_Recurse;
  }
  return CXChildVisit_Recurse;
}

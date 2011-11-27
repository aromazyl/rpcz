// Copyright 2011, Nadav Samet.
// All rights reserved.
//
// Author: thesamet@gmail.com <Nadav Samet>

#include "zrpc/plugin/cpp/cpp_helpers.h"
#include "zrpc/plugin/cpp/file_generator.h"
#include "zrpc/plugin/cpp/zrpc_cpp_service.h"
#include "zrpc/plugin/common/strutil.h"
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/printer.h>

namespace zrpc {
namespace plugin {
namespace cpp {

using std::string;
using namespace ::google::protobuf;
using namespace google::protobuf::compiler::cpp;

FileGenerator::FileGenerator(const FileDescriptor* file,
                             const string& dllexport_decl)
    : file_(file), dllexport_decl_(dllexport_decl) {
  SplitStringUsing(file_->package(), ".", &package_parts_);
  for (int i = 0; i < file->service_count(); i++) {
    service_generators_.push_back(
      new ServiceGenerator(file->service(i), dllexport_decl));
  }
}

FileGenerator::~FileGenerator() {
  for (int i = 0; i < service_generators_.size(); i++) {
    delete service_generators_[i];
  }
}

void FileGenerator::GenerateHeader(io::Printer* printer) {
  string filename_identifier = FilenameIdentifier(file_->name());

  printer->Print(
    "// Generated by the protocol buffer compiler.  DO NOT EDIT!\n"
    "// source: $filename$\n"
    "\n"
    "#ifndef ZRPC_$filename_identifier$__INCLUDED\n"
    "#define ZRPC_$filename_identifier$__INCLUDED\n"
    "\n"
    "#include <string>\n"
    "#include <zrpc/service.h>\n"
    "\n"
    "namespace google {\n"
    "namespace protobuf {\n"
    "class ServiceDescriptor;\n"
    "class MethodDescriptor;\n"
    "class RpcChannel;\n"      // TODO: remove this
    "}  // namespace protobuf\n"
    "}  // namespace google\n"
    "namespace zrpc {\n"
    "class RPC;\n"
    "}  //namesacpe zrpc\n"
    ,
    "filename", file_->name(),
    "filename_identifier", filename_identifier);

  for (int i = 0; i < file_->dependency_count(); i++) {
    printer->Print(
      "#include \"$dependency$.pb.h\"\n",
      "dependency", StripProto(file_->dependency(i)->name()));
  }

  printer->Print(
      "#include \"$dependency$.pb.h\"\n",
      "dependency", StripProto(file_->name()));

  GenerateNamespaceOpeners(printer);

  // Forward-declare the AddDescriptors, AssignDescriptors, and ShutdownFile
  // functions, so that we can declare them to be friends of each class.
  printer->Print(
    "\n"
    "// Internal implementation detail -- do not call these.\n"
    "void $dllexport_decl$ zrpc_$adddescriptorsname$();\n",
    "adddescriptorsname", GlobalAddDescriptorsName(file_->name()),
    "dllexport_decl", dllexport_decl_);

  printer->Print(
    // Note that we don't put dllexport_decl on these because they are only
    // called by the .pb.cc file in which they are defined.
    "void zrpc_$assigndescriptorsname$();\n"
    "void zrpc_$shutdownfilename$();\n"
    "\n",
    "assigndescriptorsname", GlobalAssignDescriptorsName(file_->name()),
    "shutdownfilename", GlobalShutdownFileName(file_->name()));
  for (int i = 0; i < file_->service_count(); i++) {
    service_generators_[i]->GenerateDeclarations(printer);
  }

  GenerateNamespaceClosers(printer);
  printer->Print(
    "#endif  // ZRPC_$filename_identifier$__INCLUDED\n",
    "filename_identifier", filename_identifier);
}

void FileGenerator::GenerateSource(io::Printer* printer) {
  printer->Print(
    "// Generated by the protocol buffer compiler.  DO NOT EDIT!\n"
    "\n"
    "#include \"$basename$.zrpc.h\"\n"
    "#include \"$basename$.pb.h\"\n"
    "#include <google/protobuf/descriptor.h>\n"
    "#include <google/protobuf/stubs/once.h>\n"
    "#include <zrpc/rpc.h>\n"
    "namespace {\n",
    "basename", StripProto(file_->name()));

  for (int i = 0; i < file_->service_count(); i++) {
    printer->Print(
      "const ::google::protobuf::ServiceDescriptor* $name$_descriptor_ = NULL;\n",
      "name", file_->service(i)->name());
  }
  printer->Print(
      "}  // anonymouse namespace\n");

  GenerateNamespaceOpeners(printer);
  GenerateBuildDescriptors(printer);

  for (int i = 0; i < file_->service_count(); i++) {
    if (i == 0) printer->Print("\n");
    printer->Print(kThickSeparator);
    printer->Print("\n");
    service_generators_[i]->GenerateImplementation(printer);
  }
  GenerateNamespaceClosers(printer);
}

void FileGenerator::GenerateNamespaceOpeners(io::Printer* printer) {
  if (package_parts_.size() > 0) printer->Print("\n");

  for (int i = 0; i < package_parts_.size(); i++) {
    printer->Print("namespace $part$ {\n",
                   "part", package_parts_[i]);
  }
}

void FileGenerator::GenerateNamespaceClosers(io::Printer* printer) {
  if (package_parts_.size() > 0) printer->Print("\n");

  for (int i = package_parts_.size() - 1; i >= 0; i--) {
    printer->Print("}  // namespace $part$\n",
                   "part", package_parts_[i]);
  }
}

void FileGenerator::GenerateBuildDescriptors(io::Printer* printer) {
  // AddDescriptors() is a file-level procedure which adds the encoded
  // FileDescriptorProto for this .proto file to the global DescriptorPool
  // for generated files (DescriptorPool::generated_pool()).  It always runs
  // at static initialization time, so all files will be registered before
  // main() starts.  This procedure also constructs default instances and
  // registers extensions.
  //
  // Its sibling, AssignDescriptors(), actually pulls the compiled
  // FileDescriptor from the DescriptorPool and uses it to populate all of
  // the global variables which store pointers to the descriptor objects.
  // It also constructs the reflection objects.  It is called the first time
  // anyone calls descriptor() or GetReflection() on one of the types defined
  // in the file.

  // In optimize_for = LITE_RUNTIME mode, we don't generate AssignDescriptors()
  // and we only use AddDescriptors() to allocate default instances.
  if (HasDescriptorMethods(file_)) {
    printer->Print(
      "\n"
      "void zrpc_$assigndescriptorsname$() {\n",
      "assigndescriptorsname", GlobalAssignDescriptorsName(file_->name()));
    printer->Indent();

    // Make sure the file has found its way into the pool.  If a descriptor
    // is requested *during* static init then AddDescriptors() may not have
    // been called yet, so we call it manually.  Note that it's fine if
    // AddDescriptors() is called multiple times.
    printer->Print(
      "zrpc_$adddescriptorsname$();\n",
      "adddescriptorsname", GlobalAddDescriptorsName(file_->name()));

    // Get the file's descriptor from the pool.
    printer->Print(
      "const ::google::protobuf::FileDescriptor* file =\n"
      "  ::google::protobuf::DescriptorPool::generated_pool()->FindFileByName(\n"
      "    \"$filename$\");\n"
      // Note that this GOOGLE_CHECK is necessary to prevent a warning about "file"
      // being unused when compiling an empty .proto file.
      "GOOGLE_CHECK(file != NULL);\n",
      "filename", file_->name());

    // Go through all the stuff defined in this file and generated code to
    // assign the global descriptor pointers based on the file descriptor.
    for (int i = 0; i < file_->service_count(); i++) {
      service_generators_[i]->GenerateDescriptorInitializer(printer, i);
    }

    printer->Outdent();
    printer->Print(
      "}\n"
      "\n");

    // ---------------------------------------------------------------

    // protobuf_AssignDescriptorsOnce():  The first time it is called, calls
    // AssignDescriptors().  All later times, waits for the first call to
    // complete and then returns.
    printer->Print(
      "namespace {\n"
      "\n"
      "GOOGLE_PROTOBUF_DECLARE_ONCE(protobuf_AssignDescriptors_once_);\n"
      "inline void protobuf_AssignDescriptorsOnce() {\n"
      "  ::google::protobuf::GoogleOnceInit(&protobuf_AssignDescriptors_once_,\n"
      "                 &zrpc_$assigndescriptorsname$);\n"
      "}\n"
      "\n",
      "assigndescriptorsname", GlobalAssignDescriptorsName(file_->name()));

    // protobuf_RegisterTypes():  Calls
    // MessageFactory::InternalRegisterGeneratedType() for each message type.
    printer->Print(
      "void protobuf_RegisterTypes(const ::std::string&) {\n"
      "  protobuf_AssignDescriptorsOnce();\n");
    printer->Indent();

    printer->Outdent();
    printer->Print(
      "}\n"
      "\n"
      "}  // namespace\n");
  }

  // -----------------------------------------------------------------

  // ShutdownFile():  Deletes descriptors, default instances, etc. on shutdown.
  printer->Print(
    "\n"
    "void zrpc_$shutdownfilename$() {\n",
    "shutdownfilename", GlobalShutdownFileName(file_->name()));
  printer->Indent();

  printer->Outdent();
  printer->Print(
    "}\n");

  // -----------------------------------------------------------------

  // Now generate the AddDescriptors() function.
  printer->Print(
    "\n"
    "void zrpc_$adddescriptorsname$() {\n"
    // We don't need any special synchronization here because this code is
    // called at static init time before any threads exist.
    "  static bool already_here = false;\n"
    "  if (already_here) return;\n"
    "  already_here = true;\n"
    "  GOOGLE_PROTOBUF_VERIFY_VERSION;\n"
    "\n",
    "adddescriptorsname", GlobalAddDescriptorsName(file_->name()));
  printer->Indent();

  // Call the AddDescriptors() methods for all of our dependencies, to make
  // sure they get added first.
  for (int i = 0; i < file_->dependency_count(); i++) {
    const FileDescriptor* dependency = file_->dependency(i);
    // Print the namespace prefix for the dependency.
    vector<string> dependency_package_parts;
    SplitStringUsing(dependency->package(), ".", &dependency_package_parts);
    printer->Print("::");
    for (int i = 0; i < dependency_package_parts.size(); i++) {
      printer->Print("$name$::",
                     "name", dependency_package_parts[i]);
    }
    // Call its AddDescriptors function.
    printer->Print(
      "$name$();\n",
      "name", GlobalAddDescriptorsName(dependency->name()));
  }

  if (HasDescriptorMethods(file_)) {
    // Embed the descriptor.  We simply serialize the entire FileDescriptorProto
    // and embed it as a string literal, which is parsed and built into real
    // descriptors at initialization time.
    FileDescriptorProto file_proto;
    file_->CopyTo(&file_proto);
    string file_data;
    file_proto.SerializeToString(&file_data);

    printer->Print(
      "::google::protobuf::DescriptorPool::InternalAddGeneratedFile(");

    // Only write 40 bytes per line.
    static const int kBytesPerLine = 40;
    for (int i = 0; i < file_data.size(); i += kBytesPerLine) {
      printer->Print("\n  \"$data$\"",
        "data", EscapeTrigraphs(CEscape(file_data.substr(i, kBytesPerLine))));
    }
    printer->Print(
      ", $size$);\n",
      "size", SimpleItoa(file_data.size()));

    // Call MessageFactory::InternalRegisterGeneratedFile().
    printer->Print(
      "::google::protobuf::MessageFactory::InternalRegisterGeneratedFile(\n"
      "  \"$filename$\", &protobuf_RegisterTypes);\n",
      "filename", file_->name());
  }

  printer->Print(
    "::google::protobuf::internal::OnShutdown(&zrpc_$shutdownfilename$);\n",
    "shutdownfilename", GlobalShutdownFileName(file_->name()));

  printer->Outdent();

  printer->Print(
    "}\n"
    "\n"
    "// Force AddDescriptors() to be called at static initialization time.\n"
    "struct zrpc_StaticDescriptorInitializer_$filename$ {\n"
    "  zrpc_StaticDescriptorInitializer_$filename$() {\n"
    "    zrpc_$adddescriptorsname$();\n"
    "  }\n"
    "} zrpc_static_descriptor_initializer_$filename$_;\n"
    "\n",
    "adddescriptorsname", GlobalAddDescriptorsName(file_->name()),
    "filename", FilenameIdentifier(file_->name()));
}


}  // namespace cpp
}  // namespace plugin
}  // namespace zrpc
